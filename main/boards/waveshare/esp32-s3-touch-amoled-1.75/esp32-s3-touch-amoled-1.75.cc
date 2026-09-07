#include "wifi_board.h"
#include "display/lcd_display.h"
#include "esp_lcd_co5300.h"
#include "assets.h"

#include "codecs/box_audio_codec.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "config.h"
#include "power_save_timer.h"
#include "axp2101.h"
#include "i2c_device.h"

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_timer.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include "esp_io_expander_tca9554.h"
#include "settings.h"
#include "assets/source_arrays/ygsoul_boot_lvgl.h"

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include <array>
#include <cstring>

#define TAG "WaveshareEsp32s3TouchAMOLED1inch75"

static constexpr uint32_t PWR_BUTTON_INPUT_MASK = 1U << IO_EXPANDER_PIN_NUM_4;
static constexpr int64_t PWR_BUTTON_LONG_PRESS_US = 3 * 1000 * 1000;

// YGSoul display-only styling. Keep these values local to this board so other
// boards and the conversation state machine keep their existing behavior.
static constexpr uint32_t YGSOUL_UI_BACKGROUND = 0xF4EFF9;
static constexpr uint32_t YGSOUL_UI_TEXT = 0x4B355E;
static constexpr uint8_t YGSOUL_MOUTH_CLOSED = 0;
static constexpr int32_t YGSOUL_MOUTH_X = 119;
static constexpr int32_t YGSOUL_MOUTH_Y = 116;
static constexpr uint32_t YGSOUL_MOUTH_FRAME_DURATION_MS[] = {140, 120, 140};
static constexpr uint8_t YGSOUL_MOUTH_FRAME_COUNT = 3;
static_assert(
    YGSOUL_MOUTH_FRAME_COUNT ==
    sizeof(YGSOUL_MOUTH_FRAME_DURATION_MS) / sizeof(YGSOUL_MOUTH_FRAME_DURATION_MS[0]));

class Pmic : public Axp2101 {
public:
    Pmic(i2c_master_bus_handle_t i2c_bus, uint8_t addr) : Axp2101(i2c_bus, addr) {
        WriteReg(0x22, 0b110); // PWRON > OFFLEVEL as POWEROFF Source enable
        WriteReg(0x27, 0x1C);  // hold 10s to power off

        // Disable All DCs but DC1
        WriteReg(0x80, 0x01);
        // Disable All LDOs
        WriteReg(0x90, 0x00);
        WriteReg(0x91, 0x00);

        // Set DC1 to 3.3V
        WriteReg(0x82, (3300 - 1500) / 100);

        // Set ALDO1 to 3.3V
        WriteReg(0x92, (3300 - 500) / 100);

        // Enable ALDO1(MIC)
        WriteReg(0x90, 0x01);

        WriteReg(0x64, 0x02); // CV charger voltage setting to 4.1V

        WriteReg(0x61, 0x02); // set Main battery precharge current to 50mA
        WriteReg(0x62, 0x08); // set Main battery charger current to 400mA ( 0x08-200mA, 0x09-300mA, 0x0A-400mA )
        WriteReg(0x63, 0x01); // set Main battery term charge current to 25mA
    }
};

#define LCD_OPCODE_WRITE_CMD (0x02ULL)
#define LCD_OPCODE_READ_CMD (0x03ULL)
#define LCD_OPCODE_WRITE_COLOR (0x32ULL)

