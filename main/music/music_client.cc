#include "music_client.h"

#include "application.h"
#include "board.h"
#include "music_frame.h"
#include "settings.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include <algorithm>

#define TAG "MusicClient"

namespace {
std::string MusicUrl(std::string url) {
    if (url.rfind("https://", 0) == 0) url.replace(0, 8, "wss://");
    else if (url.rfind("http://", 0) == 0) url.replace(0, 7, "ws://");
    const auto position = url.find("/ydp/v1/device/ws");
    if (position != std::string::npos) url.replace(position, std::string("/ydp/v1/device/ws").size(), "/ydp/v1/music/ws");
    return url;
}
}  // namespace

MusicClient::MusicClient() {
    ReloadCredentials();
    player_.OnState([this](uint32_t epoch, const char* state) { ReportPlaybackState(state, epoch); });
}

MusicClient::~MusicClient() { Stop(); }

void MusicClient::Start() {
    std::lock_guard<std::mutex> task_lock(task_mutex_);
    if (task_ != nullptr) {
        running_ = true;
        xTaskNotifyGive(task_);
        return;
    }
    running_ = true;
    SetUiState(MusicUiState::kConnecting);
    if (xTaskCreateWithCaps(Run, "music_ws", 8192, this, 2, &task_,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        task_ = nullptr;
        running_ = false;
        ESP_LOGE(TAG, "Unable to allocate independent music network worker");
    }
}

void MusicClient::Stop() {
    running_ = false;
    {
        std::lock_guard<std::mutex> task_lock(task_mutex_);
        if (task_ != nullptr) xTaskNotifyGive(task_);
    }
    player_.Stop();
    std::unique_ptr<WebSocket> websocket;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        websocket = std::move(websocket_);
    }
    websocket.reset();
}

bool MusicClient::Send(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    return connected_ && websocket_ && websocket_->Send(text);
}

void MusicClient::ReloadCredentials() {
    Settings settings("management", false);
    url_ = MusicUrl(settings.GetString("url"));
    token_ = settings.GetString("token");
}

bool MusicClient::Select(size_t index) {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= tracks_.size()) return false;
        selected_index_ = index;
        id = tracks_[index].id;
    }
    return Send("{\"type\":\"music.playlist.select\",\"trackId\":\"" + id + "\"}");
}

bool MusicClient::PlaySelected() {
    std::string id;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (tracks_.empty()) return false;
        id = tracks_[selected_index_].id;
    }
    const bool sent = Send("{\"type\":\"music.playback.request\",\"trackId\":\"" + id + "\"}");
    if (sent) SetUiState(MusicUiState::kBuffering);
    return sent;
}

bool MusicClient::TogglePlayback() {
    const auto state = ui_state_.load();
    if (state == MusicUiState::kPlaying || state == MusicUiState::kBuffering)
        return Send("{\"type\":\"music.playback.pause\"}");
    if (state == MusicUiState::kPaused) return Send("{\"type\":\"music.playback.resume\"}");
    return PlaySelected();
}

bool MusicClient::Seek(uint32_t position_ms) {
    const auto state = ui_state_.load();
    if (state != MusicUiState::kPlaying && state != MusicUiState::kPaused &&
        state != MusicUiState::kBuffering) return false;
    stream_epoch_ = 0;
    player_.Stop();
    SetUiState(MusicUiState::kBuffering);
    const bool sent = Send("{\"type\":\"music.playback.seek\",\"positionMs\":" +
                           std::to_string(position_ms) + "}");
    if (!sent) SetUiState(MusicUiState::kFailed);
    return sent;
}

bool MusicClient::StopPlayback() {
    const bool sent = Send("{\"type\":\"music.playback.stop\",\"reason\":\"user\"}");
    player_.Stop();
    if (sent) SetUiState(MusicUiState::kReady);
    return sent;
}

void MusicClient::SetUiState(MusicUiState state) {
    if (ui_state_.exchange(state) != state && on_changed_) on_changed_();
}

void MusicClient::SetBusy(bool busy) {
    const bool known = availability_known_.exchange(true);
    const bool previous = busy_.exchange(busy);
    if (known && previous == busy) return;
    if (busy) {
        const auto active_epoch = stream_epoch_.exchange(0);
        if (active_epoch != 0) ReportPlaybackState("STOPPED");
        player_.Stop();
        if (active_epoch != 0) Send("{\"type\":\"music.playback.stop\",\"reason\":\"voice_busy\"}");
    }
    Send(std::string("{\"type\":\"music.availability\",\"busy\":") + (busy ? "true}" : "false}"));
}

std::vector<MusicTrackInfo> MusicClient::GetTracks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tracks_;
}

