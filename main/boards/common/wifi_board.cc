#include "wifi_board.h"

#include "display.h"
#include "application.h"
#include "system_info.h"
#include "settings.h"
#include "assets.h"
#include "assets/lang_config.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_network.h>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <utility>

#include <font_awesome.h>
#include <wifi_manager.h>
#include <wifi_station.h>
#include <ssid_manager.h>
#include "afsk_demod.h"
#include "mcp_server.h"
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
#include "blufi.h"
#endif
#ifdef CONFIG_USE_YGSOUL_BLE_WIFI_PROVISIONING
#include "ygsoul_ble_provisioning.h"
#endif

static const char *TAG = "WifiBoard";

// Connection timeout in seconds
static constexpr int CONNECT_TIMEOUT_SEC = 60;

WifiBoard::WifiBoard() {
    // Create connection timeout timer
    esp_timer_create_args_t timer_args = {
        .callback = OnWifiConnectTimeout,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_connect_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&timer_args, &connect_timer_);

    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddTool("self.system.reconfigure_wifi",
        "When the user says 进入配网模式, call this immediately and switch to pairing mode. "
        "Do not ask for confirmation. Keep the current Wi-Fi so the user can still speak.",
        PropertyList(), [this](const PropertyList&) {
            EnterWifiConfigModeForVoice();
            return true;
        });
    mcp_server.AddTool("self.system.exit_wifi_config",
        "When the user says 退出配网模式, call this immediately and leave pairing mode. Do not wait.",
        PropertyList(), [this](const PropertyList&) {
            ExitWifiConfigMode();
            return true;
        });
}

WifiBoard::~WifiBoard() {
    if (connect_timer_) {
        esp_timer_stop(connect_timer_);
        esp_timer_delete(connect_timer_);
    }
}

std::string WifiBoard::GetBoardType() {
    return "wifi";
}

void WifiBoard::StartNetwork() {
    auto& wifi_manager = WifiManager::GetInstance();

    // Initialize WiFi manager
    WifiManagerConfig config;
    config.ssid_prefix = "Xiaozhi";
    config.language = Lang::CODE;
    wifi_manager.Initialize(config);

    // Set unified event callback - forward to NetworkEvent with SSID data
    wifi_manager.SetEventCallback([this](WifiEvent event, const std::string& data) {
        switch (event) {
            case WifiEvent::Scanning:
                OnNetworkEvent(NetworkEvent::Scanning);
                break;
            case WifiEvent::Connecting:
                OnNetworkEvent(NetworkEvent::Connecting, data);
                break;
            case WifiEvent::Connected:
                OnNetworkEvent(NetworkEvent::Connected, data);
                break;
            case WifiEvent::Disconnected:
                OnNetworkEvent(NetworkEvent::Disconnected);
                break;
            case WifiEvent::ConfigModeEnter:
                OnNetworkEvent(NetworkEvent::WifiConfigModeEnter);
                break;
            case WifiEvent::ConfigModeExit:
                OnNetworkEvent(NetworkEvent::WifiConfigModeExit);
                break;
        }
    });

    // Try to connect or enter config mode
    TryWifiConnect();
}

void WifiBoard::TryWifiConnect() {
    auto& ssid_manager = SsidManager::GetInstance();
    bool have_ssid = !ssid_manager.GetSsidList().empty();

    if (have_ssid) {
        // Start connection attempt with timeout
        ESP_LOGI(TAG, "Starting WiFi connection attempt");
        esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
        WifiManager::GetInstance().StartStation();
    } else {
        // No SSID configured, enter config mode
        // Wait for the board version to be shown
        vTaskDelay(pdMS_TO_TICKS(300));
        StartWifiConfigMode();
    }
}