static const co5300_lcd_init_cmd_t vendor_specific_init[] = {
    // set display to qspi mode
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

// 在waveshare_amoled_1_75类之前添加新的显示类
class CustomLcdDisplay : public SpiLcdDisplay {
private:
    bool showing_boot_logo_ = true;
    bool ygsoul_mouth_speaking_ = false;
    bool ygsoul_chat_message_is_system_ = true;
    bool ygsoul_assets_ready_ = false;
    std::unique_ptr<LvglImage> ygsoul_companion_;
    std::array<std::unique_ptr<LvglImage>, YGSOUL_MOUTH_FRAME_COUNT> ygsoul_mouth_frames_;
    lv_obj_t* ygsoul_image_ = nullptr;
    lv_obj_t* ygsoul_mouth_image_ = nullptr;
    lv_obj_t* ygsoul_boot_image_ = nullptr;
    lv_timer_t* ygsoul_mouth_timer_ = nullptr;
    uint8_t ygsoul_mouth_frame_ = YGSOUL_MOUTH_CLOSED;

    static uint16_t ReadLe16(const uint8_t* value) {
        return static_cast<uint16_t>(value[0]) |
               (static_cast<uint16_t>(value[1]) << 8);
    }

    static uint32_t ReadLe32(const uint8_t* value) {
        return static_cast<uint32_t>(value[0]) |
               (static_cast<uint32_t>(value[1]) << 8) |
               (static_cast<uint32_t>(value[2]) << 16) |
               (static_cast<uint32_t>(value[3]) << 24);
    }

    std::unique_ptr<LvglImage> LoadYGSoulAsset(const char* name) {
        void* data = nullptr;
        size_t size = 0;
        constexpr size_t kHeaderSize = 16;
        if (!Assets::GetInstance().GetAssetData(name, data, size) ||
            data == nullptr || size < kHeaderSize) {
            ESP_LOGE(TAG, "YGSoul asset is missing or invalid: %s", name);
            return nullptr;
        }

        const auto* bytes = static_cast<const uint8_t*>(data);
        const uint8_t color_format = bytes[4];
        const uint16_t width = ReadLe16(bytes + 6);
        const uint16_t height = ReadLe16(bytes + 8);
        const uint16_t stride = ReadLe16(bytes + 10);
        const uint32_t payload_size = ReadLe32(bytes + 12);
        const bool supported_format =
            color_format == LV_COLOR_FORMAT_RGB565 ||
            color_format == LV_COLOR_FORMAT_RGB565A8;
        const uint32_t bytes_per_pixel =
            color_format == LV_COLOR_FORMAT_RGB565A8 ? 3U : 2U;
        const uint32_t expected_payload_size =
            static_cast<uint32_t>(width) * height * bytes_per_pixel;
        if (memcmp(bytes, "YGI1", 4) != 0 || bytes[5] != 0 ||
            !supported_format || width == 0 || height == 0 ||
            stride != static_cast<uint32_t>(width) * 2U ||
            payload_size != expected_payload_size ||
            payload_size != size - kHeaderSize) {
            ESP_LOGE(TAG, "YGSoul asset header is invalid: %s", name);
            return nullptr;
        }

        void* pixels = heap_caps_malloc(
            payload_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (pixels == nullptr) {
            ESP_LOGE(TAG, "Not enough PSRAM for YGSoul asset: %s", name);
            return nullptr;
        }
        memcpy(pixels, bytes + kHeaderSize, payload_size);
        return std::make_unique<LvglAllocatedImage>(
            pixels, payload_size, width, height, stride, color_format);
    }

    bool LoadYGSoulAssets() {
        auto companion = LoadYGSoulAsset("ygsoul_companion_nomouth.cbin");
        std::array<std::unique_ptr<LvglImage>, YGSOUL_MOUTH_FRAME_COUNT> mouths;
        mouths[0] = LoadYGSoulAsset("ygsoul_mouth_1.cbin");
        mouths[1] = LoadYGSoulAsset("ygsoul_mouth_2.cbin");
        mouths[2] = LoadYGSoulAsset("ygsoul_mouth_3.cbin");
        if (companion == nullptr ||
            mouths[0] == nullptr || mouths[1] == nullptr || mouths[2] == nullptr) {
            ESP_LOGE(TAG, "YGSoul assets are incomplete; using the lightweight display fallback");
            return false;
        }
        ygsoul_companion_ = std::move(companion);
        ygsoul_mouth_frames_ = std::move(mouths);
        ygsoul_assets_ready_ = true;
        ESP_LOGI(TAG, "Loaded all YGSoul CBin assets from the assets partition");
        return true;
    }

    void ApplyYGSoulChatMessageColor() {
        if (chat_message_label_ == nullptr) {
            return;
        }
        const auto color = ygsoul_chat_message_is_system_
            ? lv_color_white()
            : lv_color_hex(YGSOUL_UI_TEXT);
        lv_obj_set_style_text_color(chat_message_label_, color, 0);
    }

    void ApplyYGSoulOverlayStyle() {
        const auto text = showing_boot_logo_ ? lv_color_white() : lv_color_hex(YGSOUL_UI_TEXT);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(network_label_, text, 0);
        lv_obj_set_style_text_color(status_label_, text, 0);
        lv_obj_set_style_text_color(notification_label_, text, 0);
        lv_obj_set_style_text_color(mute_label_, text, 0);
        lv_obj_set_style_text_color(battery_label_, text, 0);
        lv_obj_set_style_text_color(emoji_label_, text, 0);
        ApplyYGSoulChatMessageColor();
    }

    void RaiseYGSoulChrome() {
        lv_obj_move_foreground(top_bar_);
        lv_obj_move_foreground(status_bar_);
        lv_obj_move_foreground(bottom_bar_);
    }

    void ApplyYGSoulTheme() {
        const auto background = lv_color_hex(YGSOUL_UI_BACKGROUND);
        auto screen = lv_screen_active();
        lv_obj_set_style_bg_color(screen, background, 0);
        lv_obj_set_style_bg_color(container_, background, 0);
        lv_obj_set_style_bg_opa(container_, LV_OPA_COVER, 0);
        ApplyYGSoulOverlayStyle();
    }

    void SetYGSoulMouthFrame(uint8_t frame) {
        if (!ygsoul_assets_ready_ || ygsoul_mouth_image_ == nullptr) {
            return;
        }
        ygsoul_mouth_frame_ = frame % YGSOUL_MOUTH_FRAME_COUNT;
        lv_image_set_src(
            ygsoul_mouth_image_,
            ygsoul_mouth_frames_[ygsoul_mouth_frame_]->image_dsc());
    }

    static void YGSoulMouthTimerCallback(lv_timer_t* timer) {
        auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr || !self->ygsoul_mouth_speaking_) {
            return;
        }
        const uint8_t next_frame =
            (self->ygsoul_mouth_frame_ + 1) % YGSOUL_MOUTH_FRAME_COUNT;
        self->SetYGSoulMouthFrame(next_frame);
        lv_timer_set_period(timer, YGSOUL_MOUTH_FRAME_DURATION_MS[next_frame]);
    }

    void StartYGSoulSpeakingAnimation() {
        if (ygsoul_mouth_image_ == nullptr || ygsoul_mouth_speaking_) {
            return;
        }
        SetYGSoulMouthFrame(YGSOUL_MOUTH_CLOSED);
        if (ygsoul_mouth_timer_ == nullptr) {
            ygsoul_mouth_timer_ = lv_timer_create(
                YGSoulMouthTimerCallback,
                YGSOUL_MOUTH_FRAME_DURATION_MS[YGSOUL_MOUTH_CLOSED],
                this);
            if (ygsoul_mouth_timer_ == nullptr) {
                ESP_LOGE(TAG, "Failed to create YGSoul mouth timer");
                return;
            }
        }
        ygsoul_mouth_speaking_ = true;
        lv_timer_set_period(
            ygsoul_mouth_timer_,
            YGSOUL_MOUTH_FRAME_DURATION_MS[YGSOUL_MOUTH_CLOSED]);
        lv_timer_reset(ygsoul_mouth_timer_);
        lv_timer_resume(ygsoul_mouth_timer_);
    }

    void StopYGSoulSpeakingAnimation() {
        ygsoul_mouth_speaking_ = false;
        if (ygsoul_mouth_timer_ != nullptr) {
            lv_timer_pause(ygsoul_mouth_timer_);
        }
        SetYGSoulMouthFrame(YGSOUL_MOUTH_CLOSED);
    }

    void ShowYGSoulCompanion() {
        if (ygsoul_image_ == nullptr || emoji_box_ == nullptr) {
            return;
        }

        showing_boot_logo_ = false;
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ygsoul_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ygsoul_mouth_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ygsoul_boot_image_, LV_OBJ_FLAG_HIDDEN);
        if (!ygsoul_mouth_speaking_) {
            SetYGSoulMouthFrame(YGSOUL_MOUTH_CLOSED);
        }
        RaiseYGSoulChrome();
        ApplyYGSoulOverlayStyle();
    }

    void ShowYGSoulBootLogo() {
        if (ygsoul_boot_image_ == nullptr || emoji_box_ == nullptr) {
            return;
        }

        showing_boot_logo_ = true;
        StopYGSoulSpeakingAnimation();
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ygsoul_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ygsoul_mouth_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ygsoul_boot_image_, LV_OBJ_FLAG_HIDDEN);
        RaiseYGSoulChrome();
        ApplyYGSoulOverlayStyle();
    }

