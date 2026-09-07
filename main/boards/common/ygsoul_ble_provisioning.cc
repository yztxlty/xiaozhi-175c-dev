#include "ygsoul_ble_provisioning.h"

#include <algorithm>
#include <cstring>
#include <memory>

#include "cJSON.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "application.h"
#include "ssid_manager.h"
#include "system_info.h"
#include "wifi_board.h"
#include "wifi_manager.h"
#include "ygsoul_ble_protocol.h"

namespace {
constexpr char kTag[] = "YGSoulBLE";
constexpr char kProductKey[] = "ESP32S3";
constexpr uint64_t kWifiConnectTimeoutUs = 45ULL * 1000 * 1000;
constexpr uint16_t kAppId = 0x42;
enum AttributeIndex { kService, kWriteDecl, kWriteValue, kNotifyDecl, kNotifyValue, kNotifyCccd, kCount };
YgSoulBleProvisioning* g_service = nullptr;
uint16_t g_handles[kCount]{};
uint16_t g_serviceUuid = ygsoul::ble::kServiceUuid;
// ESP-IDF requires a 16-byte UUID-shaped buffer for service advertising.
// Keep the service in the primary packet and put the longer name in scan rsp.
uint8_t g_advertisedServiceUuid[16] = {
    0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x10, 0x19, 0x00, 0x00,
};
bool g_adv_data_ready = false;
bool g_scan_rsp_ready = false;
uint16_t g_primaryServiceUuid = ESP_GATT_UUID_PRI_SERVICE;
uint16_t g_charDeclUuid = ESP_GATT_UUID_CHAR_DECLARE;
uint16_t g_writeUuid = ygsoul::ble::kWriteCharacteristicUuid;
uint16_t g_notifyUuid = ygsoul::ble::kNotifyCharacteristicUuid;
uint16_t g_cccdUuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
uint8_t g_writeProperties = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
uint8_t g_notifyProperties = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
uint8_t g_notifyValue[1] = {0};
uint8_t g_notifyCccd[2] = {0, 0};

const esp_gatts_attr_db_t kGattDb[kCount] = {
    [kService] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&g_primaryServiceUuid),
        ESP_GATT_PERM_READ, sizeof(uint16_t), sizeof(uint16_t), reinterpret_cast<uint8_t*>(&g_serviceUuid)}},
    [kWriteDecl] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&g_charDeclUuid),
        ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t), &g_writeProperties}},
    [kWriteValue] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&g_writeUuid),
        ESP_GATT_PERM_WRITE, 238, 0, nullptr}},
    [kNotifyDecl] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&g_charDeclUuid),
        ESP_GATT_PERM_READ, sizeof(uint8_t), sizeof(uint8_t), &g_notifyProperties}},
    [kNotifyValue] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&g_notifyUuid),
        ESP_GATT_PERM_READ, 238, sizeof(g_notifyValue), g_notifyValue}},
    [kNotifyCccd] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, reinterpret_cast<uint8_t*>(&g_cccdUuid),
        ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
        sizeof(g_notifyCccd), sizeof(g_notifyCccd), g_notifyCccd}},
};

esp_ble_adv_data_t kAdvData = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0x20,
    .max_interval = 0x40,
    .appearance = 0,
    .manufacturer_len = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len = 0,
    .p_service_data = nullptr,
    .service_uuid_len = sizeof(g_advertisedServiceUuid),
    .p_service_uuid = g_advertisedServiceUuid,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};
esp_ble_adv_data_t kScanRspData = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0,
    .max_interval = 0,
    .appearance = 0,
    .manufacturer_len = 0,
    .p_manufacturer_data = nullptr,
    .service_data_len = 0,
    .p_service_data = nullptr,
    .service_uuid_len = 0,
    .p_service_uuid = nullptr,
    .flag = 0,
};
esp_ble_adv_params_t kAdvParams = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};
}

YgSoulBleProvisioning& YgSoulBleProvisioning::GetInstance() {
    static YgSoulBleProvisioning instance;
    return instance;
}