void WifiBoard::OnNetworkEvent(NetworkEvent event, const std::string& data) {
    switch (event) {
        case NetworkEvent::Connected:
            // Stop timeout timer
            esp_timer_stop(connect_timer_);
#ifdef CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
            // make sure blufi resources has been released
            Blufi::GetInstance().deinit();
#endif
#ifdef CONFIG_USE_YGSOUL_BLE_WIFI_PROVISIONING
            YgSoulBleProvisioning::GetInstance().OnNetworkEvent(event, data);
#endif
            in_config_mode_ = false;
            ESP_LOGI(TAG, "Connected to WiFi: %s", data.c_str());
            break;
        case NetworkEvent::Scanning:
            ESP_LOGI(TAG, "WiFi scanning");
            break;
        case NetworkEvent::Connecting:
            ESP_LOGI(TAG, "WiFi connecting to %s", data.c_str());
            break;
        case NetworkEvent::Disconnected:
            ESP_LOGW(TAG, "WiFi disconnected");
#ifdef CONFIG_USE_YGSOUL_BLE_WIFI_PROVISIONING
            YgSoulBleProvisioning::GetInstance().OnNetworkEvent(event, data);
#endif
            // A device can receive an IP and then lose Wi-Fi. Restart the
            // bounded recovery timer so it cannot remain in silent retries.
            esp_timer_stop(connect_timer_);
            esp_timer_start_once(connect_timer_, CONNECT_TIMEOUT_SEC * 1000000ULL);
            break;
        case NetworkEvent::WifiConfigModeEnter:
            ESP_LOGI(TAG, "WiFi config mode entered");
            in_config_mode_ = true;
            break;
        case NetworkEvent::WifiConfigModeExit:
            ESP_LOGI(TAG, "WiFi config mode exited");
            in_config_mode_ = false;
            // Try to connect with the new credentials
            TryWifiConnect();
            break;
        default:
            break;
    }

    // Notify external callback if set
    if (network_event_callback_) {
        network_event_callback_(event, data);
    }
}

void WifiBoard::SetNetworkEventCallback(NetworkEventCallback callback) {
    network_event_callback_ = std::move(callback);
}

void WifiBoard::OnWifiConnectTimeout(void* arg) {
    auto* board = static_cast<WifiBoard*>(arg);
    ESP_LOGW(TAG, "WiFi connection timeout, entering config mode");

    WifiManager::GetInstance().StopStation();
    board->StartWifiConfigMode();
}

void WifiBoard::StartWifiConfigMode() {
    in_config_mode_ = true;
    if (!Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring)) {
        in_config_mode_ = false;
        ESP_LOGE(TAG, "Unable to enter Wi-Fi config state");
        return;
    }
#ifdef CONFIG_USE_YGSOUL_BLE_WIFI_PROVISIONING
    // Run after the state handler releases conversation audio memory.
    Application::GetInstance().Schedule([this]() {
        if (Application::GetInstance().GetDeviceState() != kDeviceStateWifiConfiguring) {
            return;
        }
        if (YgSoulBleProvisioning::GetInstance().Start() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start YGSoul BLE provisioning");
            in_config_mode_ = false;
        }
    });
#elif CONFIG_USE_HOTSPOT_WIFI_PROVISIONING
    auto& wifi_manager = WifiManager::GetInstance();

    wifi_manager.StartConfigAp();

    // Show config prompt after a short delay
    Application::GetInstance().Schedule([&wifi_manager]() {
        std::string hint = Lang::Strings::CONNECT_TO_HOTSPOT;
        hint += wifi_manager.GetApSsid();
        hint += Lang::Strings::ACCESS_VIA_BROWSER;
        hint += wifi_manager.GetApWebUrl();

        Application::GetInstance().Alert(Lang::Strings::WIFI_CONFIG_MODE, hint.c_str(), "gear", Lang::Sounds::OGG_WIFICONFIG);
    });
#elif CONFIG_USE_ESP_BLUFI_WIFI_PROVISIONING
    auto &blufi = Blufi::GetInstance();
    // initialize esp-blufi protocol
    blufi.init();
#endif
#if CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING
    // Start acoustic provisioning task
    auto codec = Board::GetInstance().GetAudioCodec();
    int channel = codec ? codec->input_channels() : 1;
    ESP_LOGI(TAG, "Starting acoustic WiFi provisioning, channels: %d", channel);

    xTaskCreate([](void* arg) {
        auto ch = reinterpret_cast<intptr_t>(arg);
        auto& app = Application::GetInstance();
        auto& wifi = WifiManager::GetInstance();
        auto disp = Board::GetInstance().GetDisplay();
        audio_wifi_config::ReceiveWifiCredentialsFromAudio(&app, &wifi, disp, ch);
        vTaskDelete(NULL);
    }, "acoustic_wifi", 4096, reinterpret_cast<void*>(channel), 2, NULL);
