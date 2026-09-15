#include "wifi_board.h"
#include "display/lcd_display.h"
#include "display/lvgl_display/jpg/jpeg_to_image.h"
#include "display/lvgl_display/lvgl_image.h"
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
#include "ui/ygsoul_launcher_state.h"
#include "music/music_client.h"
#include "device_content/gallery_store.h"
#include "watch/watch_face_store.h"
#include "watch/watch_clock_math.h"

#include <esp_lcd_touch_cst9217.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>
#include <lv_eaf.h>
#include <font_awesome.h>

#include <array>
#include <atomic>
#include <ctime>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/time.h>

#define TAG "WaveshareEsp32s3TouchAMOLED1inch75"
LV_FONT_DECLARE(font_awesome_30_4);
LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_puhui_20_4);
extern const uint8_t default_watch_face_start[] asm("_binary_default_watch_face_jpg_start");
extern const uint8_t default_watch_face_end[] asm("_binary_default_watch_face_jpg_end");

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

class Qmi8658Pedometer {
public:
    explicit Qmi8658Pedometer(i2c_master_bus_handle_t bus) : ready_(Initialize(bus)) {}

    bool Read(uint32_t& steps) {
        uint8_t value[3]{};
        if (!ready_ || !ReadRegisters(QMI8658_STEP_CNT_LOW, value, sizeof(value))) return false;
        steps = static_cast<uint32_t>(value[0]) |
                (static_cast<uint32_t>(value[1]) << 8) |
                (static_cast<uint32_t>(value[2]) << 16);
        return true;
    }

private:
    // Waveshare/SensorLib QMI8658 pedometer example parameters; tune only after measured hardware calibration.
    static constexpr uint8_t QMI8658_ADDRESS = 0x6B;
    static constexpr uint8_t QMI8658_WHO_AM_I = 0x00;
    static constexpr uint8_t QMI8658_CTRL1 = 0x02;
    static constexpr uint8_t QMI8658_CTRL2 = 0x03;
    static constexpr uint8_t QMI8658_CTRL7 = 0x08;
    static constexpr uint8_t QMI8658_CTRL8 = 0x09;
    static constexpr uint8_t QMI8658_CTRL9 = 0x0A;
    static constexpr uint8_t QMI8658_CAL1_L = 0x0B;
    static constexpr uint8_t QMI8658_STATUS_INT = 0x2D;
    static constexpr uint8_t QMI8658_RESET_RESULT = 0x4D;
    static constexpr uint8_t QMI8658_STEP_CNT_LOW = 0x5A;
    static constexpr uint8_t QMI8658_RESET = 0x60;
    i2c_master_dev_handle_t device_ = nullptr;
    bool ready_ = false;

    bool WriteRegister(uint8_t reg, uint8_t value) {
        const uint8_t payload[] = {reg, value};
        return i2c_master_transmit(device_, payload, sizeof(payload), 100) == ESP_OK;
    }

    bool ReadRegisters(uint8_t reg, uint8_t* value, size_t size) {
        return i2c_master_transmit_receive(device_, &reg, 1, value, size, 100) == ESP_OK;
    }

    bool UpdateBits(uint8_t reg, uint8_t mask, uint8_t value) {
        uint8_t current = 0;
        return ReadRegisters(reg, &current, 1) &&
               WriteRegister(reg, static_cast<uint8_t>((current & ~mask) | (value & mask)));
    }