esp_err_t YgSoulBleProvisioning::Start() {
    if (started_) return ESP_OK;
    parser_ = new ygsoul::ble::BleFrameParser();
    g_service = this;
    const auto release_result = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (release_result != ESP_OK && release_result != ESP_ERR_INVALID_STATE) return release_result;
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t result = esp_bt_controller_init(&controller_config);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    result = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    result = esp_bluedroid_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    result = esp_bluedroid_enable();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
    uint8_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;
    esp_ble_io_cap_t io_cap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t auth_option = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE,
                                                    &auth_req, sizeof(auth_req)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE,
                                                    &io_cap, sizeof(io_cap)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE,
                                                    &key_size, sizeof(key_size)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY,
                                                    &init_key, sizeof(init_key)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY,
                                                    &rsp_key, sizeof(rsp_key)));
    ESP_ERROR_CHECK(esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH,
                                                    &auth_option, sizeof(auth_option)));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(GattsEvent));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(GapEvent));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(kAppId));
    started_ = true;
    ESP_LOGI(kTag, "BLE provisioning started");
    return ESP_OK;
}

void YgSoulBleProvisioning::EnsureAdvertising() {
    if (!started_ || connected_) {
        return;
    }
    const auto result = esp_ble_gap_start_advertising(&kAdvParams);
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "ensure advertising: %s", esp_err_to_name(result));
    }
}

void YgSoulBleProvisioning::Stop() {
    pairing_receipt_.CancelPending();
    if (!started_) return;
    if (wifi_connect_timeout_ != nullptr) {
        esp_timer_stop(wifi_connect_timeout_);
        esp_timer_delete(wifi_connect_timeout_);
        wifi_connect_timeout_ = nullptr;
    }
    esp_ble_gap_stop_advertising();
    if (gatts_if_ != ESP_GATT_IF_NONE) esp_ble_gatts_stop_service(g_handles[kService]);
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    delete parser_;
    parser_ = nullptr;
    started_ = connected_ = netcfg_started_ = false;
    candidate_saved_ = false;
    previous_ssids_.clear();
    gatts_if_ = ESP_GATT_IF_NONE;
    g_service = nullptr;
}

void YgSoulBleProvisioning::GattsEvent(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                       esp_ble_gatts_cb_param_t* param) {
    if (!g_service) return;
    switch (event) {
        case ESP_GATTS_REG_EVT:
            g_service->gatts_if_ = gatts_if;
            {
                const auto device_name = std::string("YGSOUL-") + SystemInfo::GetMacAddress().substr(9);
                ESP_LOGI(kTag, "GATT registered: if=%d name=%s", gatts_if, device_name.c_str());
                ESP_LOGI(kTag, "set device name: %s", esp_err_to_name(esp_ble_gap_set_device_name(device_name.c_str())));
                ESP_LOGI(kTag, "config advertising: %s", esp_err_to_name(esp_ble_gap_config_adv_data(&kAdvData)));
                ESP_LOGI(kTag, "config scan response: %s", esp_err_to_name(esp_ble_gap_config_adv_data(&kScanRspData)));
            }
            ESP_LOGI(kTag, "create GATT table: %s", esp_err_to_name(esp_ble_gatts_create_attr_tab(kGattDb, gatts_if, kCount, 0)));
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            ESP_LOGI(kTag, "GATT table result: status=%d handles=%d", param->add_attr_tab.status,
                     param->add_attr_tab.num_handle);
            if (param->add_attr_tab.status == ESP_GATT_OK && param->add_attr_tab.num_handle == kCount) {
                std::copy(param->add_attr_tab.handles, param->add_attr_tab.handles + kCount, g_handles);
                g_service->notify_handle_ = g_handles[kNotifyValue];
                ESP_LOGI(kTag, "start GATT service: %s",
                         esp_err_to_name(esp_ble_gatts_start_service(g_handles[kService])));
            }
            break;
        case ESP_GATTS_CONNECT_EVT:
            g_service->connected_ = true;
            g_service->connection_id_ = param->connect.conn_id;
            break;
        case ESP_GATTS_DISCONNECT_EVT:
            g_service->connected_ = false;
            esp_ble_gap_start_advertising(&kAdvParams);
            break;
        case ESP_GATTS_WRITE_EVT:
            g_service->HandleWrite(param);
            break;
        default:
            break;
    }
}