#endif
}

void WifiBoard::EnterWifiConfigMode() {
    ESP_LOGI(TAG, "EnterWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    if (IsInWifiConfigMode()) {
        ESP_LOGI(TAG, "already in wifi config mode");
#ifdef CONFIG_USE_YGSOUL_BLE_WIFI_PROVISIONING
        YgSoulBleProvisioning::GetInstance().EnsureAdvertising();
#endif
        return;
    }

    if (WifiManager::GetInstance().IsConnected()) {
        ESP_LOGI(TAG, "Wi-Fi already connected, keep station and start BLE pairing");
        StartWifiConfigMode();
        return;
    }

    auto& app = Application::GetInstance();
    auto state = app.GetDeviceState();

    if (state == kDeviceStateSpeaking || state == kDeviceStateListening || state == kDeviceStateIdle
        || state == kDeviceStateActivating || state == kDeviceStateConnecting
        || state == kDeviceStateUpgrading || state == kDeviceStateUnknown) {
        xTaskCreate([](void* arg) {
            auto* board = static_cast<WifiBoard*>(arg);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_timer_stop(board->connect_timer_);
            board->StartWifiConfigMode();
            vTaskDelete(NULL);
        }, "wifi_cfg_delay", 4096, this, 2, NULL);
        return;
    }

    if (state != kDeviceStateStarting) {
        ESP_LOGE(TAG, "EnterWifiConfigMode called but device state is not starting or speaking, device state: %d", state);
        return;
    }

    esp_timer_stop(connect_timer_);
    StartWifiConfigMode();
}

void WifiBoard::EnterWifiConfigModeForVoice() {
    ESP_LOGI(TAG, "EnterWifiConfigModeForVoice called");
    GetDisplay()->ShowNotification(Lang::Strings::ENTERING_WIFI_CONFIG_MODE);
    if (IsInWifiConfigMode()) {
        ESP_LOGI(TAG, "already in wifi config mode");
        return;
    }
    StartWifiConfigMode();
}

void WifiBoard::ExitWifiConfigMode() {
    ESP_LOGI(TAG, "ExitWifiConfigMode called");
    GetDisplay()->ShowNotification(Lang::Strings::EXITING_WIFI_CONFIG_MODE);
#ifdef CONFIG_USE_YGSOUL_BLE_WIFI_PROVISIONING
    YgSoulBleProvisioning::GetInstance().Stop();
#endif
    in_config_mode_ = false;
    Application::GetInstance().EnterStandby();
}

bool WifiBoard::IsInWifiConfigMode() const {
    return in_config_mode_ || WifiManager::GetInstance().IsConfigMode();
}

NetworkInterface* WifiBoard::GetNetwork() {
    static EspNetwork network;
    return &network;
}

const char* WifiBoard::GetNetworkStateIcon() {
    auto& wifi = WifiManager::GetInstance();

    if (wifi.IsConfigMode()) {
        return FONT_AWESOME_WIFI;
    }
    if (!wifi.IsConnected()) {
        return FONT_AWESOME_WIFI_SLASH;
    }

    int rssi = wifi.GetRssi();
    if (rssi >= -65) {
        return FONT_AWESOME_WIFI;
    } else if (rssi >= -75) {
        return FONT_AWESOME_WIFI_FAIR;
    }
    return FONT_AWESOME_WIFI_WEAK;
}

std::string WifiBoard::GetBoardJson() {
    auto& wifi = WifiManager::GetInstance();
    std::string json = R"({"type":")" + std::string(BOARD_TYPE) + R"(",)";
    json += R"("name":")" + std::string(BOARD_NAME) + R"(",)";

    if (!wifi.IsConfigMode()) {
        json += R"("ssid":")" + wifi.GetSsid() + R"(",)";
        json += R"("rssi":)" + std::to_string(wifi.GetRssi()) + R"(,)";
        json += R"("channel":)" + std::to_string(wifi.GetChannel()) + R"(,)";
        json += R"("ip":")" + wifi.GetIpAddress() + R"(",)";
    }

    json += R"("mac":")" + SystemInfo::GetMacAddress() + R"("})";
    return json;
}

