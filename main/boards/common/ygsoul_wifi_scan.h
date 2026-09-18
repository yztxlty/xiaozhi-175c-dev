#pragma once

#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "wifi_manager.h"
#include "ygsoul_wifi_scan_protocol.h"

// Owned by the BLE provisioning singleton. No Wi-Fi credentials, cloud calls or
// application/voice state live here. A bounded snapshot is pulled over BLE.
class YgSoulWifiScan {
public:
    bool Open() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) return false;
        accepting_ = true;
        ResetLocked();
        return true;
    }
    void Reset() { std::lock_guard<std::mutex> lock(mutex_); ResetLocked(); }
    // Closing is invoked outside the Bluetooth callback before stack teardown.
    // The worker owns at most one 120 ms channel scan at a time. Never forcibly
    // delete it: its radio cleanup must complete before StartStation may run.
    void Close() {
        { std::lock_guard<std::mutex> lock(mutex_); accepting_ = false; ResetLocked(); }
        while (Busy()) vTaskDelay(pdMS_TO_TICKS(5));
    }
    bool Busy() const { std::lock_guard<std::mutex> lock(mutex_); return running_; }

    std::string Handle(const ygsoul::wifi_scan::Request& request, bool may_start) {
        using namespace ygsoul::wifi_scan;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ValidScanId(request.scan_id) ||
            (!request.start && request.index < 0) ||
            request.index >= static_cast<int>(kMaxNetworks))
            return Reply(request.scan_id, State::Failed, rows_, request.index, Error::InvalidRequest);
        if (!accepting_) return Reply(request.scan_id, State::Failed, rows_, request.index, Error::DeviceBusy);
        if (request.start) {
            if (!may_start) return Reply(request.scan_id, State::Failed, rows_, request.index, Error::DeviceBusy);
            // Same scanId retry returns current task state without restarting.
            if (request.scan_id == scan_id_ && state_ != State::Idle)
                return Reply(scan_id_, state_, rows_, -1, error_);
            if (running_) return Reply(request.scan_id, State::Failed, rows_, request.index, Error::DeviceBusy);
            ResetLocked();
            scan_id_ = request.scan_id;
            state_ = State::Scanning;
            running_ = true;
            worker_generation_ = generation_;
            if (xTaskCreate(&RunTask, "yg_wifi_scan", 4096, this, 2, nullptr) != pdPASS) {
                running_ = false;
                state_ = State::Failed;
                error_ = Error::NoMemory;
            }
            return Reply(scan_id_, state_, rows_, -1, error_);
        }
        // GET_WIFI_SCAN_RESULT
        if (request.scan_id != scan_id_ || state_ == State::Idle)
            return Reply(request.scan_id, State::Failed, rows_, request.index, Error::ScanExpired);
        return Reply(scan_id_, state_, rows_, request.index, error_);
    }