void YgSoulBleProvisioning::GapEvent(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param) {
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
        g_adv_data_ready = param->adv_data_cmpl.status == ESP_BT_STATUS_SUCCESS;
        ESP_LOGI(kTag, "advertising data result: status=%d ready=%d", param->adv_data_cmpl.status, g_adv_data_ready);
    } else if (event == ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT) {
        g_scan_rsp_ready = param->scan_rsp_data_cmpl.status == ESP_BT_STATUS_SUCCESS;
        ESP_LOGI(kTag, "scan response result: status=%d ready=%d", param->scan_rsp_data_cmpl.status, g_scan_rsp_ready);
    }
    if ((event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT || event == ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT)
        && g_adv_data_ready && g_scan_rsp_ready) {
        ESP_LOGI(kTag, "start advertising after data: %s",
                 esp_err_to_name(esp_ble_gap_start_advertising(&kAdvParams)));
    } else if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT) {
        ESP_LOGI(kTag, "advertising start result: status=%d", param->adv_start_cmpl.status);
    } else if (event == ESP_GAP_BLE_SEC_REQ_EVT) {
        ESP_LOGI(kTag, "security request: %s",
                 esp_err_to_name(esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true)));
    } else if (event == ESP_GAP_BLE_AUTH_CMPL_EVT) {
        ESP_LOGI(kTag, "security authentication: %s",
                 param->ble_security.auth_cmpl.success ? "succeeded" : "failed");
    }
}

void YgSoulBleProvisioning::HandleWrite(esp_ble_gatts_cb_param_t* param) {
    if (param->write.is_prep || param->write.handle != g_handles[kWriteValue] || !parser_) return;
    ESP_LOGI(kTag, "BLE write received: bytes=%u", param->write.len);
    std::vector<ygsoul::ble::Frame> frames;
    parser_->Feed(param->write.value, param->write.len, frames);
    for (const auto& frame : frames) {
        ESP_LOGI(kTag, "BLE command received: %u", frame.command);
        HandleFrame(frame.command, frame.payload);
    }
}

namespace {
bool ParseWifiConfig(const std::string& payload, std::string& token, std::string& ssid, std::string& password) {
    size_t index = 0;
    auto next = [&](std::string& out) {
        if (index >= payload.size()) return false;
        const auto length = static_cast<uint8_t>(payload[index++]);
        if (index + length > payload.size()) return false;
        out.assign(payload.data() + index, length);
        index += length;
        return true;
    };
    std::string first;
    std::string second;
    if (!next(first) || !next(second)) return false;
    if (index == payload.size()) {
        ssid = first;
        password = second;
        return !ssid.empty();
    }
    std::string third;
    if (!next(third) || index != payload.size()) return false;
    token = first;
    ssid = second;
    password = third;
    return !token.empty() && !ssid.empty();
}
}