void MusicClient::HandleText(const char* data, size_t size) {
    cJSON* root = cJSON_ParseWithLength(data, size);
    auto* type = root ? cJSON_GetObjectItem(root, "type") : nullptr;
    if (!cJSON_IsString(type)) { cJSON_Delete(root); return; }
    if (strcmp(type->valuestring, "music.hello") == 0 || strcmp(type->valuestring, "music.playlist.replace") == 0) {
        auto* revision = cJSON_GetObjectItem(root, "revision");
        auto* items = cJSON_GetObjectItem(root, "items");
        std::vector<MusicTrackInfo> parsed;
        if (cJSON_IsArray(items)) {
            cJSON* item = nullptr;
            cJSON_ArrayForEach(item, items) {
                auto* id = cJSON_GetObjectItem(item, "trackId");
                auto* title = cJSON_GetObjectItem(item, "title");
                auto* artist = cJSON_GetObjectItem(item, "artist");
                if (cJSON_IsString(id) && cJSON_IsString(title)) {
                    auto* cover = cJSON_GetObjectItem(item, "coverUrl");
                    auto* duration = cJSON_GetObjectItem(item, "durationMs");
                    parsed.push_back({id->valuestring, title->valuestring,
                        cJSON_IsString(artist) ? artist->valuestring : "",
                        cJSON_IsString(cover) ? cover->valuestring : "",
                        cJSON_IsNumber(duration) ? static_cast<uint32_t>(duration->valuedouble) : 0});
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tracks_ = std::move(parsed);
            if (selected_index_ >= tracks_.size()) selected_index_ = 0;
            ESP_LOGI(TAG, "Playlist received: revision=%d, tracks=%u",
                     cJSON_IsNumber(revision) ? revision->valueint : 0,
                     static_cast<unsigned>(tracks_.size()));
        }
        if (on_playlist_) on_playlist_();
        SetUiState(MusicUiState::kReady);
        if (cJSON_IsNumber(revision)) {
            Send("{\"type\":\"music.playlist.applied\",\"revision\":" + std::to_string(revision->valueint) + "}");
        }
    } else if (strcmp(type->valuestring, "music.playback.started") == 0) {
        auto* epoch = cJSON_GetObjectItem(root, "streamEpoch");
        auto* duration = cJSON_GetObjectItem(root, "frameDurationMs");
        auto* playback = cJSON_GetObjectItem(root, "playbackId");
        auto* position = cJSON_GetObjectItem(root, "positionMs");
        if (cJSON_IsNumber(epoch)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (cJSON_IsString(playback)) playback_id_ = playback->valuestring;
                stream_epoch_ = static_cast<uint32_t>(epoch->valuedouble);
                expected_sequence_ = 0;
                position_ms_ = cJSON_IsNumber(position) ? static_cast<uint32_t>(position->valuedouble) : 0;
            }
            ESP_LOGI(TAG, "Playback started: id=%s, epoch=%lu",
                     cJSON_IsString(playback) ? playback->valuestring : "",
                     static_cast<unsigned long>(stream_epoch_.load()));
            SetUiState(MusicUiState::kBuffering);
            if (busy_.load()) {
                ReportPlaybackState("STOPPED");
                stream_epoch_ = 0;
                Send("{\"type\":\"music.playback.stop\",\"reason\":\"voice_busy\"}");
            } else {
                player_.Begin(stream_epoch_.load(), cJSON_IsNumber(duration) ? duration->valueint : 60);
            }
        }
    } else if (strcmp(type->valuestring, "music.playback.stopped") == 0) {
        player_.Stop();
        stream_epoch_ = 0;
        expected_sequence_ = 0;
        SetUiState(MusicUiState::kReady);
    } else if (strcmp(type->valuestring, "music.playback.paused") == 0) {
        player_.Pause();
        SetUiState(MusicUiState::kPaused);
    } else if (strcmp(type->valuestring, "music.playback.resumed") == 0) {
        player_.Resume();
        SetUiState(MusicUiState::kPlaying);
    } else if (strcmp(type->valuestring, "music.error") == 0) {
        auto* code = cJSON_GetObjectItem(root, "code");
        const bool confirmation_failed = (cJSON_IsNumber(code) && code->valueint == 17318) ||
            (cJSON_IsString(code) && strcmp(code->valuestring, "STATE_CONFIRM_FAILED") == 0);
        if (!confirmation_failed) {
            SetUiState(MusicUiState::kFailed);
            ReportPlaybackState("FAILED");
            player_.Stop();
            stream_epoch_ = 0;
            expected_sequence_ = 0;
        }
    }
    cJSON_Delete(root);
}

void MusicClient::HandleBinary(const uint8_t* data, size_t size) {
    MusicFrameView frame{};
    const auto epoch = stream_epoch_.load();
    if (!ParseMusicFrame(data, size, epoch, frame) || frame.epoch != epoch) return;
    auto expected = expected_sequence_.load();
    if (frame.sequence != expected || !expected_sequence_.compare_exchange_strong(expected, expected + 1)) {
        ReportPlaybackState("FAILED");
        Send("{\"type\":\"music.playback.stop\",\"reason\":\"sequence_gap\"}");
        stream_epoch_ = 0;
        expected_sequence_ = 0;
        player_.Stop();
        return;
    }
    const bool accepted = frame.end ? player_.FeedEnd() : player_.Feed(frame.payload, frame.payload_size);
    if (!accepted) {
        ReportPlaybackState("FAILED");
        Send("{\"type\":\"music.playback.stop\",\"reason\":\"buffer_failed\"}");
        stream_epoch_ = 0;
        expected_sequence_ = 0;
        player_.Stop();
    }
}
void MusicClient::ReportPlaybackState(const char* state, uint32_t expected_epoch) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (expected_epoch && expected_epoch != stream_epoch_.load()) return;
        if (strcmp(state, "PLAYING") == 0 && ui_state_.load() == MusicUiState::kPaused) return;
        auto target = ui_state_.load();
        const bool terminal = strcmp(state, "COMPLETED") == 0 || strcmp(state, "STOPPED") == 0 || strcmp(state, "FAILED") == 0;
        if (strcmp(state, "PLAYING") == 0) target = MusicUiState::kPlaying;
        else if (strcmp(state, "BUFFERING") == 0) target = MusicUiState::kBuffering;
        else if (strcmp(state, "FAILED") == 0) target = MusicUiState::kFailed;
        else if (terminal) target = MusicUiState::kReady;
        changed = ui_state_.exchange(target) != target;
        // 当前云端状态接口不接受 BUFFERING；仅更新设备展示。
        if (strcmp(state, "BUFFERING") != 0 && !playback_id_.empty() && connected_ && websocket_) {
            websocket_->Send("{\"type\":\"music.playback.state\",\"playbackId\":\"" + playback_id_ + "\",\"status\":\"" + state + "\"}");
        }
        if (terminal) {
            stream_epoch_ = 0;
            expected_sequence_ = 0;
            playback_id_.clear();
        }
    }
    ESP_LOGI(TAG, "Playback state: %s, position=%lu ms", state, static_cast<unsigned long>(GetPositionMs()));
    if (changed && on_changed_) on_changed_();
}