public:
    static void rounder_event_cb(lv_event_t* e) {
        lv_area_t* area = (lv_area_t* )lv_event_get_param(e);
        uint16_t x1 = area->x1;
        uint16_t x2 = area->x2;

        uint16_t y1 = area->y1;
        uint16_t y2 = area->y2;

        // round the start of coordinate down to the nearest 2M number
        area->x1 = (x1 >> 1) << 1;
        area->y1 = (y1 >> 1) << 1;
        // round the end of coordinate up to the nearest 2N+1 number
        area->x2 = ((x2 >> 1) << 1) + 1;
        area->y2 = ((y2 >> 1) << 1) + 1;
    }

    CustomLcdDisplay(esp_lcd_panel_io_handle_t io_handle,
                     esp_lcd_panel_handle_t panel_handle,
                     int width,
                     int height,
                     int offset_x,
                     int offset_y,
                     bool mirror_x,
                     bool mirror_y,
                     bool swap_xy)
        : SpiLcdDisplay(io_handle, panel_handle,
                        width, height, offset_x, offset_y, mirror_x, mirror_y, swap_xy) {
        // Note: UI customization should be done in SetupUI(), not in constructor
        // to ensure lvgl objects are created before accessing them
    }

    virtual void SetupUI() override {
        // Call parent SetupUI() first to create all lvgl objects
        SpiLcdDisplay::SetupUI();

        DisplayLockGuard lock(this);
        lv_obj_set_style_pad_left(status_bar_, LV_HOR_RES*  0.1, 0);
        lv_obj_set_style_pad_right(status_bar_, LV_HOR_RES*  0.1, 0);
        ApplyYGSoulTheme();
        if (!LoadYGSoulAssets()) {
            showing_boot_logo_ = false;
            ApplyYGSoulOverlayStyle();
            lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
            return;
        }
        ygsoul_image_ = lv_image_create(lv_screen_active());
        lv_image_set_src(ygsoul_image_, ygsoul_companion_->image_dsc());
        lv_obj_center(ygsoul_image_);
        lv_obj_add_flag(ygsoul_image_, LV_OBJ_FLAG_HIDDEN);
        ygsoul_mouth_image_ = lv_image_create(lv_screen_active());
        lv_image_set_src(
            ygsoul_mouth_image_,
            ygsoul_mouth_frames_[YGSOUL_MOUTH_CLOSED]->image_dsc());
        lv_obj_align_to(
            ygsoul_mouth_image_, ygsoul_image_, LV_ALIGN_TOP_LEFT,
            YGSOUL_MOUTH_X, YGSOUL_MOUTH_Y);
        lv_obj_add_flag(ygsoul_mouth_image_, LV_OBJ_FLAG_HIDDEN);
        ygsoul_boot_image_ = lv_image_create(lv_screen_active());
        lv_image_set_src(ygsoul_boot_image_, &ygsoul_boot_lvgl);
        lv_obj_center(ygsoul_boot_image_);
        ShowYGSoulBootLogo();
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    virtual void SetEmotion(const char* emotion) override {
        if (emotion == nullptr || !IsSetupUICalled()) {
            return;
        }
        if (!ygsoul_assets_ready_) {
            LcdDisplay::SetEmotion(emotion);
            return;
        }
        DisplayLockGuard lock(this);
        const auto state = Application::GetInstance().GetDeviceState();
        if (state == kDeviceStateWifiConfiguring) {
            ShowYGSoulBootLogo();
            return;
        }
        const bool conversation_ui =
            state == kDeviceStateIdle ||
            state == kDeviceStateConnecting ||
            state == kDeviceStateListening ||
            state == kDeviceStateSpeaking;
        if (showing_boot_logo_ && !conversation_ui) {
            return;
        }
        ShowYGSoulCompanion();
        if (Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
            StartYGSoulSpeakingAnimation();
        } else {
            StopYGSoulSpeakingAnimation();
        }
    }

    virtual void SetStatus(const char* status) override {
        LcdDisplay::SetStatus(status);
        const bool speaking = Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking;
        if (IsSetupUICalled() && !showing_boot_logo_) {
            DisplayLockGuard lock(this);
            if (speaking) {
                StartYGSoulSpeakingAnimation();
            } else {
                StopYGSoulSpeakingAnimation();
            }
        }
    }

    virtual void SetChatMessage(const char* role, const char* content) override {
        LcdDisplay::SetChatMessage(role, content);
        DisplayLockGuard lock(this);
        ygsoul_chat_message_is_system_ = role != nullptr && strcmp(role, "system") == 0;
        ApplyYGSoulChatMessageColor();
        if (chat_message_label_ != nullptr && content != nullptr && content[0] != '\0') {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            RaiseYGSoulChrome();
        }
    }

    virtual void SetTheme(Theme* theme) override {
        LcdDisplay::SetTheme(theme);
        if (IsSetupUICalled()) {
            DisplayLockGuard lock(this);
            ApplyYGSoulOverlayStyle();
        }
    }
};

