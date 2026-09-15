#pragma once

#include "music_player.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <freertos/task.h>

class WebSocket;

struct MusicTrackInfo {
    std::string id;
    std::string title;
    std::string artist;
    std::string cover_url;
    uint32_t duration_ms = 0;
};

enum class MusicUiState { kConnecting, kReady, kBuffering, kPlaying, kPaused, kDisconnected, kFailed };

class MusicClient {
public:
    MusicClient();
    ~MusicClient();
    void Start();
    void Stop();
    bool Select(size_t index);
    bool PlaySelected();
    bool TogglePlayback();
    bool Seek(uint32_t position_ms);
    bool StopPlayback();
    void SetBusy(bool busy);
    std::vector<MusicTrackInfo> GetTracks() const;
    MusicUiState GetUiState() const { return ui_state_.load(); }
    uint32_t GetPositionMs() const { return position_ms_.load() + player_.GetPlayedMs(); }
    void OnPlaylist(std::function<void()> callback) { on_playlist_ = std::move(callback); }
    void OnChanged(std::function<void()> callback) { on_changed_ = std::move(callback); }

private:
    std::unique_ptr<WebSocket> websocket_;
    MusicPlayer player_;
    std::vector<MusicTrackInfo> tracks_;
    std::function<void()> on_playlist_;
    std::function<void()> on_changed_;
    mutable std::mutex mutex_;
    std::mutex task_mutex_;
    std::string url_;
    std::string token_;
    std::string playback_id_;
    std::atomic_uint32_t stream_epoch_{0};
    std::atomic_uint32_t expected_sequence_{0};
    size_t selected_index_ = 0;
    bool connected_ = false;
    std::atomic_bool running_{false};
    std::atomic_bool busy_{false};
    std::atomic_bool availability_known_{false};
    std::atomic<MusicUiState> ui_state_{MusicUiState::kDisconnected};
    std::atomic_uint32_t position_ms_{0};
    TaskHandle_t task_ = nullptr;

    static void Run(void* arg);
    void HandleText(const char* data, size_t size);
    void HandleBinary(const uint8_t* data, size_t size);
    bool Send(const std::string& text);
    void ReloadCredentials();
    void ReportPlaybackState(const char* state, uint32_t expected_epoch = 0);
    void SetUiState(MusicUiState state);
};