    bool WriteCommand(uint8_t command) {
        if (!WriteRegister(QMI8658_CTRL9, command)) return false;
        for (int attempt = 0; attempt < 100; ++attempt) {
            uint8_t status = 0;
            if (ReadRegisters(QMI8658_STATUS_INT, &status, 1) && (status & 0x80)) {
                if (!WriteRegister(QMI8658_CTRL9, 0x00)) return false;
                for (int clear_attempt = 0; clear_attempt < 100; ++clear_attempt) {
                    if (ReadRegisters(QMI8658_STATUS_INT, &status, 1) && !(status & 0x80)) return true;
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        return false;
    }

    bool WritePedometerPage(uint16_t first, uint16_t second, uint16_t third, uint16_t fourth) {
        return WriteRegister(QMI8658_CAL1_L + 0, first & 0xFF) &&
               WriteRegister(QMI8658_CAL1_L + 1, first >> 8) &&
               WriteRegister(QMI8658_CAL1_L + 2, second & 0xFF) &&
               WriteRegister(QMI8658_CAL1_L + 3, second >> 8) &&
               WriteRegister(QMI8658_CAL1_L + 4, third & 0xFF) &&
               WriteRegister(QMI8658_CAL1_L + 5, third >> 8) &&
               WriteRegister(QMI8658_CAL1_L + 6, fourth & 0xFF) &&
               WriteRegister(QMI8658_CAL1_L + 7, fourth >> 8) &&
               WriteCommand(0x0D);
    }

    bool Initialize(i2c_master_bus_handle_t bus) {
        if (i2c_master_probe(bus, QMI8658_ADDRESS, 100) != ESP_OK) {
            ESP_LOGW(TAG, "QMI8658 not found; watch steps unavailable");
            return false;
        }
        const i2c_device_config_t config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = QMI8658_ADDRESS,
            .scl_speed_hz = 400 * 1000,
        };
        if (i2c_master_bus_add_device(bus, &config, &device_) != ESP_OK ||
            !WriteRegister(QMI8658_RESET, 0xB0)) return false;

        bool reset_done = false;
        for (int attempt = 0; attempt < 50; ++attempt) {
            uint8_t result = 0;
            if (ReadRegisters(QMI8658_RESET_RESULT, &result, 1) && result == 0x80) {
                reset_done = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        uint8_t who_am_i = 0;
        if (!reset_done || !ReadRegisters(QMI8658_WHO_AM_I, &who_am_i, 1) || who_am_i != 0x05) return false;

        const bool configured =
            UpdateBits(QMI8658_CTRL1, 0x40, 0x40) &&
            WriteRegister(QMI8658_CTRL8, 0x80) &&
            UpdateBits(QMI8658_CTRL7, 0x80, 0x00) &&
            WriteRegister(QMI8658_CTRL2, 0x07) &&
            UpdateBits(QMI8658_CTRL7, 0x03, 0x00) &&
            WritePedometerPage(50, 200, 100, 0x0102) &&
            WritePedometerPage(200, 20 | (10 << 8), 0 | (4 << 8), 0x0202) &&
            UpdateBits(QMI8658_CTRL7, 0x01, 0x01) &&
            UpdateBits(QMI8658_CTRL8, 0x10, 0x10);
        ESP_LOGI(TAG, "QMI8658 pedometer %s", configured ? "ready" : "unavailable");
        return configured;
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
    YGSoulLauncherState launcher_state_{0};
    lv_obj_t* launcher_panel_ = nullptr;
    lv_obj_t* launcher_transition_image_ = nullptr;
    lv_draw_buf_t launcher_transition_buffer_{};
    void* launcher_transition_pixels_ = nullptr;
    bool launcher_transitioning_ = false;
    lv_obj_t* launcher_title_ = nullptr;
    lv_obj_t* launcher_content_ = nullptr;
    std::array<lv_obj_t*, 4> launcher_buttons_{};
    lv_timer_t* gallery_timer_ = nullptr;
    lv_timer_t* music_progress_timer_ = nullptr;
    lv_timer_t* watch_timer_ = nullptr;
    lv_obj_t* watch_time_label_ = nullptr;
    lv_obj_t* watch_date_label_ = nullptr;
    lv_obj_t* watch_battery_label_ = nullptr;
    lv_obj_t* watch_steps_label_ = nullptr;
    lv_obj_t* watch_hour_hand_ = nullptr;
    lv_obj_t* watch_minute_hand_ = nullptr;
    lv_obj_t* watch_second_hand_ = nullptr;
    std::array<lv_point_precise_t, 2> watch_hour_points_{};
    std::array<lv_point_precise_t, 2> watch_minute_points_{};
    std::array<lv_point_precise_t, 2> watch_second_points_{};
    std::unique_ptr<LvglImage> watch_face_asset_;
    time_t watch_last_hand_second_ = -1;
    time_t watch_last_data_second_ = -1;
    lv_obj_t* gallery_image_ = nullptr;
    lv_obj_t* gallery_mouth_ = nullptr;
    lv_obj_t* gallery_delete_ = nullptr;
    std::unique_ptr<LvglImage> gallery_asset_;
    void* gallery_asset_bytes_ = nullptr;
    lv_obj_t* brightness_slider_ = nullptr;
    lv_obj_t* music_progress_slider_ = nullptr;
    lv_obj_t* music_elapsed_label_ = nullptr;
    std::array<lv_obj_t*, 9> music_bars_{};
    uint8_t gallery_frame_ = 0;
    uint8_t music_wave_phase_ = 0;
    uint8_t music_index_ = 0;
    std::unique_ptr<MusicClient> music_client_;
    bool showing_boot_logo_ = true;
    bool ygsoul_mouth_speaking_ = false;
    bool ygsoul_chat_message_is_system_ = true;
    bool ygsoul_assets_ready_ = false;
    bool custom_role_image_ = false;
    std::unique_ptr<LvglImage> ygsoul_companion_;
    std::unique_ptr<LvglImage> ygsoul_role_animation_asset_;
    void* ygsoul_role_animation_bytes_ = nullptr;
    lv_obj_t* ygsoul_role_animation_ = nullptr;
    std::unique_ptr<LvglImage> ygsoul_music_cover_;
    std::array<std::unique_ptr<LvglImage>, YGSOUL_MOUTH_FRAME_COUNT> ygsoul_mouth_frames_;
    lv_obj_t* ygsoul_image_ = nullptr;
    lv_obj_t* ygsoul_mouth_image_ = nullptr;
    lv_obj_t* ygsoul_boot_image_ = nullptr;
    lv_timer_t* ygsoul_mouth_timer_ = nullptr;
    uint8_t ygsoul_mouth_frame_ = YGSOUL_MOUTH_CLOSED;

    static void SetPanelY(void* object, int32_t value) {
        lv_obj_set_y(static_cast<lv_obj_t*>(object), value);
    }

    static void SetPanelX(void* object, int32_t value) {
        lv_obj_set_x(static_cast<lv_obj_t*>(object), value);
    }

    void ReleaseLauncherTransition(bool show_panel) {
        if (launcher_transition_image_ != nullptr) {
            lv_obj_delete(launcher_transition_image_);
            launcher_transition_image_ = nullptr;
        }
        if (launcher_transition_pixels_ != nullptr) {
            heap_caps_free(launcher_transition_pixels_);
            launcher_transition_pixels_ = nullptr;
        }
        launcher_transition_buffer_ = {};
        launcher_transitioning_ = false;
        if (launcher_panel_ != nullptr) {
            if (show_panel) {
                lv_obj_remove_flag(launcher_panel_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_move_foreground(launcher_panel_);
            } else {
                lv_obj_add_flag(launcher_panel_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    static void LauncherTransitionCompleted(lv_anim_t* animation) {
        auto self = static_cast<CustomLcdDisplay*>(lv_anim_get_user_data(animation));
        if (self == nullptr) return;
        const bool show_panel = (Application::GetInstance().GetDeviceState() == kDeviceStateIdle ||
                                 self->launcher_state_.page() == YGSoulPage::kWatch) &&
                                self->launcher_state_.page() != YGSoulPage::kDesktop;
        self->ReleaseLauncherTransition(show_panel);
    }

    void AnimateLauncher(int32_t from_y) {
        constexpr uint32_t kStride = DISPLAY_WIDTH * sizeof(uint16_t);
        constexpr size_t kBufferSize = kStride * DISPLAY_HEIGHT;
        launcher_transition_pixels_ = heap_caps_aligned_alloc(
            64, kBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (launcher_transition_pixels_ == nullptr ||
            lv_draw_buf_init(&launcher_transition_buffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                             LV_COLOR_FORMAT_RGB565, kStride, launcher_transition_pixels_,
                             kBufferSize) != LV_RESULT_OK) {
            ReleaseLauncherTransition(true);
            return;
        }
        lv_obj_update_layout(launcher_panel_);
        if (lv_snapshot_take_to_draw_buf(launcher_panel_, LV_COLOR_FORMAT_RGB565,
                                         &launcher_transition_buffer_) != LV_RESULT_OK) {
            ReleaseLauncherTransition(true);
            return;
        }
        launcher_transition_image_ = lv_image_create(lv_screen_active());
        lv_image_set_src(launcher_transition_image_, &launcher_transition_buffer_);
        lv_obj_set_pos(launcher_transition_image_, 0, from_y);
        lv_obj_add_flag(launcher_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(launcher_transition_image_);
        launcher_transitioning_ = true;

        lv_anim_t animation;
        lv_anim_init(&animation);
        lv_anim_set_var(&animation, launcher_transition_image_);
        lv_anim_set_exec_cb(&animation, SetPanelY);
        lv_anim_set_values(&animation, from_y, 0);
        lv_anim_set_duration(&animation, 260);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
        lv_anim_set_user_data(&animation, this);
        lv_anim_set_completed_cb(&animation, LauncherTransitionCompleted);
        lv_anim_start(&animation);
    }

    void StopGalleryAnimation() {
        if (gallery_timer_ != nullptr) {
            lv_timer_delete(gallery_timer_);
            gallery_timer_ = nullptr;
        }
        if (gallery_image_ != nullptr) {
            lv_obj_delete(gallery_image_);
            gallery_image_ = nullptr;
        }
        gallery_mouth_ = nullptr;
        gallery_delete_ = nullptr;
        gallery_asset_.reset();
        if (gallery_asset_bytes_ != nullptr) {
            heap_caps_free(gallery_asset_bytes_);
            gallery_asset_bytes_ = nullptr;
        }
    }

    bool LoadGalleryAsset(const GalleryStore::Item& item) {
        FILE* file = fopen(item.path.c_str(), "rb");
        if (!file || fseek(file, 0, SEEK_END) != 0) { if (file) fclose(file); return false; }
        const long length = ftell(file);
        if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
        gallery_asset_bytes_ = heap_caps_malloc(static_cast<size_t>(length), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!gallery_asset_bytes_ || fread(gallery_asset_bytes_, 1, length, file) != static_cast<size_t>(length)) {
            fclose(file);
            if (gallery_asset_bytes_) heap_caps_free(gallery_asset_bytes_);
            gallery_asset_bytes_ = nullptr;
            return false;
        }
        fclose(file);
        if (item.format == "gif" || item.format == "eaf") {
            gallery_asset_ = std::make_unique<LvglRawImage>(gallery_asset_bytes_, static_cast<size_t>(length));
            return item.format == "eaf" || gallery_asset_->IsGif();
        }
        uint8_t* pixels = nullptr;
        size_t pixel_bytes = 0;
        size_t width = 0;
        size_t height = 0;
        size_t stride = 0;
        if (jpeg_to_image(static_cast<uint8_t*>(gallery_asset_bytes_), static_cast<size_t>(length),
                          &pixels, &pixel_bytes, &width, &height, &stride) != ESP_OK) {
            return false;
        }
        heap_caps_free(gallery_asset_bytes_);
        gallery_asset_bytes_ = nullptr;
        gallery_asset_ = std::make_unique<LvglAllocatedImage>(
            pixels, pixel_bytes, static_cast<int>(width), static_cast<int>(height),
            static_cast<int>(stride), LV_COLOR_FORMAT_RGB565);
        return true;
    }

    void SwitchGalleryHorizontal(bool next) {
        constexpr uint32_t kStride = DISPLAY_WIDTH * sizeof(uint16_t);
        constexpr size_t kBufferSize = kStride * DISPLAY_HEIGHT;
        launcher_transition_pixels_ = heap_caps_aligned_alloc(
            64, kBufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (launcher_transition_pixels_ == nullptr ||
            lv_draw_buf_init(&launcher_transition_buffer_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                             LV_COLOR_FORMAT_RGB565, kStride, launcher_transition_pixels_,
                             kBufferSize) != LV_RESULT_OK ||
            lv_snapshot_take_to_draw_buf(launcher_panel_, LV_COLOR_FORMAT_RGB565,
                                         &launcher_transition_buffer_) != LV_RESULT_OK) {
            ReleaseLauncherTransition(true);
            launcher_state_.Gesture(next ? YGSoulGesture::kLeft : YGSoulGesture::kRight);
            RenderLauncher();
            return;
        }
        launcher_transition_image_ = lv_image_create(lv_screen_active());
        lv_image_set_src(launcher_transition_image_, &launcher_transition_buffer_);
        lv_obj_set_pos(launcher_transition_image_, 0, 0);
        launcher_transitioning_ = true;

        launcher_state_.Gesture(next ? YGSoulGesture::kLeft : YGSoulGesture::kRight);
        RenderLauncher();
        const int32_t direction = next ? 1 : -1;
        lv_obj_set_x(launcher_content_, direction * DISPLAY_WIDTH);
        lv_obj_move_foreground(launcher_transition_image_);

        lv_anim_t incoming;
        lv_anim_init(&incoming);
        lv_anim_set_var(&incoming, launcher_content_);
        lv_anim_set_exec_cb(&incoming, SetPanelX);
        lv_anim_set_values(&incoming, direction * DISPLAY_WIDTH, 0);
        lv_anim_set_duration(&incoming, 220);
        lv_anim_set_path_cb(&incoming, lv_anim_path_ease_out);
        lv_anim_start(&incoming);

        lv_anim_t outgoing;
        lv_anim_init(&outgoing);
        lv_anim_set_var(&outgoing, launcher_transition_image_);
        lv_anim_set_exec_cb(&outgoing, SetPanelX);
        lv_anim_set_values(&outgoing, 0, -direction * DISPLAY_WIDTH);
        lv_anim_set_duration(&outgoing, 220);
        lv_anim_set_path_cb(&outgoing, lv_anim_path_ease_out);
        lv_anim_set_user_data(&outgoing, this);
        lv_anim_set_completed_cb(&outgoing, LauncherTransitionCompleted);
        lv_anim_start(&outgoing);
    }

    void StopMusicProgress() {
        if (music_progress_timer_ != nullptr) {
            lv_timer_delete(music_progress_timer_);
            music_progress_timer_ = nullptr;
        }
        music_progress_slider_ = nullptr;
        music_elapsed_label_ = nullptr;
        music_bars_.fill(nullptr);
    }

    void StopWatchClock() {
        if (watch_timer_ != nullptr) {
            lv_timer_delete(watch_timer_);
            watch_timer_ = nullptr;
        }
        watch_time_label_ = nullptr;
        watch_date_label_ = nullptr;
        watch_battery_label_ = nullptr;
        watch_steps_label_ = nullptr;
        watch_hour_hand_ = nullptr;
        watch_minute_hand_ = nullptr;
        watch_second_hand_ = nullptr;
        watch_last_hand_second_ = -1;
        watch_last_data_second_ = -1;
    }

    bool DecodeWatchFaceAsset(const uint8_t* source, size_t length) {
        if (!source || length == 0) return false;
        uint8_t* pixels = nullptr;
        size_t pixel_bytes = 0, width = 0, height = 0, stride = 0;
        const bool decoded = jpeg_to_image(source, length, &pixels, &pixel_bytes,
                                           &width, &height, &stride) == ESP_OK;
        if (!decoded || width != 466 || height != 466) {
            if (pixels) heap_caps_free(pixels);
            return false;
        }
        watch_face_asset_ = std::make_unique<LvglAllocatedImage>(
            pixels, pixel_bytes, static_cast<int>(width), static_cast<int>(height),
            static_cast<int>(stride), LV_COLOR_FORMAT_RGB565);
        return true;
    }

    bool LoadWatchFaceAsset(const std::string& path) {
        FILE* file = fopen(path.c_str(), "rb");
        if (!file || fseek(file, 0, SEEK_END) != 0) { if (file) fclose(file); return false; }
        const long length = ftell(file);
        if (length <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return false; }
        auto source = static_cast<uint8_t*>(heap_caps_malloc(
            static_cast<size_t>(length), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!source || fread(source, 1, length, file) != static_cast<size_t>(length)) {
            fclose(file);
            if (source) heap_caps_free(source);
            return false;
        }
        fclose(file);
        const bool decoded = DecodeWatchFaceAsset(source, static_cast<size_t>(length));
        heap_caps_free(source);
        return decoded;
    }

    void UpdateWatchHand(lv_obj_t* line, std::array<lv_point_precise_t, 2>& points,
                         double turns, int length) {
        if (!line) return;
        constexpr double kPi = 3.14159265358979323846;
        const double angle = turns * 2.0 * kPi - kPi / 2.0;
        points[0] = {233, 233};
        points[1] = {
            static_cast<lv_value_precise_t>(233 + std::cos(angle) * length),
            static_cast<lv_value_precise_t>(233 + std::sin(angle) * length),
        };
        lv_line_set_points_mutable(line, points.data(), points.size());
        lv_obj_invalidate(line);
    }

    void StartMusicClient() {
        if (!music_client_) {
            music_client_ = std::make_unique<MusicClient>();
            auto redraw = [this]() {
                Application::GetInstance().Schedule([this]() {
                    const auto page = launcher_state_.page();
                    if (page != YGSoulPage::kMusicPlaylist && page != YGSoulPage::kMusicPlayer) return;
                    DisplayLockGuard lock(this);
                    if (lock) RenderLauncher();
                });
            };
            music_client_->OnPlaylist(redraw);
            music_client_->OnChanged(redraw);
        }
        music_client_->Start();
    }

    static void FormatMusicTime(char* output, size_t size, uint32_t milliseconds) {
        snprintf(output, size, "%02lu:%02lu", static_cast<unsigned long>(milliseconds / 60000),
                 static_cast<unsigned long>(milliseconds / 1000 % 60));
    }

    lv_obj_t* CreateLauncherButton(const char* text, int x, int y, int width, int height) {
        auto button = lv_button_create(launcher_content_);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_size(button, width, height);
        lv_obj_set_style_radius(button, 26, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x4E2C91), 0);
        lv_obj_set_style_bg_grad_color(button, lv_color_hex(0x7961C9), 0);
        lv_obj_set_style_bg_grad_dir(button, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_80, 0);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, lv_color_hex(0xA99BFF), 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_shadow_color(button, lv_color_hex(0x6D42FF), 0);
        lv_obj_set_style_shadow_opa(button, LV_OPA_40, 0);
        lv_obj_add_flag(button, LV_OBJ_FLAG_GESTURE_BUBBLE);
        auto label = lv_label_create(button);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        return button;
    }

    lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, int x, int y, int width,
                          lv_color_t color, lv_text_align_t align = LV_TEXT_ALIGN_CENTER) {
        auto label = lv_label_create(parent);
        lv_label_set_text(label, text);
        lv_obj_set_pos(label, x, y);
        lv_obj_set_width(label, width);
        lv_obj_set_style_text_color(label, color, 0);
        lv_obj_set_style_text_align(label, align, 0);
        return label;
    }

    lv_obj_t* CreateSmallLabel(lv_obj_t* parent, const char* text, int x, int y, int width,
                               lv_color_t color, lv_text_align_t align = LV_TEXT_ALIGN_CENTER) {
        auto label = CreateLabel(parent, text, x, y, width, color, align);
        lv_obj_set_style_text_font(label, &font_puhui_20_4, 0);
        return label;
    }

    lv_obj_t* CreateWatchRegionLabel(const char* text, int x, int y, int width, int height,
                                     lv_color_t color, const lv_font_t* font = nullptr) {
        auto region = lv_obj_create(launcher_content_);
        lv_obj_remove_style_all(region);
        lv_obj_set_pos(region, x, y);
        lv_obj_set_size(region, width, height);
        lv_obj_remove_flag(region, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(region, LV_OBJ_FLAG_SCROLLABLE);
        auto label = lv_label_create(region);
        lv_label_set_text(label, text);
        lv_obj_set_width(label, width);
        lv_obj_set_style_text_color(label, color, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        if (font != nullptr) lv_obj_set_style_text_font(label, font, 0);
        lv_obj_center(label);
        return label;
    }

    lv_obj_t* CreateIcon(lv_obj_t* parent, const char* icon, int x, int y, int width,
                         lv_color_t color) {
        auto label = CreateLabel(parent, icon, x, y, width, color);
        lv_obj_set_style_text_font(label, &font_awesome_30_4, 0);
        return label;
    }

    void AddStars() {
        static constexpr int points[][3] = {
            {94, 102, 3}, {362, 110, 2}, {52, 212, 2}, {405, 224, 3},
            {78, 348, 2}, {386, 364, 2}, {133, 414, 2}, {327, 74, 2},
            {170, 96, 1}, {302, 397, 1}, {421, 302, 1}, {45, 286, 1},
        };
        for (const auto& point : points) {
            auto star = lv_obj_create(launcher_content_);
            lv_obj_set_pos(star, point[0], point[1]);
            lv_obj_set_size(star, point[2], point[2]);
            lv_obj_set_style_radius(star, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(star, lv_color_hex(0xB9D8FF), 0);
            lv_obj_set_style_border_width(star, 0, 0);
            lv_obj_set_style_shadow_width(star, 0, 0);
            lv_obj_set_style_shadow_color(star, lv_color_hex(0x724BFF), 0);
        }
    }

    static void FeatureClickCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        auto target = lv_event_get_target_obj(event);
        for (size_t index = 0; index < self->launcher_buttons_.size(); ++index) {
            if (target == self->launcher_buttons_[index]) {
                self->launcher_state_.OpenFeature(
                    self->launcher_state_.feature_page() == 0 ? index : 4);
                if (self->launcher_state_.page() == YGSoulPage::kGallery) {
                    self->launcher_state_.SetGalleryCount(GalleryStore::GetInstance().Count());
                }
                if (self->launcher_state_.page() == YGSoulPage::kChat) {
                    self->launcher_state_.Home();
                    self->HideLauncher();
                } else {
                    if (self->launcher_state_.page() == YGSoulPage::kMusic) {
                        self->StartMusicClient();
                    }
                    self->RenderLauncher();
                }
                return;
            }
        }
    }

    static void BackClickCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        self->launcher_state_.Back();
        if (self->launcher_state_.page() == YGSoulPage::kDesktop) self->HideLauncher();
        else self->RenderLauncher();
    }

    static void MusicClickCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (!self->music_client_) return;
        const auto tracks = self->music_client_->GetTracks();
        if (tracks.empty()) return;
        self->music_index_ = static_cast<uint8_t>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event))));
        self->launcher_state_.SetMusicIndex(self->music_index_);
        self->music_client_->Select(self->music_index_);
        self->launcher_state_.OpenMusicPlayer();
        self->music_client_->PlaySelected();
        self->RenderLauncher();
    }

    static void MusicPlayCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (!self->music_client_ || !self->music_client_->TogglePlayback()) {
            ESP_LOGW(TAG, "Music playback request rejected: playlist empty or music channel offline");
        }
    }

    static void MusicStepCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (!self->music_client_) return;
        const auto tracks = self->music_client_->GetTracks();
        if (tracks.empty()) return;
        const intptr_t delta = reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
        self->music_index_ = static_cast<uint8_t>((self->music_index_ + tracks.size() + delta) % tracks.size());
        self->launcher_state_.SetMusicIndex(self->music_index_);
        self->music_client_->Select(self->music_index_);
        self->music_client_->PlaySelected();
        self->RenderLauncher();
    }

    static void MusicRetryCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self->music_client_) self->music_client_->Start();
    }

    static void MusicStopCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self->music_client_) self->music_client_->StopPlayback();
        self->launcher_state_.Back();
        self->RenderLauncher();
    }

    static void MusicSeekCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (!self->music_client_ || self->music_progress_slider_ == nullptr) return;
        const uint32_t position = static_cast<uint32_t>(lv_slider_get_value(self->music_progress_slider_));
        if (lv_event_get_code(event) == LV_EVENT_VALUE_CHANGED) {
            char elapsed[12];
            FormatMusicTime(elapsed, sizeof(elapsed), position);
            if (self->music_elapsed_label_) lv_label_set_text(self->music_elapsed_label_, elapsed);
        } else {
            self->music_client_->Seek(position);
        }
    }

    static void MusicProgressTimerCallback(lv_timer_t* timer) {
        auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (!self || !self->music_client_ || !self->music_progress_slider_ || !self->music_elapsed_label_) return;
        const auto state = self->music_client_->GetUiState();
        if (!lv_obj_has_state(self->music_progress_slider_, LV_STATE_PRESSED)) {
            const uint32_t position = self->music_client_->GetPositionMs();
            lv_slider_set_value(self->music_progress_slider_, static_cast<int>(position), LV_ANIM_OFF);
            char elapsed[12];
            FormatMusicTime(elapsed, sizeof(elapsed), position);
            lv_label_set_text(self->music_elapsed_label_, elapsed);
        }
        if (state == MusicUiState::kPlaying) {
            static constexpr uint8_t heights[] = {10, 16, 22, 30, 21, 14, 26, 18};
            ++self->music_wave_phase_;
            for (size_t index = 0; index < self->music_bars_.size(); ++index) {
                if (self->music_bars_[index]) {
                    const int height = heights[(self->music_wave_phase_ + index * 2) % 8];
                    lv_obj_set_height(self->music_bars_[index], height);
                    lv_obj_set_y(self->music_bars_[index], 314 - height / 2);
                }
            }
        }
    }

    static void SliderCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        auto slider = lv_event_get_target_obj(event);
        const int value = lv_slider_get_value(slider);
        if (slider == self->brightness_slider_) Board::GetInstance().GetBacklight()->SetBrightness(value, true);
        else Board::GetInstance().GetAudioCodec()->SetOutputVolume(value);
    }