private:
    void ResetLocked() {
        ++generation_;
        scan_id_.clear();
        rows_.clear();
        state_ = ygsoul::wifi_scan::State::Idle;
        error_ = ygsoul::wifi_scan::Error::None;
        truncated_ = false;
    }
    bool Current(uint32_t generation) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return accepting_ && generation_ == generation;
    }
    static void RunTask(void* context) {
        auto* self = static_cast<YgSoulWifiScan*>(context);
        self->Run();
        vTaskDelete(nullptr);
    }
    void Run() {
        using namespace ygsoul::wifi_scan;
        uint32_t generation;
        { std::lock_guard<std::mutex> lock(mutex_); generation = worker_generation_; }
        Error failure = Error::None;
        esp_err_t driver_error = ESP_OK;
        bool own_radio = false;
        bool truncated = false;
        std::vector<AccessPoint> rows;
        auto& manager = WifiManager::GetInstance();
        std::unique_ptr<wifi_ap_record_t[]> records(new (std::nothrow) wifi_ap_record_t[kMaxNetworks]);
        if (!records) failure = Error::NoMemory;
        if (failure == Error::None && Current(generation)) {
            if ((!manager.IsInitialized() && !manager.Initialize()) || manager.IsConfigMode()) {
                failure = Error::DeviceBusy;
            } else if (!manager.IsConnected()) {
                // Only explicit BLE pairing pauses an unconnected station. This
                // unregisters its scan/reconnect consumer; saved SSIDs are untouched.
                // A connected station is NEVER stopped for discovery.
                manager.StopStation();
                if (Current(generation)) {
                    driver_error = esp_wifi_set_mode(WIFI_MODE_STA);
                    if (driver_error == ESP_OK) {
                        driver_error = esp_wifi_start();
                        own_radio = driver_error == ESP_OK;
                    }
                    if (driver_error != ESP_OK) failure = Error::ScanFailed;
                }
            }
        }
        wifi_country_t country{};
        if (failure == Error::None && Current(generation)) {
            driver_error = esp_wifi_get_country(&country);
            if (driver_error != ESP_OK || country.schan < 1 || country.schan > 14 || country.nchan < 1)
                failure = Error::ScanFailed;
        }
        // Blocking scans run in this worker, not the BLE callback, and do not
        // emit SCAN_DONE to the station manager. Respect the configured country.
        const int last_channel = std::min(14, static_cast<int>(country.schan) + country.nchan - 1);
        for (int channel = country.schan; failure == Error::None && Current(generation) && channel <= last_channel; ++channel) {
            wifi_scan_config_t config{};
            config.channel = channel;
            config.show_hidden = false;
            config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
            config.scan_time.active.min = 0;
            config.scan_time.active.max = 120;
            driver_error = esp_wifi_scan_start(&config, true);
            if (driver_error != ESP_OK) { failure = Error::ScanFailed; break; }
            uint16_t available = 0;
            driver_error = esp_wifi_scan_get_ap_num(&available);
            if (driver_error != ESP_OK) {
                esp_wifi_clear_ap_list(); failure = Error::ScanFailed; break;
            }
            uint16_t count = static_cast<uint16_t>(std::min<size_t>(available, kMaxNetworks));
            if (available > kMaxNetworks) truncated = true;
            if (count == 0) { esp_wifi_clear_ap_list(); continue; }
            driver_error = esp_wifi_scan_get_ap_records(&count, records.get());
            if (driver_error != ESP_OK) {
                esp_wifi_clear_ap_list(); failure = Error::ScanFailed; break;
            }
            if (!Current(generation)) break;
            for (uint16_t index = 0; index < count; ++index) {
                const auto& ap = records[index];
                AccessPoint row;
                row.ssid = std::string(reinterpret_cast<const char*>(ap.ssid),
                                       strnlen(reinterpret_cast<const char*>(ap.ssid), 32));
                row.rssi = ap.rssi;
                row.has_rssi = true;
                row.channel = ap.primary;
                row.auth = static_cast<int>(ap.authmode);
                AddAccessPoint(rows, row, truncated);
            }
        }
        // Complete cleanup BEFORE publishing DONE/unlocking Wi-Fi configuration.
        if (own_radio) {
            const auto stop_error = esp_wifi_stop();
            if (stop_error != ESP_OK && failure == Error::None) { failure = Error::ScanFailed; driver_error = stop_error; }
        }
        if (failure != Error::None) ESP_LOGW("YGWifiScan", "scan failed: %s (%d)", ErrorName(failure), static_cast<int>(driver_error));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (accepting_ && generation_ == generation) {
                error_ = failure;
                state_ = failure == Error::None ? State::Ready : State::Failed;
                if (failure == Error::None) rows_ = std::move(rows);
                truncated_ = truncated;
            }
            running_ = false;
        }
    }
    mutable std::mutex mutex_;
    bool accepting_ = false;
    bool running_ = false;
    uint32_t generation_ = 0;
    uint32_t worker_generation_ = 0;
    std::string scan_id_;
    std::vector<ygsoul::wifi_scan::AccessPoint> rows_;
    ygsoul::wifi_scan::State state_ = ygsoul::wifi_scan::State::Idle;
    ygsoul::wifi_scan::Error error_ = ygsoul::wifi_scan::Error::None;
    bool truncated_ = false;
};