class CustomBacklight : public Backlight {
public:
    CustomBacklight(esp_lcd_panel_io_handle_t panel_io) : Backlight(), panel_io_(panel_io) {}

protected:
    esp_lcd_panel_io_handle_t panel_io_;

    virtual void SetBrightnessImpl(uint8_t brightness) override {
        auto display = Board::GetInstance().GetDisplay();
        DisplayLockGuard lock(display);
        uint8_t data[1] = {((uint8_t)((255*  brightness) / 100))};
        int lcd_cmd = 0x51;
        lcd_cmd &= 0xff;
        lcd_cmd <<= 8;
        lcd_cmd |= LCD_OPCODE_WRITE_CMD << 24;
        esp_lcd_panel_io_tx_param(panel_io_, lcd_cmd, &data, sizeof(data));
    }
};

class WaveshareEsp32s3TouchAMOLED1inch75 : public WifiBoard {
private:
    i2c_master_bus_handle_t i2c_bus_;
    Pmic* pmic_ = nullptr;
    Button boot_button_;
    CustomLcdDisplay* display_;
    CustomBacklight* backlight_;
    esp_io_expander_handle_t io_expander = NULL;
    PowerSaveTimer* power_save_timer_;
    esp_timer_handle_t pwr_button_timer_ = nullptr;
    bool pwr_button_pressed_ = false;
    bool pwr_button_long_pressed_ = false;
    bool super_power_save_ = false;
    int64_t pwr_button_pressed_at_us_ = 0;