    static void GalleryTimerCallback(lv_timer_t* timer) {
        auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self == nullptr || self->launcher_state_.page() != YGSoulPage::kGallery) return;
        auto& store = GalleryStore::GetInstance();
        if (store.Count() < 2) return;
        const size_t index = self->launcher_state_.gallery_index();
        if (!store.Loop() && index + 1 >= store.Count()) {
            lv_timer_pause(timer);
            return;
        }
        self->gallery_timer_ = nullptr;
        lv_timer_delete(timer);
        self->SwitchGalleryHorizontal(true);
    }

    static void GalleryLongPressCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (self && self->gallery_delete_) lv_obj_remove_flag(self->gallery_delete_, LV_OBJ_FLAG_HIDDEN);
    }

    static void DeleteGalleryItemCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        if (!self) return;
        auto& store = GalleryStore::GetInstance();
        const auto item = store.ItemAt(self->launcher_state_.gallery_index());
        if (!item.item_id.empty() && store.DeleteItem(item.item_id)) {
            Application::GetInstance().ReportGalleryItemDeleted(item.item_id);
            self->launcher_state_.SetGalleryCount(store.Count());
            self->RenderLauncher();
        }
    }

    void RenderFeatures() {
        lv_label_set_text(launcher_title_, "功能");
        if (launcher_state_.feature_page() != 0) {
            launcher_buttons_[0] = CreateLauncherButton("", 159, 160, 148, 120);
            auto icon = CreateIcon(launcher_buttons_[0], FONT_AWESOME_WATCH, 0, 0, 80, lv_color_white());
            lv_obj_set_style_transform_scale(icon, 320, 0);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, -8, 12);
            auto label = CreateSmallLabel(launcher_buttons_[0], "智能手表", 0, 0, 126, lv_color_white());
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 72);
            lv_obj_add_event_cb(launcher_buttons_[0], FeatureClickCallback, LV_EVENT_CLICKED, this);
            CreateSmallLabel(launcher_content_, "2 / 2  ·  右滑返回", 108, 394, 250, lv_color_hex(0xCEC2ED));
            return;
        }
        static constexpr const char* labels[] = {"音乐", "图库", "AI 对话", "系统设置"};
        static constexpr const char* icons[] = {
            FONT_AWESOME_MUSIC, FONT_AWESOME_IMAGE, FONT_AWESOME_MICROCHIP_AI, FONT_AWESOME_GEAR,
        };
        for (size_t index = 0; index < 4; ++index) {
            launcher_buttons_[index] = CreateLauncherButton(
                "", 78 + (index % 2) * 162, 118 + (index / 2) * 134, 148, 120);
            auto icon = CreateIcon(launcher_buttons_[index], icons[index], 0, 0, 80,
                                   index == 0 ? lv_color_hex(0xFFB4F8) : lv_color_white());
            lv_obj_set_style_transform_scale(icon, 320, 0);
            lv_obj_align(icon, LV_ALIGN_TOP_MID, -8, 12);
            auto label = CreateSmallLabel(launcher_buttons_[index], labels[index], 0, 0, 126, lv_color_white());
            lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 72);
            lv_obj_add_event_cb(launcher_buttons_[index], FeatureClickCallback, LV_EVENT_CLICKED, this);
        }
        CreateSmallLabel(launcher_content_, "1 / 2  ·  左滑更多", 108, 394, 250, lv_color_hex(0xCEC2ED));
    }

    void UpdateWatchClock() {
        if (watch_time_label_ == nullptr || watch_date_label_ == nullptr ||
            watch_battery_label_ == nullptr || watch_steps_label_ == nullptr) return;

        timeval now{};
        if (gettimeofday(&now, nullptr) != 0) return;
        std::tm local{};
        const bool time_valid = now.tv_sec >= 1672531200 && localtime_r(&now.tv_sec, &local) != nullptr;
        if (time_valid && watch_last_hand_second_ != now.tv_sec) {
            watch_last_hand_second_ = now.tv_sec;
            const auto turns = ComputeWatchHandTurns(local);
            UpdateWatchHand(watch_hour_hand_, watch_hour_points_, turns.hours, 92);
            UpdateWatchHand(watch_minute_hand_, watch_minute_points_, turns.minutes, 132);
            UpdateWatchHand(watch_second_hand_, watch_second_points_, turns.seconds, 148);
        }

        if (watch_last_data_second_ == now.tv_sec) return;
        watch_last_data_second_ = now.tv_sec;
        int battery_level = 0;
        bool charging = false;
        bool discharging = false;
        char battery_text[8] = "--%";
        if (Board::GetInstance().GetBatteryLevel(battery_level, charging, discharging)) {
            snprintf(battery_text, sizeof(battery_text), "%d%%", battery_level);
        }
        lv_label_set_text(watch_battery_label_, battery_text);
        uint32_t steps = 0;
        char steps_text[20] = "-- 步";
        if (Board::GetInstance().GetStepCount(steps)) {
            snprintf(steps_text, sizeof(steps_text), "%lu 步", static_cast<unsigned long>(steps));
        }
        lv_label_set_text(watch_steps_label_, steps_text);
        if (!time_valid) {
            lv_label_set_text(watch_time_label_, "--:--");
            lv_label_set_text(watch_date_label_, "等待网络校时");
            return;
        }
        char time_text[6];
        char date_text[16];
        std::strftime(time_text, sizeof(time_text), "%H:%M", &local);
        std::strftime(date_text, sizeof(date_text), "%Y-%m-%d", &local);
        lv_label_set_text(watch_time_label_, time_text);
        lv_label_set_text(watch_date_label_, date_text);
    }

    static void WatchTimerCallback(lv_timer_t* timer) {
        auto self = static_cast<CustomLcdDisplay*>(lv_timer_get_user_data(timer));
        if (self != nullptr && self->launcher_state_.page() == YGSoulPage::kWatch) {
            self->UpdateWatchClock();
        }
    }

    void RenderWatch() {
        lv_label_set_text(launcher_title_, "");
        const std::string face_path = WatchFaceStore::GetInstance().CurrentPath();
        const bool loaded = !face_path.empty() && LoadWatchFaceAsset(face_path);
        if (loaded || DecodeWatchFaceAsset(default_watch_face_start,
                                           static_cast<size_t>(default_watch_face_end - default_watch_face_start))) {
            auto image = lv_image_create(launcher_content_);
            lv_image_set_src(image, watch_face_asset_->image_dsc());
            lv_obj_set_pos(image, 0, 0);
        }
        watch_time_label_ = CreateWatchRegionLabel("--:--", 128, 310, 210, 56, lv_color_white());
        lv_obj_set_style_transform_scale(watch_time_label_, 330, 0);
        watch_date_label_ = CreateWatchRegionLabel(
            "等待网络校时", 150, 48, 166, 46, lv_color_white(), &font_puhui_20_4);
        watch_steps_label_ = CreateWatchRegionLabel(
            "-- 步", 64, 98, 108, 100, lv_color_hex(0xC8F5FF), &font_puhui_20_4);
        watch_battery_label_ = CreateWatchRegionLabel(
            "--%", 294, 98, 108, 100, lv_color_hex(0xC8F5FF), &font_puhui_20_4);
        CreateWatchRegionLabel(
            "YG Soul", 128, 358, 210, 36, lv_color_hex(0xC8F5FF), &font_puhui_20_4);
        auto make_hand = [this](lv_color_t color, int width) {
            auto line = lv_line_create(launcher_content_);
            lv_obj_set_style_line_color(line, color, 0);
            lv_obj_set_style_line_width(line, width, 0);
            lv_obj_set_style_line_rounded(line, true, 0);
            return line;
        };
        watch_hour_hand_ = make_hand(lv_color_white(), 10);
        watch_minute_hand_ = make_hand(lv_color_white(), 7);
        watch_second_hand_ = make_hand(lv_color_hex(0xFF3F69), 3);
        auto hub = lv_obj_create(launcher_content_);
        lv_obj_set_pos(hub, 223, 223);
        lv_obj_set_size(hub, 20, 20);
        lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(hub, 4, 0);
        lv_obj_set_style_border_color(hub, lv_color_hex(0x1BD9FF), 0);
        lv_obj_set_style_bg_color(hub, lv_color_hex(0x10131A), 0);
        UpdateWatchClock();
        watch_timer_ = lv_timer_create(WatchTimerCallback, 50, this);
    }

    lv_obj_t* CreateCover(lv_obj_t* parent, int x, int y, int size) {
        auto clip = lv_obj_create(parent);
        lv_obj_set_pos(clip, x, y);
        lv_obj_set_size(clip, size, size);
        lv_obj_set_style_radius(clip, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_clip_corner(clip, true, 0);
        lv_obj_set_style_pad_all(clip, 0, 0);
        lv_obj_set_style_border_width(clip, 2, 0);
        lv_obj_set_style_border_color(clip, lv_color_hex(0x8B7CFF), 0);
        lv_obj_set_style_bg_color(clip, lv_color_hex(0x19105A), 0);
        lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
        if (ygsoul_music_cover_) {
            auto image = lv_image_create(clip);
            lv_image_set_src(image, ygsoul_music_cover_->image_dsc());
            lv_image_set_scale(image, size * 256 / 190);
            lv_obj_center(image);
        }
        return clip;
    }

    void RenderMusicPlaylist() {
        const auto tracks = music_client_->GetTracks();
        launcher_state_.SetMusicCount(tracks.size());
        music_index_ = static_cast<uint8_t>(launcher_state_.music_index());
        lv_label_set_text(launcher_title_, "设备曲单");
        char count[20];
        snprintf(count, sizeof(count), "%u 首", static_cast<unsigned>(tracks.size()));
        CreateSmallLabel(launcher_content_, count, 183, 85, 100, lv_color_hex(0xB9B3E7));
        if (tracks.empty()) {
            CreateCover(launcher_content_, 158, 133, 150);
            CreateLabel(launcher_content_, "等待云端曲单", 93, 306, 280, lv_color_white());
            CreateLabel(launcher_content_, "请在小程序下发", 93, 344, 280, lv_color_hex(0xB9B3E7));
            return;
        }
        const size_t first = music_index_ > 1 ? music_index_ - 1 : 0;
        const size_t visible = std::min<size_t>(4, tracks.size() - first);
        for (size_t row = 0; row < visible; ++row) {
            const size_t index = first + row;
            auto item = CreateLauncherButton("", 73, 110 + row * 70, 320, 66);
            launcher_buttons_[row] = item;
            lv_obj_set_style_radius(item, 31, 0);
            const bool selected = index == music_index_;
            lv_obj_set_style_border_width(item, selected ? 3 : 1, 0);
            lv_obj_set_style_border_color(item, selected ? lv_color_hex(0x66E7FF) : lv_color_hex(0x4658C7), 0);
            lv_obj_set_style_shadow_opa(item, selected ? LV_OPA_70 : LV_OPA_20, 0);
            char number[8];
            snprintf(number, sizeof(number), "%u", static_cast<unsigned>(index + 1));
            CreateLabel(item, number, 6, 16, 34, selected ? lv_color_hex(0x68E8FF) : lv_color_hex(0xAAA7DA));
            CreateCover(item, 43, 7, 52);
            auto title = CreateSmallLabel(item, tracks[index].title.c_str(), 106, 7, 160,
                                          lv_color_white(), LV_TEXT_ALIGN_LEFT);
            lv_obj_set_height(title, 25);
            lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
            auto artist = CreateLabel(item, tracks[index].artist.c_str(), 106, 38, 160,
                                      lv_color_hex(0xB9B3E7), LV_TEXT_ALIGN_LEFT);
            lv_obj_set_height(artist, 20);
            lv_obj_set_style_text_font(artist, &font_puhui_16_4, 0);
            lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_DOTS);
            if (selected && music_client_->GetUiState() == MusicUiState::kPlaying)
                CreateIcon(item, FONT_AWESOME_MUSIC, 266, 15, 42, lv_color_hex(0x42F2D0));
            lv_obj_set_user_data(item, reinterpret_cast<void*>(index));
            lv_obj_add_event_cb(item, MusicClickCallback, LV_EVENT_CLICKED, this);
        }
        CreateSmallLabel(launcher_content_, "上下滑动选择  ·  轻触播放", 91, 402, 284, lv_color_hex(0xC8C3EF));
    }

    void RenderMusicDisconnected(const MusicTrackInfo& track) {
        CreateIcon(launcher_content_, FONT_AWESOME_WIFI_SLASH, 171, 39, 50, lv_color_hex(0xFFB449));
        CreateLabel(launcher_content_, "连接中断", 214, 39, 150, lv_color_white(), LV_TEXT_ALIGN_LEFT);
        CreateCover(launcher_content_, 143, 99, 180);
        CreateLabel(launcher_content_, track.title.c_str(), 93, 291, 280, lv_color_white());
        CreateSmallLabel(launcher_content_, track.artist.c_str(), 143, 331, 180, lv_color_hex(0xB9B3E7));
        auto retry = CreateLauncherButton("", 165, 366, 82, 70);
        CreateIcon(retry, FONT_AWESOME_ARROWS_ROTATE, 16, 18, 50, lv_color_white());
        lv_obj_add_event_cb(retry, MusicRetryCallback, LV_EVENT_CLICKED, this);
        auto stop = CreateLauncherButton("", 270, 378, 58, 58);
        CreateIcon(stop, FONT_AWESOME_STOP, 4, 13, 50, lv_color_hex(0xC5C8EA));
        lv_obj_add_event_cb(stop, MusicStopCallback, LV_EVENT_CLICKED, this);
    }

    void RenderMusicPlayer() {
        const auto tracks = music_client_->GetTracks();
        if (tracks.empty()) { RenderMusicPlaylist(); return; }
        lv_label_set_text(launcher_title_, "");
        if (music_index_ >= tracks.size()) music_index_ = 0;
        const auto state = music_client_->GetUiState();
        if (state == MusicUiState::kDisconnected || state == MusicUiState::kFailed) {
            RenderMusicDisconnected(tracks[music_index_]);
            return;
        }
        const bool playing = state == MusicUiState::kPlaying || state == MusicUiState::kBuffering;
        CreateIcon(launcher_content_, FONT_AWESOME_WIFI, 170, 32, 50,
                   playing ? lv_color_hex(0x4AF0A1) : lv_color_hex(0xFFB449));
        CreateLabel(launcher_content_, playing ? "播放中" : "已暂停", 214, 33, 140, lv_color_white(), LV_TEXT_ALIGN_LEFT);
        CreateCover(launcher_content_, 155, 70, 156);
        auto title = CreateLabel(launcher_content_, tracks[music_index_].title.c_str(), 83, 236, 300, lv_color_white());
        lv_obj_set_height(title, 34);
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        auto artist = CreateSmallLabel(launcher_content_, tracks[music_index_].artist.c_str(), 103, 276, 260,
                                       lv_color_hex(0xB9B3E7));
        lv_obj_set_height(artist, 22);
        lv_label_set_long_mode(artist, LV_LABEL_LONG_MODE_DOTS);
        static constexpr int bars[] = {10, 17, 23, 16, 30, 21, 14, 19, 11};
        for (size_t i = 0; i < sizeof(bars) / sizeof(bars[0]); ++i) {
            auto bar = lv_obj_create(launcher_content_);
            lv_obj_set_pos(bar, 173 + i * 15, 314 - bars[i] / 2);
            lv_obj_set_size(bar, 7, bars[i]);
            lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(bar, 0, 0);
            lv_obj_set_style_bg_color(bar, i < 5 ? lv_color_hex(0x53E8FF) : lv_color_hex(0x9E79FF), 0);
            music_bars_[i] = bar;
        }
        auto progress = lv_slider_create(launcher_content_);
        music_progress_slider_ = progress;
        lv_obj_set_pos(progress, 96, 342);
        lv_obj_set_size(progress, 274, 7);
        lv_obj_set_ext_click_area(progress, 20);
        lv_obj_remove_flag(progress, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_set_style_radius(progress, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(progress, lv_color_hex(0x34428E), LV_PART_MAIN);
        lv_obj_set_style_bg_color(progress, lv_color_hex(0x8462FF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(progress, lv_color_white(), LV_PART_KNOB);
        lv_obj_set_style_width(progress, 22, LV_PART_KNOB);
        lv_obj_set_style_height(progress, 22, LV_PART_KNOB);
        const uint32_t duration = tracks[music_index_].duration_ms;
        const uint32_t position = music_client_->GetPositionMs();
        lv_slider_set_range(progress, 0, duration ? static_cast<int>(duration) : 1);
        lv_slider_set_value(progress, static_cast<int>(std::min(position, duration)), LV_ANIM_OFF);
        lv_obj_add_event_cb(progress, MusicSeekCallback, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(progress, MusicSeekCallback, LV_EVENT_RELEASED, this);
        char elapsed[12], total[12];
        FormatMusicTime(elapsed, sizeof(elapsed), position);
        FormatMusicTime(total, sizeof(total), duration);
        music_elapsed_label_ = CreateSmallLabel(launcher_content_, elapsed, 88, 352, 64, lv_color_hex(0xC8C3EF));
        CreateSmallLabel(launcher_content_, total, 314, 352, 64, lv_color_hex(0xC8C3EF));
        auto previous = CreateLauncherButton("", 126, 382, 50, 50);
        auto play = CreateLauncherButton("", 201, 370, 64, 64);
        auto next = CreateLauncherButton("", 290, 382, 50, 50);
        lv_obj_set_style_bg_opa(previous, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(next, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(previous, 0, 0);
        lv_obj_set_style_border_width(next, 0, 0);
        lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
        auto previous_icon = CreateIcon(previous, FONT_AWESOME_BACKWARD_STEP, 0, 10, 50, lv_color_white());
        CreateIcon(play, playing ? FONT_AWESOME_PAUSE : FONT_AWESOME_PLAY, 7, 17, 50, lv_color_white());
        auto next_icon = CreateIcon(next, FONT_AWESOME_FORWARD_STEP, 0, 10, 50, lv_color_white());
        lv_obj_set_style_transform_scale(previous_icon, 384, 0);
        lv_obj_set_style_transform_scale(next_icon, 384, 0);
        lv_obj_set_user_data(previous, reinterpret_cast<void*>(-1));
        lv_obj_set_user_data(next, reinterpret_cast<void*>(1));
        lv_obj_add_event_cb(previous, MusicStepCallback, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(next, MusicStepCallback, LV_EVENT_CLICKED, this);
        lv_obj_add_event_cb(play, MusicPlayCallback, LV_EVENT_CLICKED, this);
        music_progress_timer_ = lv_timer_create(MusicProgressTimerCallback, 180, this);
    }

    void RenderGallery() {
        lv_label_set_text(launcher_title_, "图库");
        auto& store = GalleryStore::GetInstance();
        launcher_state_.SetGalleryCount(store.Count());
        const size_t index = launcher_state_.gallery_index();
        if (store.Count() == 0) {
            CreateIcon(launcher_content_, FONT_AWESOME_IMAGE, 193, 176, 80, lv_color_hex(0xBEB1F3));
            CreateSmallLabel(launcher_content_, "暂无图片", 108, 248, 250, lv_color_white());
            CreateSmallLabel(launcher_content_, "请在小程序内容下发中添加", 68, 282, 330, lv_color_hex(0xBEB1F3));
            return;
        }
        const auto item = store.ItemAt(index);
        if (!LoadGalleryAsset(item)) {
            CreateSmallLabel(launcher_content_, "图片加载失败", 108, 240, 250, lv_color_white());
            return;
        }
        const lv_img_dsc_t* image = nullptr;
        if (item.format == "eaf") {
            gallery_image_ = lv_eaf_create(launcher_content_);
            lv_eaf_set_src(gallery_image_, gallery_asset_->image_dsc());
            lv_eaf_set_loop_count(gallery_image_, -1);
            if (!lv_eaf_is_loaded(gallery_image_)) {
                StopGalleryAnimation();
                CreateSmallLabel(launcher_content_, "动图解码失败", 108, 240, 250, lv_color_white());
                return;
            }
            image = static_cast<const lv_img_dsc_t*>(lv_image_get_src(gallery_image_));
        } else if (item.format == "gif") {
            gallery_image_ = lv_gif_create(launcher_content_);
            lv_gif_set_color_format(gallery_image_, LV_COLOR_FORMAT_RGB565);
            lv_gif_set_src(gallery_image_, gallery_asset_->image_dsc());
            if (!lv_gif_is_loaded(gallery_image_)) {
                StopGalleryAnimation();
                CreateSmallLabel(launcher_content_, "动图解码失败", 108, 240, 250, lv_color_white());
                return;
            }
            image = static_cast<const lv_img_dsc_t*>(lv_image_get_src(gallery_image_));
        } else {
            gallery_image_ = lv_image_create(launcher_content_);
            image = gallery_asset_->image_dsc();
            lv_image_set_src(gallery_image_, image);
        }
        if (image->header.w > 0 && image->header.h > 0) {
            const uint32_t width_scale = DISPLAY_WIDTH * 256U / image->header.w;
            const uint32_t height_scale = DISPLAY_HEIGHT * 256U / image->header.h;
            lv_image_set_scale(gallery_image_, width_scale > height_scale ? width_scale : height_scale);
        }
        lv_obj_align(gallery_image_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_event_cb(gallery_image_, GalleryLongPressCallback, LV_EVENT_LONG_PRESSED, this);
        gallery_delete_ = CreateLauncherButton("", 341, 102, 56, 56);
        CreateIcon(gallery_delete_, FONT_AWESOME_TRASH, 1, 12, 54, lv_color_white());
        lv_obj_add_flag(gallery_delete_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(gallery_delete_, DeleteGalleryItemCallback, LV_EVENT_CLICKED, this);
        auto hint = lv_label_create(launcher_content_);
        char text[48];
        snprintf(text, sizeof(text), "左右滑动切换   %u / %u%s", static_cast<unsigned>(index + 1),
                 static_cast<unsigned>(store.Count()),
                 (item.format == "gif" || item.format == "eaf") ? "  动图" : "");
        lv_label_set_text(hint, text);
        lv_obj_set_style_text_color(hint, lv_color_white(), 0);
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -22);
        if (store.Count() > 1 && (store.Loop() || index + 1 < store.Count())) {
            gallery_timer_ = lv_timer_create(GalleryTimerCallback, store.IntervalSec() * 1000, this);
        }
    }

    void RenderSettings() {
        lv_label_set_text(launcher_title_, "系统设置");
        auto wifi = CreateLauncherButton("", 78, 116, 148, 120);
        auto power = CreateLauncherButton("", 240, 116, 148, 120);
        CreateIcon(wifi, FONT_AWESOME_WIFI, 47, 12, 54, lv_color_hex(0xC1EAFF));
        CreateSmallLabel(wifi, "Wi-Fi", 14, 55, 120, lv_color_white());
        CreateSmallLabel(wifi, "已连接", 14, 82, 120, lv_color_hex(0xB9B3E7));
        CreateIcon(power, FONT_AWESOME_MOON, 47, 12, 54, lv_color_hex(0x8EFFA8));
        CreateSmallLabel(power, "省电模式", 14, 55, 120, lv_color_white());
        CreateSmallLabel(power, "常连", 14, 82, 120, lv_color_hex(0x8EFFA8));
        (void)wifi; (void)power;
        CreateLabel(launcher_content_, "屏幕亮度", 92, 253, 150, lv_color_white(), LV_TEXT_ALIGN_LEFT);
        auto brightness = lv_slider_create(launcher_content_);
        brightness_slider_ = brightness;
        lv_obj_set_pos(brightness, 92, 290);
        lv_obj_set_size(brightness, 282, 14);
        lv_obj_set_style_bg_color(brightness, lv_color_hex(0x34428E), LV_PART_MAIN);
        lv_obj_set_style_bg_color(brightness, lv_color_hex(0x64E5FF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(brightness, lv_color_white(), LV_PART_KNOB);
        lv_slider_set_value(brightness, Board::GetInstance().GetBacklight()->brightness(), LV_ANIM_OFF);
        lv_obj_add_event_cb(brightness, SliderCallback, LV_EVENT_RELEASED, this);
        CreateLabel(launcher_content_, "音量", 92, 333, 150, lv_color_white(), LV_TEXT_ALIGN_LEFT);
        auto volume = lv_slider_create(launcher_content_);
        lv_obj_set_pos(volume, 92, 370);
        lv_obj_set_size(volume, 282, 14);
        lv_obj_set_style_bg_color(volume, lv_color_hex(0x34428E), LV_PART_MAIN);
        lv_obj_set_style_bg_color(volume, lv_color_hex(0x8A75FF), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(volume, lv_color_white(), LV_PART_KNOB);
        lv_slider_set_value(volume, Board::GetInstance().GetAudioCodec()->output_volume(), LV_ANIM_OFF);
        lv_obj_add_event_cb(volume, SliderCallback, LV_EVENT_RELEASED, this);
    }

    void RenderLauncher() {
        Application::GetInstance().SetMusicPlayerVisible(launcher_state_.page() == YGSoulPage::kMusicPlayer);
        if (launcher_panel_ == nullptr) return;
        const int64_t render_started = esp_timer_get_time();
        if (ygsoul_role_animation_ != nullptr) lv_eaf_pause(ygsoul_role_animation_);
        StopGalleryAnimation();
        StopMusicProgress();
        StopWatchClock();
        brightness_slider_ = nullptr;
        lv_obj_clean(launcher_content_);
        launcher_buttons_.fill(nullptr);
        AddStars();
        switch (launcher_state_.page()) {
            case YGSoulPage::kFeatures: RenderFeatures(); break;
            case YGSoulPage::kMusicPlaylist: RenderMusicPlaylist(); break;
            case YGSoulPage::kMusicPlayer: RenderMusicPlayer(); break;
            case YGSoulPage::kGallery: RenderGallery(); break;
            case YGSoulPage::kSettings: RenderSettings(); break;
            case YGSoulPage::kWatch: RenderWatch(); break;
            default: HideLauncher(); return;
        }
        lv_obj_remove_flag(launcher_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(launcher_panel_);
        ESP_LOGI(TAG, "Launcher render: page=%d, elapsed=%lu ms",
                 static_cast<int>(launcher_state_.page()),
                 static_cast<unsigned long>((esp_timer_get_time() - render_started) / 1000));
    }

    void HideLauncher() {
        Application::GetInstance().SetMusicPlayerVisible(false);
        StopGalleryAnimation();
        StopMusicProgress();
        StopWatchClock();
        if (launcher_transition_image_ != nullptr) {
            lv_anim_delete(launcher_transition_image_, SetPanelY);
            ReleaseLauncherTransition(false);
        }
        if (launcher_panel_) lv_obj_add_flag(launcher_panel_, LV_OBJ_FLAG_HIDDEN);
        ShowYGSoulCompanion();
    }

    static void GestureCallback(lv_event_t* event) {
        auto self = static_cast<CustomLcdDisplay*>(lv_event_get_user_data(event));
        auto indev = lv_indev_active();
        if (self == nullptr || indev == nullptr) return;
        if (self->launcher_transitioning_) return;
        if (Application::GetInstance().GetDeviceState() != kDeviceStateIdle) {
            if (self->launcher_state_.page() != YGSoulPage::kDesktop) {
                self->launcher_state_.Home();
                self->HideLauncher();
            }
            return;
        }
        const lv_dir_t direction = lv_indev_get_gesture_dir(indev);
        YGSoulGesture gesture;
        if (direction == LV_DIR_TOP) gesture = YGSoulGesture::kUp;
        else if (direction == LV_DIR_BOTTOM) gesture = YGSoulGesture::kDown;
        else if (direction == LV_DIR_LEFT) gesture = YGSoulGesture::kLeft;
        else if (direction == LV_DIR_RIGHT) gesture = YGSoulGesture::kRight;
        else return;
        const auto before = self->launcher_state_.page();
        if (before == YGSoulPage::kMusicPlaylist) lv_indev_wait_release(indev);
        if (before == YGSoulPage::kGallery &&
            (gesture == YGSoulGesture::kLeft || gesture == YGSoulGesture::kRight)) {
            self->SwitchGalleryHorizontal(gesture == YGSoulGesture::kLeft);
            return;
        }
        if (before == YGSoulPage::kFeatures &&
            (gesture == YGSoulGesture::kLeft || gesture == YGSoulGesture::kRight)) {
            self->SwitchGalleryHorizontal(gesture == YGSoulGesture::kLeft);
            return;
        }
        self->launcher_state_.Gesture(gesture);
        const auto after = self->launcher_state_.page();
        if (after == YGSoulPage::kMusicPlaylist && self->music_client_) {
            const auto tracks = self->music_client_->GetTracks();
            self->music_index_ = static_cast<uint8_t>(self->launcher_state_.music_index());
        }
        if (after != before || after == YGSoulPage::kGallery || after == YGSoulPage::kMusicPlaylist) {
            self->RenderLauncher();
            if (before == YGSoulPage::kDesktop && after == YGSoulPage::kFeatures) self->AnimateLauncher(DISPLAY_HEIGHT);
            else if (after == YGSoulPage::kSettings && before != after) self->AnimateLauncher(-DISPLAY_HEIGHT);
        }
    }

    void CreateLauncher() {
        launcher_panel_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(launcher_panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_pos(launcher_panel_, 0, 0);
        lv_obj_set_style_radius(launcher_panel_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(launcher_panel_, 0, 0);
        lv_obj_set_style_bg_color(launcher_panel_, lv_color_hex(0x160C2E), 0);
        lv_obj_set_style_bg_grad_color(launcher_panel_, lv_color_hex(0x24116B), 0);
        lv_obj_set_style_bg_grad_dir(launcher_panel_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_pad_all(launcher_panel_, 0, 0);
        lv_obj_add_flag(launcher_panel_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(launcher_panel_, LV_OBJ_FLAG_SCROLLABLE);
        launcher_title_ = lv_label_create(launcher_panel_);
        lv_obj_set_style_text_color(launcher_title_, lv_color_white(), 0);
        lv_obj_align(launcher_title_, LV_ALIGN_TOP_MID, 0, 48);
        launcher_content_ = lv_obj_create(launcher_panel_);
        lv_obj_set_pos(launcher_content_, 0, 0);
        lv_obj_set_size(launcher_content_, DISPLAY_WIDTH, DISPLAY_HEIGHT);
        lv_obj_set_style_bg_opa(launcher_content_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(launcher_content_, 0, 0);
        lv_obj_set_style_pad_all(launcher_content_, 0, 0);
        lv_obj_add_flag(launcher_content_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(launcher_content_, LV_OBJ_FLAG_SCROLLABLE);
        auto back = lv_button_create(launcher_panel_);
        lv_obj_remove_style_all(back);
        lv_obj_set_pos(back, 96, 56);
        lv_obj_set_size(back, 44, 44);
        lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(back, 0, 0);
        auto label = lv_label_create(back);
        lv_label_set_text(label, FONT_AWESOME_ANGLE_LEFT);
        lv_obj_set_style_text_font(label, &font_awesome_30_4, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        lv_obj_add_event_cb(back, BackClickCallback, LV_EVENT_CLICKED, this);
        lv_obj_add_flag(launcher_panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(lv_screen_active(), GestureCallback, LV_EVENT_GESTURE, this);
    }

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
        auto music_cover = LoadYGSoulAsset("ygsoul_music_cover.cbin");
        std::array<std::unique_ptr<LvglImage>, YGSOUL_MOUTH_FRAME_COUNT> mouths;
        mouths[0] = LoadYGSoulAsset("ygsoul_mouth_1.cbin");
        mouths[1] = LoadYGSoulAsset("ygsoul_mouth_2.cbin");
        mouths[2] = LoadYGSoulAsset("ygsoul_mouth_3.cbin");
        if (companion == nullptr || music_cover == nullptr ||
            mouths[0] == nullptr || mouths[1] == nullptr || mouths[2] == nullptr) {
            ESP_LOGE(TAG, "YGSoul assets are incomplete; using the lightweight display fallback");
            return false;
        }
        ygsoul_companion_ = std::move(companion);
        ygsoul_music_cover_ = std::move(music_cover);
        ygsoul_mouth_frames_ = std::move(mouths);
        ygsoul_assets_ready_ = true;
        ESP_LOGI(TAG, "Loaded all YGSoul CBin assets from the assets partition");
        return true;
    }

    void ApplyYGSoulChatMessageColor() {
        if (chat_message_label_ == nullptr) {
            return;
        }
        const auto color = ygsoul_chat_message_is_system_ || custom_role_image_
            ? lv_color_white()
            : lv_color_hex(YGSOUL_UI_TEXT);
        lv_obj_set_style_text_color(chat_message_label_, color, 0);
    }

    void ApplyYGSoulOverlayStyle() {
        const bool image_overlay = showing_boot_logo_ || custom_role_image_;
        const auto text = image_overlay ? lv_color_white() : lv_color_hex(YGSOUL_UI_TEXT);
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bottom_bar_, custom_role_image_ ? LV_OPA_60 : LV_OPA_TRANSP, 0);
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
        if (custom_role_image_ || ygsoul_mouth_image_ == nullptr || ygsoul_mouth_speaking_) {
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

    void StopRoleAnimation() {
        if (ygsoul_role_animation_ != nullptr) {
            lv_obj_delete(ygsoul_role_animation_);
            ygsoul_role_animation_ = nullptr;
        }
        ygsoul_role_animation_asset_.reset();
        if (ygsoul_role_animation_bytes_ != nullptr) {
            heap_caps_free(ygsoul_role_animation_bytes_);
            ygsoul_role_animation_bytes_ = nullptr;
        }
    }

    void ShowYGSoulCompanion() {
        if (ygsoul_image_ == nullptr || emoji_box_ == nullptr) {
            return;
        }

        showing_boot_logo_ = false;
        lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
        if (ygsoul_role_animation_ != nullptr) {
            lv_obj_add_flag(ygsoul_image_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(ygsoul_role_animation_, LV_OBJ_FLAG_HIDDEN);
            lv_eaf_resume(ygsoul_role_animation_);
        } else {
            lv_obj_remove_flag(ygsoul_image_, LV_OBJ_FLAG_HIDDEN);
        }
        if (custom_role_image_) {
            lv_obj_add_flag(ygsoul_mouth_image_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(ygsoul_mouth_image_, LV_OBJ_FLAG_HIDDEN);
        }
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
        if (ygsoul_role_animation_ != nullptr) {
            lv_eaf_pause(ygsoul_role_animation_);
            lv_obj_add_flag(ygsoul_role_animation_, LV_OBJ_FLAG_HIDDEN);
        }
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
        CreateLauncher();
        lv_display_add_event_cb(display_, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    }

    bool IsMusicPlayerPage() {
        DisplayLockGuard lock(this);
        return lock && launcher_state_.page() == YGSoulPage::kMusicPlayer;
    }

    bool ExitWatchForPhysicalButton() {
        DisplayLockGuard lock(this);
        if (!lock || launcher_state_.page() != YGSoulPage::kWatch) return false;
        launcher_state_.Home();
        HideLauncher();
        return true;
    }

    void PrepareGalleryDownload() override {
        DisplayLockGuard lock(this);
        if (!lock) return;
        const bool keep_watch = launcher_state_.page() == YGSoulPage::kWatch;
        StopGalleryAnimation();
        StopRoleAnimation();
        ReleaseLauncherTransition(keep_watch);
        ESP_LOGI(TAG, "Released transient visuals for download, free internal=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    }

    void RefreshGallery() override {
        DisplayLockGuard lock(this);
        if (lock && launcher_state_.page() == YGSoulPage::kGallery) RenderLauncher();
    }

    void RefreshWatchFace() override {
        DisplayLockGuard lock(this);
        if (lock && launcher_state_.page() == YGSoulPage::kWatch) RenderLauncher();
    }

    virtual void SetEmotion(const char* emotion) override {
        if (emotion == nullptr || !IsSetupUICalled()) {
            return;
        }
        const auto state = Application::GetInstance().GetDeviceState();
        if (music_client_) music_client_->SetBusy(state != kDeviceStateIdle);
        if (!ygsoul_assets_ready_) {
            LcdDisplay::SetEmotion(emotion);
            return;
        }
        const bool conversation_ui =
            state == kDeviceStateIdle ||
            state == kDeviceStateListening ||
            state == kDeviceStateSpeaking;
        DisplayLockGuard lock(this);
        if (launcher_state_.page() == YGSoulPage::kWatch && conversation_ui) {
            lv_obj_move_foreground(launcher_panel_);
            return;
        }
        if (state != kDeviceStateIdle && launcher_state_.page() != YGSoulPage::kDesktop) {
            launcher_state_.Home();
            HideLauncher();
        }
        if (state == kDeviceStateWifiConfiguring) {
            ShowYGSoulBootLogo();
            return;
        }
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

    void ShowUpdatedRoleImage() {
        if (showing_boot_logo_) return;
        // 角色资源后台更新不抢占当前播放器，也不启动不可见的角色动画。
        if (launcher_state_.page() == YGSoulPage::kMusicPlayer ||
            launcher_state_.page() == YGSoulPage::kWatch) {
            if (ygsoul_role_animation_ != nullptr) lv_eaf_pause(ygsoul_role_animation_);
            if (launcher_panel_ != nullptr) lv_obj_move_foreground(launcher_panel_);
            return;
        }
        ShowYGSoulCompanion();
    }

    virtual bool SetRoleImage(const char* path, const char* format = "jpg") override {
        if (path == nullptr || !IsSetupUICalled() || ygsoul_image_ == nullptr) {
            return false;
        }
        if (path[0] == '\0') {
            auto image = LoadYGSoulAsset("ygsoul_companion_nomouth.cbin");
            if (!image) return false;
            DisplayLockGuard lock(this);
            if (!lock) return false;
            StopRoleAnimation();
            if (launcher_state_.page() != YGSoulPage::kMusicPlayer &&
                launcher_state_.page() != YGSoulPage::kWatch) {
                launcher_state_.Home();
                HideLauncher();
            }
            StopYGSoulSpeakingAnimation();
            ygsoul_companion_ = std::move(image);
            custom_role_image_ = false;
            lv_image_set_src(ygsoul_image_, ygsoul_companion_->image_dsc());
            if (ygsoul_mouth_image_ != nullptr) {
                lv_obj_clear_flag(ygsoul_mouth_image_, LV_OBJ_FLAG_HIDDEN);
            }
            ShowUpdatedRoleImage();
            return true;
        }
        FILE* file = fopen(path, "rb");
        if (file == nullptr || fseek(file, 0, SEEK_END) != 0) {
            if (file != nullptr) fclose(file);
            return false;
        }
        const long size = ftell(file);
        if (size <= 0 || size > 192 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
            fclose(file);
            return false;
        }
        void* data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (data == nullptr || fread(data, 1, size, file) != static_cast<size_t>(size)) {
            if (data != nullptr) heap_caps_free(data);
            fclose(file);
            return false;
        }
        fclose(file);
        if (format != nullptr && strcmp(format, "eaf") == 0) {
            try {
                auto asset = std::make_unique<LvglRawImage>(data, static_cast<size_t>(size));
                DisplayLockGuard lock(this);
                if (!lock) {
                    heap_caps_free(data);
                    return false;
                }
                StopRoleAnimation();
                ygsoul_role_animation_bytes_ = data;
                ygsoul_role_animation_asset_ = std::move(asset);
                ygsoul_role_animation_ = lv_eaf_create(lv_screen_active());
                lv_eaf_set_src(ygsoul_role_animation_, ygsoul_role_animation_asset_->image_dsc());
                lv_eaf_set_loop_count(ygsoul_role_animation_, -1);
                if (!lv_eaf_is_loaded(ygsoul_role_animation_)) {
                    StopRoleAnimation();
                    return false;
                }
                const auto image = static_cast<const lv_img_dsc_t*>(lv_image_get_src(ygsoul_role_animation_));
                if (image != nullptr && image->header.w > 0 && image->header.h > 0) {
                    const uint32_t width_scale = DISPLAY_WIDTH * 256U / image->header.w;
                    const uint32_t height_scale = DISPLAY_HEIGHT * 256U / image->header.h;
                    lv_image_set_scale(ygsoul_role_animation_, width_scale > height_scale ? width_scale : height_scale);
                }
                lv_obj_center(ygsoul_role_animation_);
                custom_role_image_ = true;
                lv_obj_add_flag(ygsoul_image_, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ygsoul_mouth_image_, LV_OBJ_FLAG_HIDDEN);
                ShowUpdatedRoleImage();
                return true;
            } catch (...) {
                heap_caps_free(data);
                return false;
            }
        }
        uint8_t* decoded_data = nullptr;
        size_t decoded_size = 0;
        size_t decoded_width = 0;
        size_t decoded_height = 0;
        size_t decoded_stride = 0;
        const esp_err_t decode_result = jpeg_to_image(
            static_cast<const uint8_t*>(data), size, &decoded_data, &decoded_size,
            &decoded_width, &decoded_height, &decoded_stride);
        heap_caps_free(data);
        if (decode_result != ESP_OK || decoded_data == nullptr) {
            if (decoded_data != nullptr) heap_caps_free(decoded_data);
            return false;
        }
        try {
            auto image = std::make_unique<LvglAllocatedImage>(
                decoded_data, decoded_size, decoded_width, decoded_height,
                decoded_stride, LV_COLOR_FORMAT_RGB565);
            decoded_data = nullptr;
            DisplayLockGuard lock(this);
            if (!lock) return false;
            StopRoleAnimation();
            if (launcher_state_.page() != YGSoulPage::kMusicPlayer &&
                launcher_state_.page() != YGSoulPage::kWatch) {
                launcher_state_.Home();
                HideLauncher();
            }
            StopYGSoulSpeakingAnimation();
            ygsoul_companion_ = std::move(image);
            custom_role_image_ = true;
            lv_image_set_src(ygsoul_image_, ygsoul_companion_->image_dsc());
            lv_obj_center(ygsoul_image_);
            lv_obj_add_flag(ygsoul_mouth_image_, LV_OBJ_FLAG_HIDDEN);
            ShowUpdatedRoleImage();
            return true;
        } catch (...) {
            if (decoded_data != nullptr) heap_caps_free(decoded_data);
            return false;
        }
    }

    virtual void SetStatus(const char* status) override {
        LcdDisplay::SetStatus(status);
        const auto device_state = Application::GetInstance().GetDeviceState();
        if (music_client_) music_client_->SetBusy(device_state != kDeviceStateIdle);
        const bool speaking = device_state == kDeviceStateSpeaking;
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
        if (showing_boot_logo_) return;
        LcdDisplay::SetChatMessage(role, content);
        DisplayLockGuard lock(this);
        if (launcher_state_.page() == YGSoulPage::kWatch) {
            lv_obj_move_foreground(launcher_panel_);
            return;
        }
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
    std::unique_ptr<Qmi8658Pedometer> pedometer_;
    std::atomic<int> battery_level_{-1};
    std::atomic_bool charging_{false};
    std::atomic_bool discharging_{false};
    std::atomic<uint32_t> step_count_{0};
    std::atomic_bool steps_available_{false};
    bool last_discharging_ = false;
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
            auto& app = Application::GetInstance();
            app.Schedule([this]() {
                auto& app = Application::GetInstance();
                if (!app.CanEnterSleepMode()) {
                    power_save_timer_->WakeUp();
                    return;
                }
                if (app.GetDeviceState() == kDeviceStateListening) {
                    app.EnterStandby();
                }
                EnterSuperPowerSave();
            });
        });
        power_save_timer_->OnExitSleepMode([this]() {
            super_power_save_ = false;
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
        // 仅降低显示功耗；播放器仍保持唤醒词计算关闭。
        audio.EnableVoiceProcessing(false);
        audio.EnableWakeWordDetection(display_ == nullptr || !display_->IsMusicPlayerPage());
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
        audio.EnableWakeWordDetection(display_ == nullptr || !display_->IsMusicPlayerPage());
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

    void InitializePedometer() {
        pedometer_ = std::make_unique<Qmi8658Pedometer>(i2c_bus_);
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
                const bool wake_player_only = display_ != nullptr && display_->IsMusicPlayerPage();
                ExitSuperPowerSave();
                if (wake_player_only) return;
            }
            if (app.GetDeviceState() == kDeviceStateStarting) {
                if (display_ != nullptr) display_->ExitWatchForPhysicalButton();
                EnterWifiConfigMode();
                return;
            }
            const bool exited_watch = display_ != nullptr && display_->ExitWatchForPhysicalButton();
            if (exited_watch && app.GetDeviceState() != kDeviceStateIdle) return;
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "BOOT long press 3s, enter wifi config");
            if (display_ != nullptr) display_->ExitWatchForPhysicalButton();
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
        InitializePedometer();
        RefreshDynamicData();
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
        level = battery_level_.load();
        charging = charging_.load();
        discharging = discharging_.load();
        return level >= 0;
    }

    virtual bool GetStepCount(uint32_t& steps) override {
        steps = step_count_.load();
        return steps_available_.load();
    }

    virtual void RefreshDynamicData() override {
        const bool charging = pmic_->IsCharging();
        const bool discharging = pmic_->IsDischarging();
        if (discharging != last_discharging_) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging_ = discharging;
        }
        battery_level_.store(pmic_->GetBatteryLevel());
        charging_.store(charging);
        discharging_.store(discharging);

        uint32_t steps = 0;
        const bool available = pedometer_ != nullptr && pedometer_->Read(steps);
        if (available) step_count_.store(steps);
        steps_available_.store(available);
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(WaveshareEsp32s3TouchAMOLED1inch75);