void WifiBoard::SetPowerSaveLevel(PowerSaveLevel level) {
    WifiPowerSaveLevel wifi_level;
    switch (level) {
        case PowerSaveLevel::LOW_POWER:
            wifi_level = WifiPowerSaveLevel::LOW_POWER;
            break;
        case PowerSaveLevel::BALANCED:
            wifi_level = WifiPowerSaveLevel::BALANCED;
            break;
        case PowerSaveLevel::PERFORMANCE:
        default:
            wifi_level = WifiPowerSaveLevel::PERFORMANCE;
            break;
    }
    WifiManager::GetInstance().SetPowerSaveLevel(wifi_level);
}

std::string WifiBoard::GetDeviceStatusJson() {
    auto& board = Board::GetInstance();
    auto root = cJSON_CreateObject();

    auto vendor_sn = SystemInfo::GetMacAddress();
    vendor_sn.erase(std::remove(vendor_sn.begin(), vendor_sn.end(), ':'), vendor_sn.end());
    cJSON_AddStringToObject(root, "vendorSn", vendor_sn.c_str());
    cJSON_AddStringToObject(root, "productKey", "ESP32S3");
    cJSON_AddStringToObject(root, "hardwareVersion", BOARD_NAME);
    cJSON_AddStringToObject(root, "firmwareVersion", esp_app_get_description()->version);

    // Audio speaker
    auto audio_speaker = cJSON_CreateObject();
    if (auto codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio_speaker, "volume", codec->output_volume());
    }
    cJSON_AddItemToObject(root, "audio_speaker", audio_speaker);

    // Screen
    auto screen = cJSON_CreateObject();
    if (board.GetBacklight() != nullptr) {
        // The panel fades asynchronously. Report the persisted user setting, not
        // a transient brightness value sampled during the fade or power-save dim.
        Settings settings("display", false);
        cJSON_AddNumberToObject(screen, "brightness", settings.GetInt("brightness", 75));
    }
    if (auto display = board.GetDisplay(); display && display->height() > 64) {
        if (auto theme = display->GetTheme()) {
            cJSON_AddStringToObject(screen, "theme", theme->name().c_str());
        }
    }
    cJSON_AddItemToObject(root, "screen", screen);

    // Battery
    int level = 0;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(level, charging, discharging)) {
        auto battery = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery, "level", level);
        cJSON_AddBoolToObject(battery, "charging", charging);
        cJSON_AddItemToObject(root, "battery", battery);
    }

    // Network
    auto& wifi = WifiManager::GetInstance();
    auto network = cJSON_CreateObject();
    cJSON_AddStringToObject(network, "type", "wifi");
    cJSON_AddStringToObject(network, "ssid", wifi.GetSsid().c_str());
    int rssi = wifi.GetRssi();
    cJSON_AddNumberToObject(network, "rssi", rssi);
    const char* signal = rssi >= -60 ? "strong" : (rssi >= -70 ? "medium" : "weak");
    cJSON_AddStringToObject(network, "signal", signal);
    cJSON_AddItemToObject(root, "network", network);

    auto& assets = Assets::GetInstance();
    auto storage_free = assets.GetFreeSpace();
    auto storage_total = assets.GetTotalSize();
    cJSON_AddNumberToObject(root, "storageFree", storage_free);
    cJSON_AddNumberToObject(root, "storageTotal", storage_total);
    if (storage_total >= 1024 * 1024) {
        int total_mb = static_cast<int>(storage_total / (1024 * 1024));
        int free_mb = static_cast<int>(storage_free / (1024 * 1024));
        cJSON_AddNumberToObject(root, "totalStorageMb", total_mb);
        cJSON_AddNumberToObject(root, "usedStorageMb", total_mb > free_mb ? total_mb - free_mb : 0);
    }

    int auto_sleep_minutes = board.GetAutoSleepMinutes();
    if (auto_sleep_minutes > 0) {
        cJSON_AddNumberToObject(root, "autoSleep", auto_sleep_minutes);
    }

    // Chip temperature
    float temp = 0.0f;
    if (board.GetTemperature(temp)) {
        auto chip = cJSON_CreateObject();
        cJSON_AddNumberToObject(chip, "temperature", temp);
        cJSON_AddItemToObject(root, "chip", chip);
    }

    auto str = cJSON_PrintUnformatted(root);
    std::string result(str);
    cJSON_free(str);
    cJSON_Delete(root);
    return result;
}