    void InitializePowerSaveTimer() {
        // 保留 Wi-Fi / WSS 会话；该设备只进入低亮度省电模式，绝不由空闲计时器断电。
        power_save_timer_ = new PowerSaveTimer(-1, GetAutoSleepMinutes() * 60, -1);
        power_save_timer_->OnEnterSleepMode([this]() {
            EnterSuperPowerSave();
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->SetEnabled(true);
    }

    void EnterSuperPowerSave() {
        if (super_power_save_) {
            return;
        }
        super_power_save_ = true;
        GetDisplay()->SetPowerSaveMode(true);
        GetBacklight()->SetBrightness(1);
        auto& audio = Application::GetInstance().GetAudioService();
        // CRITICAL-RUNTIME-CONTRACT: super power save must not close Wi-Fi/WSS.
        // It only dims the display and leaves wake-word detection available.
        audio.EnableVoiceProcessing(false);
        audio.EnableWakeWordDetection(true);
        ESP_LOGI(TAG, "PWR: enter super power save");
    }

    void ExitSuperPowerSave() {
        if (!super_power_save_) {
            return;
        }
        super_power_save_ = false;
        power_save_timer_->WakeUp();
        GetDisplay()->SetPowerSaveMode(false);
        GetBacklight()->RestoreBrightness();
        auto& audio = Application::GetInstance().GetAudioService();
        audio.EnableVoiceProcessing(false);
        audio.EnableWakeWordDetection(true);
        ESP_LOGI(TAG, "PWR: exit super power save");
    }

    void HandlePwrButton() {
        if (io_expander == NULL) {
            return;
        }
        uint32_t levels = 0;
        if (esp_io_expander_get_level(io_expander, PWR_BUTTON_INPUT_MASK, &levels) != ESP_OK) {
            ESP_LOGW(TAG, "PWR: failed to read EXIO4");
            return;
        }
        // EXIO4 is active-low on this board; invert only this read, not the PWR semantics.
        const bool pressed = (levels & PWR_BUTTON_INPUT_MASK) == 0;
        const int64_t now_us = esp_timer_get_time();
        if (pressed && !pwr_button_pressed_) {
            pwr_button_pressed_ = true;
            pwr_button_long_pressed_ = false;
            pwr_button_pressed_at_us_ = now_us;
            return;
        }
        if (pressed && !pwr_button_long_pressed_ && now_us - pwr_button_pressed_at_us_ >= PWR_BUTTON_LONG_PRESS_US) {
            pwr_button_long_pressed_ = true;
            ESP_LOGI(TAG, "PWR long press 3s, enter super power save raw=0x%lx", (unsigned long)levels);
            EnterSuperPowerSave();
            return;
        }
        if (!pressed && pwr_button_pressed_) {
            if (!pwr_button_long_pressed_) {
                ESP_LOGI(TAG, "PWR short press, toggle super power save raw=0x%lx", (unsigned long)levels);
                if (super_power_save_) {
                    ExitSuperPowerSave();
                } else {
                    EnterSuperPowerSave();
                }
            }
            pwr_button_pressed_ = false;
        }
    }

    void InitializePwrButton() {
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                static_cast<WaveshareEsp32s3TouchAMOLED1inch75*>(arg)->HandlePwrButton();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "pwr_btn",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &pwr_button_timer_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(pwr_button_timer_, 20 * 1000));
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &i2c_bus_));
    }

    void InitializeTca9554(void) {
        io_expander = NULL;
        const uint32_t addrs[] = {
            ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000,
            ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_001,
            ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_010,
            ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_011,
            ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_000,
            ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_001,
            ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_010,
            ESP_IO_EXPANDER_I2C_TCA9554A_ADDRESS_011,
        };
        for (uint32_t addr : addrs) {
            if (i2c_master_probe(i2c_bus_, addr, 80) != ESP_OK) {
                continue;
            }
            ESP_LOGI(TAG, "I2C ack at 0x%02lx, try TCA9554", (unsigned long)addr);
            esp_err_t ret = esp_io_expander_new_i2c_tca9554(i2c_bus_, addr, &io_expander);
            if (ret != ESP_OK || io_expander == NULL) {
                io_expander = NULL;
                continue;
            }
            ret = esp_io_expander_set_dir(io_expander, IO_EXPANDER_PIN_NUM_4, IO_EXPANDER_INPUT);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "TCA9554 EXIO4 dir failed at 0x%02lx", (unsigned long)addr);
                io_expander = NULL;
                continue;
            }
            ESP_LOGI(TAG, "PWR EXIO4 ready at 0x%02lx", (unsigned long)addr);
            return;
        }
        ESP_LOGW(TAG, "TCA9554 not found on I2C, PWR software path unavailable");
    }

    void InitializeAxp2101() {
        ESP_LOGI(TAG, "Init AXP2101");
        pmic_ = new Pmic(i2c_bus_, 0x34);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.sclk_io_num = EXAMPLE_PIN_NUM_LCD_PCLK;
        buscfg.data0_io_num = EXAMPLE_PIN_NUM_LCD_DATA0;
        buscfg.data1_io_num = EXAMPLE_PIN_NUM_LCD_DATA1;
        buscfg.data2_io_num = EXAMPLE_PIN_NUM_LCD_DATA2;
        buscfg.data3_io_num = EXAMPLE_PIN_NUM_LCD_DATA3;
        buscfg.max_transfer_sz = DISPLAY_WIDTH*  DISPLAY_HEIGHT*  sizeof(uint16_t);
        buscfg.flags = SPICOMMON_BUSFLAG_QUAD;
        ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            ESP_LOGI(TAG, "BOOT short press");
            auto& app = Application::GetInstance();
            if (super_power_save_) {
                ExitSuperPowerSave();
            }
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT long press 3s, enter wifi config");
            EnterWifiConfigMode();
        });