void MusicClient::Run(void* arg) {
    auto* self = static_cast<MusicClient*>(arg);
    int retry = 2;
    for (;;) {
      while (self->running_) {
        if (self->url_.empty() || self->token_.empty()) {
            self->ReloadCredentials();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));
            continue;
        }
        auto websocket = Board::GetInstance().GetNetwork()->CreateWebSocket(2);
        websocket->SetHeader("Authorization", ("Bearer " + self->token_).c_str());
        websocket->SetHeader("Protocol-Version", "1");
        websocket->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        websocket->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        websocket->OnData([self](const char* data, size_t size, bool binary) {
            if (binary) self->HandleBinary(reinterpret_cast<const uint8_t*>(data), size);
            else self->HandleText(data, size);
        });
        websocket->OnDisconnected([self]() {
            self->player_.Stop();
            self->SetUiState(MusicUiState::kDisconnected);
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->connected_ = false;
        });
        if (!websocket->Connect(self->url_.c_str())) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(retry * 1000));
            retry = std::min(retry * 2, 60);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->websocket_ = std::move(websocket);
            self->connected_ = true;
        }
        self->SetUiState(MusicUiState::kReady);
        retry = 2;
        self->Send("{\"type\":\"music.hello\"}");
        self->availability_known_ = false;
        self->SetBusy(Application::GetInstance().GetDeviceState() != kDeviceStateIdle);
        auto heartbeat_at = xTaskGetTickCount();
        while (self->running_) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
            if (!self->running_) break;
            self->player_.DispatchState();
            if (xTaskGetTickCount() - heartbeat_at >= pdMS_TO_TICKS(30000)) {
                if (!self->Send("{\"type\":\"heartbeat\"}")) break;
                heartbeat_at = xTaskGetTickCount();
            }
        }
        std::unique_ptr<WebSocket> old;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            old = std::move(self->websocket_);
            self->connected_ = false;
        }
        old.reset();
      }
      std::lock_guard<std::mutex> task_lock(self->task_mutex_);
      if (self->running_) continue;
      self->task_ = nullptr;
      break;
    }
    vTaskDeleteWithCaps(nullptr);
}