void YgSoulBleProvisioning::HandleFrame(uint8_t command, const std::string& payload) {
    if (command == ygsoul::ble::kQueryDevInfo) {
        cJSON* json = cJSON_CreateObject();
        const auto mac = SystemInfo::GetMacAddress();
        cJSON_AddStringToObject(json, "vendorSn", mac.c_str());
        cJSON_AddStringToObject(json, "productKey", kProductKey);
        cJSON_AddStringToObject(json, "modelCode", kProductKey);
        cJSON_AddStringToObject(json, "name", "YGSoul ESP32S3");
        cJSON_AddStringToObject(json, "status", netcfg_started_ ? "WIFI_CONFIGURING" : "READY");
        char* text = cJSON_PrintUnformatted(json);
        Notify(ygsoul::ble::kDevInfoRsp, text);
        cJSON_free(text);
        cJSON_Delete(json);
        return;
    }
    if (command != ygsoul::ble::kWifiConfig) {
        ESP_LOGW(kTag, "BLE command rejected: %u", command);
        return;
    }
    std::string token;
    std::string ssid;
    std::string password;
    if (!ParseWifiConfig(payload, token, ssid, password)) {
        SendStatus("FAILED", "INVALID_WIFI");
        return;
    }
    pairing_receipt_.Begin(token, ssid);
    netcfg_started_ = true;
    if (!candidate_saved_) {
        previous_ssids_ = SsidManager::GetInstance().GetSsidList();
        candidate_saved_ = true;
    }
    SsidManager::GetInstance().AddSsid(ssid.c_str(), password.c_str());
    ESP_LOGI(kTag, "WiFi credentials saved; starting station");
    WifiManager::GetInstance().StopStation();
    pairing_receipt_.StartWaiting();
    if (wifi_connect_timeout_ == nullptr) {
        const esp_timer_create_args_t timer_args = {
            .callback = &YgSoulBleProvisioning::OnWifiConnectTimeout,
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "BleWifiTimeout",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &wifi_connect_timeout_));
    }
    esp_timer_stop(wifi_connect_timeout_);
    ESP_ERROR_CHECK(esp_timer_start_once(wifi_connect_timeout_, kWifiConnectTimeoutUs));
    WifiManager::GetInstance().StartStation();
    SendStatus("CONNECTING");
}

void YgSoulBleProvisioning::Notify(uint8_t command, const std::string& payload) {
    if (!connected_) return;
    const auto frame = ygsoul::ble::EncodeBleFrame(command, payload);
    for (size_t offset = 0; offset < frame.size(); offset += 20) {
        const auto length = std::min<size_t>(20, frame.size() - offset);
        esp_ble_gatts_send_indicate(gatts_if_, connection_id_, notify_handle_, length,
                                    const_cast<uint8_t*>(frame.data() + offset), false);
    }
}

void YgSoulBleProvisioning::SendStatus(const char* status, const char* code) {
    ESP_LOGI(kTag, "Provisioning status: %s%s", status, code ? " (with code)" : "");
    cJSON* json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", status);
    if (code) cJSON_AddStringToObject(json, "code", code);
    char* text = cJSON_PrintUnformatted(json);
    Notify(ygsoul::ble::kNetcfgResult, text);
    cJSON_free(text);
    cJSON_Delete(json);
}

void YgSoulBleProvisioning::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    if (event == NetworkEvent::Connected) {
        const auto result = pairing_receipt_.Complete(data);
        if (result == ygsoul::ble::PairingReceipt::Result::Ignored) return;
        if (result == ygsoul::ble::PairingReceipt::Result::Mismatch) {
            FailProvisioning("WIFI_SSID_MISMATCH");
            return;
        }
        if (wifi_connect_timeout_ != nullptr) esp_timer_stop(wifi_connect_timeout_);
        candidate_saved_ = false;
        previous_ssids_.clear();
        SendStatus("SUCCEEDED");
        EnsureAdvertising();
        Application::GetInstance().Schedule([]() {
            static_cast<WifiBoard&>(Board::GetInstance()).ExitWifiConfigMode();
        });
    }
}

void YgSoulBleProvisioning::OnWifiConnectTimeout(void* arg) {
    auto* service = static_cast<YgSoulBleProvisioning*>(arg);
    if (service->pairing_receipt_.CancelPending()) {
        service->FailProvisioning("WIFI_CONNECT_TIMEOUT");
    }
}

void YgSoulBleProvisioning::FailProvisioning(const char* code) {
    pairing_receipt_.CancelPending();
    if (wifi_connect_timeout_ != nullptr) esp_timer_stop(wifi_connect_timeout_);
    WifiManager::GetInstance().StopStation();
    if (candidate_saved_) {
        SsidManager::GetInstance().ReplaceSsidList(previous_ssids_);
    }
    candidate_saved_ = false;
    previous_ssids_.clear();
    netcfg_started_ = false;
    SendStatus("FAILED", code);
    // Keep BLE provisioning available for an immediate retry.
}
