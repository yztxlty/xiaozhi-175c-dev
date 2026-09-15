#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <atomic>
#include <functional>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>

class OggDemuxer;

class MusicPlayer {
public:
    MusicPlayer();
    ~MusicPlayer();

    void Begin(uint32_t epoch, int frame_duration_ms);
    bool Feed(const uint8_t* data, size_t size);
    bool FeedEnd();
    void Pause();
    void Resume();
    void Stop();
    void OnState(std::function<void(uint32_t, const char*)> callback) { on_state_ = std::move(callback); }
    void DispatchState();
    uint32_t GetPlayedMs() const { return played_ms_.load(); }

private:
    std::unique_ptr<OggDemuxer> demuxer_;
    QueueHandle_t queue_ = nullptr;
    RingbufHandle_t pcm_buffer_ = nullptr;
    QueueHandle_t state_queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    TaskHandle_t output_task_ = nullptr;
    void* decoder_ = nullptr;
    std::atomic_uint32_t active_epoch_{0};
    std::atomic_int requested_frame_duration_ms_{60};
    std::atomic_bool running_{true};
    std::atomic_bool available_{false};
    std::atomic_bool paused_{false};
    std::atomic_bool decode_stopped_{true};
    std::atomic_bool output_stopped_{true};
    std::atomic_uint32_t buffered_samples_{0};
    std::atomic_uint32_t queued_end_epoch_{0};
    std::atomic_uint32_t played_ms_{0};
    uint32_t decoder_epoch_ = 0;
    int frame_duration_ms_ = 60;
    int sample_rate_ = 0;
    uint32_t playing_epoch_ = 0;
    std::mutex output_mutex_;
    std::function<void(uint32_t, const char*)> on_state_;

    static void Run(void* arg);
    static void Output(void* arg);
    void QueueState(uint32_t epoch, const char* state);
    void Fail(uint32_t epoch);
    void ResetDecoder();
    void Decode(const uint8_t* data, int sample_rate, size_t size);
};