#if CONFIG_USE_DEVICE_AEC
        boot_button_.OnDoubleClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateIdle) {
                app.SetAecMode(app.GetAecMode() == kAecOff ? kAecOnDeviceSide : kAecOff);
            }
        });
#endif
    }

    void InitializeDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(
            EXAMPLE_PIN_NUM_LCD_CS,
            nullptr,
            nullptr);
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        const co5300_vendor_config_t vendor_config = {
            .init_cmds = &vendor_specific_init[0],
            .init_cmds_size = sizeof(vendor_specific_init) / sizeof(co5300_lcd_init_cmd_t),
            .flags = {
                .use_qspi_interface = 1,
            }};

        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.vendor_config = (void* )&vendor_config;
        ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(panel_io, &panel_config, &panel));
        esp_lcd_panel_set_gap(panel, 0x06, 0);
        esp_lcd_panel_reset(panel);
        esp_lcd_panel_init(panel);
        esp_lcd_panel_invert_color(panel, false);
        esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        esp_lcd_panel_disp_on_off(panel, true);
        display_ = new CustomLcdDisplay(panel_io, panel,
                                        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        backlight_ = new CustomBacklight(panel_io);
        backlight_->RestoreBrightness();
    }

    void InitializeTouch() {
        esp_lcd_touch_handle_t tp;
        esp_lcd_touch_config_t tp_cfg = {
            .x_max = DISPLAY_WIDTH - 1,
            .y_max = DISPLAY_HEIGHT - 1,
            .rst_gpio_num = PIN_NUM_TOUCH_RST,
            .int_gpio_num = PIN_NUM_TOUCH_INT,
            .levels = {
                .reset = 0,
                .interrupt = 0,
            },
            .flags = {
                .swap_xy = 0,
                .mirror_x = 1,
                .mirror_y = 1,
            },
        };
        esp_lcd_panel_io_handle_t tp_io_handle = NULL;
        esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
        tp_io_config.scl_speed_hz = 400*  1000;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus_, &tp_io_config, &tp_io_handle));
        ESP_LOGI(TAG, "Initialize touch controller");
        ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst9217(tp_io_handle, &tp_cfg, &tp));
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = lv_display_get_default(),
            .handle = tp,
        };
        lvgl_port_add_touch(&touch_cfg);
        ESP_LOGI(TAG, "Touch panel initialized successfully");
    }

