#pragma once

#include <string>
#include <vector>

#include "board.h"
#include "ssid_manager.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_timer.h"
#include "ygsoul_ble_protocol.h"

class YgSoulBleProvisioning {
public:
    static YgSoulBleProvisioning& GetInstance();

    esp_err_t Start();
    void Stop();
    void EnsureAdvertising();
    void OnNetworkEvent(NetworkEvent event, const std::string& data);
    std::string GetPairingSessionId() const { return pairing_receipt_.SessionId(); }

private:
    static void GattsEvent(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                           esp_ble_gatts_cb_param_t* param);
    static void GapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
    void HandleWrite(esp_ble_gatts_cb_param_t* param);
    void HandleFrame(uint8_t command, const std::string& payload);
    void Notify(uint8_t command, const std::string& payload);
    void SendStatus(const char* status, const char* code = nullptr);
    void FailProvisioning(const char* code);
    static void OnWifiConnectTimeout(void* arg);

    bool started_ = false;
    bool connected_ = false;
    bool netcfg_started_ = false;
    esp_gatt_if_t gatts_if_ = ESP_GATT_IF_NONE;
    uint16_t connection_id_ = 0;
    uint16_t notify_handle_ = 0;
    ygsoul::ble::PairingReceipt pairing_receipt_;
    bool candidate_saved_ = false;
    std::vector<SsidItem> previous_ssids_;
    esp_timer_handle_t wifi_connect_timeout_ = nullptr;
    ygsoul::ble::BleFrameParser* parser_ = nullptr;
};
