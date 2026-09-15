#include "music_player.h"

#include "application.h"
#include "audio/demuxer/ogg_demuxer.h"
#include "board.h"

#include <esp_audio_dec.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_opus_dec.h>

#include <vector>
#include <cstring>

#define TAG "MusicPlayer"

namespace {
esp_opus_dec_frame_duration_t FrameDuration(int milliseconds) {
    switch (milliseconds) {
        case 5: return ESP_OPUS_DEC_FRAME_DURATION_5_MS;
        case 10: return ESP_OPUS_DEC_FRAME_DURATION_10_MS;
        case 20: return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        case 40: return ESP_OPUS_DEC_FRAME_DURATION_40_MS;
        case 60: return ESP_OPUS_DEC_FRAME_DURATION_60_MS;
        default: return ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    }
}
}  // namespace

namespace {
struct MusicChunk {
    uint32_t epoch;
    size_t size;
    bool end;
    uint8_t data[4096];
};
struct PcmFrame {
    uint32_t epoch;
    uint32_t samples;
    bool end;
};
struct PlaybackState {
    uint32_t epoch;
    const char* state;
};
constexpr int kPrefillMs = 1500;
}

MusicPlayer::MusicPlayer() : demuxer_(std::make_unique<OggDemuxer>()) {
    demuxer_->OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t size) {
        Decode(data, sample_rate, size);
    });
    queue_ = xQueueCreate(8, sizeof(MusicChunk*));
    state_queue_ = xQueueCreate(1, sizeof(PlaybackState));
    // ESP-IDF 原生环形缓冲，预留 3 秒单声道 PCM 和帧头；不缓存整曲。
    pcm_buffer_ = xRingbufferCreateWithCaps(
        Board::GetInstance().GetAudioCodec()->output_sample_rate() * 2 * 3 + 4096,
        RINGBUF_TYPE_NOSPLIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (queue_ && state_queue_ && pcm_buffer_) {
        decode_stopped_ = false;
        if (xTaskCreateWithCaps(Run, "music_decode", 12288, this, 3, &task_,
                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) decode_stopped_ = true;
        if (task_) {
            output_stopped_ = false;
            if (xTaskCreateWithCaps(Output, "music_output", 4096, this, 4, &output_task_,
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) output_stopped_ = true;
        }
    }
    if (task_ == nullptr || output_task_ == nullptr) {
        ESP_LOGE(TAG, "Unable to allocate independent music worker");
        running_ = false;
        return;
    }
    available_ = true;
}

MusicPlayer::~MusicPlayer() {
    available_ = false;
    running_ = false;
    Stop();
    while (!decode_stopped_ || !output_stopped_) vTaskDelay(pdMS_TO_TICKS(10));
    if (queue_) vQueueDelete(queue_);
    if (state_queue_) vQueueDelete(state_queue_);
    if (pcm_buffer_) vRingbufferDeleteWithCaps(pcm_buffer_);
}

void MusicPlayer::Begin(uint32_t epoch, int frame_duration_ms) {
    if (!available_) {
        if (on_state_) on_state_(epoch, "FAILED");
        return;
    }
    Stop();
    requested_frame_duration_ms_ = frame_duration_ms;
    queued_end_epoch_ = 0;
    played_ms_ = 0;
    active_epoch_ = epoch;
}

bool MusicPlayer::Feed(const uint8_t* data, size_t size) {
    const auto epoch = active_epoch_.load();
    if (!available_ || epoch == 0 || data == nullptr || size == 0 || size > sizeof(MusicChunk::data)) return false;
    auto* chunk = static_cast<MusicChunk*>(
        heap_caps_malloc(sizeof(MusicChunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!chunk) return false;
    *chunk = MusicChunk{.epoch = epoch, .size = size, .end = false};
    std::memcpy(chunk->data, data, size);
    if (xQueueSend(queue_, &chunk, pdMS_TO_TICKS(500)) == pdTRUE) return true;
    heap_caps_free(chunk);
    return false;
}
bool MusicPlayer::FeedEnd() {
    const auto epoch = active_epoch_.load();
    if (!available_ || epoch == 0) return false;
    auto* chunk = static_cast<MusicChunk*>(
        heap_caps_malloc(sizeof(MusicChunk), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (chunk) *chunk = MusicChunk{.epoch = epoch, .size = 0, .end = true};
    if (chunk && xQueueSend(queue_, &chunk, pdMS_TO_TICKS(1000)) == pdTRUE) return true;
    heap_caps_free(chunk);
    return false;
}

void MusicPlayer::Run(void* arg) {
    auto* self = static_cast<MusicPlayer*>(arg);
    while (self->running_) {
        MusicChunk* chunk = nullptr;
        if (xQueueReceive(self->queue_, &chunk, pdMS_TO_TICKS(20)) != pdTRUE || chunk == nullptr) continue;
        if (chunk->epoch != self->active_epoch_.load() ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            heap_caps_free(chunk);
            continue;
        }
        if (chunk->end) {
            PcmFrame* frame = nullptr;
            while (self->running_ && chunk->epoch == self->active_epoch_.load()) {
                if (xRingbufferSendAcquire(self->pcm_buffer_, reinterpret_cast<void**>(&frame),
                                           sizeof(PcmFrame), pdMS_TO_TICKS(20)) == pdTRUE) {
                    *frame = {chunk->epoch, 0, true};
                    self->queued_end_epoch_ = chunk->epoch;
                    xRingbufferSendComplete(self->pcm_buffer_, frame);
                    break;
                }
            }
            heap_caps_free(chunk);
            continue;
        }
        if (chunk->epoch != self->active_epoch_.load()) {
            heap_caps_free(chunk);
            continue;
        }
        if (self->decoder_epoch_ != chunk->epoch) {
            self->ResetDecoder();
            self->decoder_epoch_ = chunk->epoch;
            self->frame_duration_ms_ = self->requested_frame_duration_ms_.load();
            self->demuxer_->Reset();
        }
        self->demuxer_->Process(chunk->data, chunk->size);
        heap_caps_free(chunk);
    }
    self->ResetDecoder();
    self->decode_stopped_ = true;
    vTaskDeleteWithCaps(nullptr);
}

void MusicPlayer::Decode(const uint8_t* data, int sample_rate, size_t size) {
    const auto epoch = decoder_epoch_;
    if (!running_ || epoch == 0 || epoch != active_epoch_.load() ||
        Application::GetInstance().GetDeviceState() != kDeviceStateIdle) return;
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (sample_rate != codec->output_sample_rate()) {
        ESP_LOGE(TAG, "Unsupported music sample rate %d, codec=%d", sample_rate, codec->output_sample_rate());
        Fail(epoch);
        return;
    }
    if (decoder_ == nullptr || sample_rate_ != sample_rate) {
        if (decoder_ != nullptr) esp_opus_dec_close(decoder_);
        esp_opus_dec_cfg_t config = {
            .sample_rate = static_cast<uint32_t>(sample_rate),
            .channel = ESP_AUDIO_MONO,
            .frame_duration = FrameDuration(frame_duration_ms_),
            .self_delimited = false,
        };
        if (esp_opus_dec_open(&config, sizeof(config), &decoder_) != ESP_AUDIO_ERR_OK || decoder_ == nullptr) {
            ESP_LOGE(TAG, "Unable to open independent music decoder");
            Fail(epoch);
            return;
        }
        sample_rate_ = sample_rate;
    }
    const size_t pcm_bytes = sample_rate * frame_duration_ms_ / 1000 * sizeof(int16_t);
    PcmFrame* frame = nullptr;
    while (running_ && epoch == active_epoch_.load()) {
        if (xRingbufferSendAcquire(pcm_buffer_, reinterpret_cast<void**>(&frame),
                                   sizeof(PcmFrame) + pcm_bytes, pdMS_TO_TICKS(20)) == pdTRUE) break;
    }
    if (!frame) return;
    *frame = {epoch, 0, false};
    esp_audio_dec_in_raw_t input = {
        .buffer = const_cast<uint8_t*>(data), .len = static_cast<uint32_t>(size),
        .consumed = 0, .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
    };
    esp_audio_dec_out_frame_t output = {
        .buffer = reinterpret_cast<uint8_t*>(frame + 1),
        .len = static_cast<uint32_t>(pcm_bytes),
        .decoded_size = 0,
    };
    esp_audio_dec_info_t info = {};
    if (esp_opus_dec_decode(decoder_, &input, &output, &info) != ESP_AUDIO_ERR_OK || output.decoded_size == 0) {
        xRingbufferSendComplete(pcm_buffer_, frame);
        Fail(epoch);
        return;
    }
    frame->samples = output.decoded_size / sizeof(int16_t);
    buffered_samples_.fetch_add(frame->samples);
    xRingbufferSendComplete(pcm_buffer_, frame);
}

void MusicPlayer::Output(void* arg) {
    auto* self = static_cast<MusicPlayer*>(arg);
    auto* codec = Board::GetInstance().GetAudioCodec();
    std::vector<int16_t> pcm;
    pcm.reserve(codec->output_sample_rate() * 60 / 1000);
    uint32_t output_epoch = 0;
    bool prefill = true;
    while (self->running_) {
        size_t size = 0;
        auto* frame = static_cast<PcmFrame*>(xRingbufferReceive(self->pcm_buffer_, &size, pdMS_TO_TICKS(20)));
        if (!frame) {
            std::lock_guard<std::mutex> output_lock(self->output_mutex_);
            if (!prefill && output_epoch == self->active_epoch_.load() && !self->paused_) {
                prefill = true;
                self->QueueState(output_epoch, "BUFFERING");
                ESP_LOGW(TAG, "Music underrun: epoch=%lu", static_cast<unsigned long>(output_epoch));
            }
            continue;
        }
        const auto epoch = frame->epoch;
        if (output_epoch != epoch) { output_epoch = epoch; prefill = true; }
        while (self->running_ && epoch == self->active_epoch_.load() &&
               (self->paused_ || (prefill && !frame->end &&
                self->queued_end_epoch_.load() != epoch &&
                self->buffered_samples_.load() < codec->output_sample_rate() * kPrefillMs / 1000))) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        const bool end = frame->end;
        const auto samples = frame->samples;
        if (epoch == self->active_epoch_.load() && samples) {
            const auto* data = reinterpret_cast<const int16_t*>(frame + 1);
            pcm.assign(data, data + samples);
        }
        self->buffered_samples_.fetch_sub(samples);
        vRingbufferReturnItem(self->pcm_buffer_, frame);
        std::lock_guard<std::mutex> output_lock(self->output_mutex_);
        if (!self->running_ || epoch != self->active_epoch_.load() ||
            Application::GetInstance().GetDeviceState() != kDeviceStateIdle) continue;
        if (end) {
            self->QueueState(epoch, "COMPLETED");
            auto expected = epoch;
            self->active_epoch_.compare_exchange_strong(expected, 0);
            continue;
        }
        if (!samples) continue;
        if (!codec->output_enabled()) codec->EnableOutput(true);
        codec->OutputData(pcm);
        self->played_ms_.fetch_add(samples * 1000 / codec->output_sample_rate());
        if (prefill || self->playing_epoch_ != epoch) {
            self->playing_epoch_ = epoch;
            self->QueueState(epoch, "PLAYING");
            ESP_LOGI(TAG, "Music buffer ready: epoch=%lu, remaining=%lu ms",
                     static_cast<unsigned long>(epoch),
                     static_cast<unsigned long>(self->buffered_samples_.load() * 1000 / codec->output_sample_rate()));
        }
        prefill = false;
    }
    self->output_stopped_ = true;
    vTaskDeleteWithCaps(nullptr);
}

void MusicPlayer::QueueState(uint32_t epoch, const char* state) {
    // 音频线程只投递状态；网络和界面回调由音乐连接线程执行。
    const PlaybackState event{epoch, state};
    if (state_queue_ && epoch == active_epoch_.load()) xQueueOverwrite(state_queue_, &event);
}

void MusicPlayer::DispatchState() {
    PlaybackState event{};
    if (state_queue_ && xQueueReceive(state_queue_, &event, 0) == pdTRUE && on_state_)
        on_state_(event.epoch, event.state);
}

void MusicPlayer::Fail(uint32_t epoch) {
    std::lock_guard<std::mutex> output_lock(output_mutex_);
    QueueState(epoch, "FAILED");
    active_epoch_.compare_exchange_strong(epoch, 0);
}

void MusicPlayer::Stop() {
    paused_ = false;
    active_epoch_ = 0;
    {
        std::lock_guard<std::mutex> output_lock(output_mutex_);
        playing_epoch_ = 0;
    }
    MusicChunk* chunk = nullptr;
    while (queue_ && xQueueReceive(queue_, &chunk, 0) == pdTRUE) heap_caps_free(chunk);
}

void MusicPlayer::Pause() { paused_ = true; }

void MusicPlayer::Resume() { paused_ = false; }

void MusicPlayer::ResetDecoder() {
    decoder_epoch_ = 0;
    sample_rate_ = 0;
    if (decoder_ != nullptr) {
        esp_opus_dec_close(decoder_);
        decoder_ = nullptr;
    }
}