public:
    WaveshareEsp32s3TouchAMOLED1inch75() : boot_button_(BOOT_BUTTON_GPIO, false, 3000) {
        InitializePowerSaveTimer();
        InitializeCodecI2c();
        InitializeTca9554();
        InitializeAxp2101();
        InitializeSpi();
        InitializeDisplay();
        InitializeTouch();
        InitializeButtons();
        InitializePwrButton();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BoxAudioCodec audio_codec(
            i2c_bus_, 
            AUDIO_INPUT_SAMPLE_RATE, 
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, 
            AUDIO_I2S_GPIO_BCLK, 
            AUDIO_I2S_GPIO_WS, 
            AUDIO_I2S_GPIO_DOUT, 
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, 
            AUDIO_CODEC_ES8311_ADDR, 
            AUDIO_CODEC_ES7210_ADDR, 
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool SetAutoSleepMinutes(int minutes) override {
        if (power_save_timer_ == nullptr ||
            (minutes != 1 && minutes != 5 && minutes != 15 && minutes != 30)) {
            return false;
        }
        Settings settings("power", true);
        settings.SetInt("auto_sleep", minutes);
        power_save_timer_->SetSleepTimeout(minutes * 60);
        ESP_LOGI(TAG, "PWR: auto sleep set to %d minutes", minutes);
        return true;
    }

    virtual int GetAutoSleepMinutes() override {
        Settings settings("power", false);
        const int minutes = settings.GetInt("auto_sleep", 1);
        return (minutes == 1 || minutes == 5 || minutes == 15 || minutes == 30) ? minutes : 1;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = pmic_->IsCharging();
        discharging = pmic_->IsDischarging();
        if (discharging != last_discharging)
        {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }

        level = pmic_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch75);
