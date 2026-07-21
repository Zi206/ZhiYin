#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "zhengchen_lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "power_save_timer.h"
#include "led/led.h"
#include "power_manager.h"

#include <esp_log.h>
#include <esp_lcd_panel_vendor.h>
#include <led_strip.h>

#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <mutex>

#define TAG "ZHENGCHEN_1_54TFT_WIFI"

class ConversationLightStrip : public Led {
public:
    ConversationLightStrip() {
        led_strip_config_t strip_config = {};
        strip_config.strip_gpio_num = LIGHT_STRIP_GPIO;
        strip_config.max_leds = LIGHT_STRIP_LED_COUNT;
        strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
        strip_config.led_model = LED_MODEL_WS2812;

        led_strip_rmt_config_t rmt_config = {};
        rmt_config.resolution_hz = 10 * 1000 * 1000;

        ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &strip_));
        ESP_ERROR_CHECK(led_strip_clear(strip_));
    }

    ~ConversationLightStrip() override {
        if (strip_ != nullptr) {
            led_strip_clear(strip_);
            led_strip_del(strip_);
        }
    }

    void OnStateChanged() override {
        std::lock_guard<std::mutex> lock(mutex_);
        ApplyStateLocked(Application::GetInstance().GetDeviceState());
    }

    void SetSleeping(bool sleeping) {
        std::lock_guard<std::mutex> lock(mutex_);
        sleeping_ = sleeping;
        ApplyStateLocked(Application::GetInstance().GetDeviceState());
    }

private:
    led_strip_handle_t strip_ = nullptr;
    std::mutex mutex_;
    bool sleeping_ = false;

    static bool IsConversationPageState(DeviceState state) {
        switch (state) {
            case kDeviceStateIdle:
            case kDeviceStateConnecting:
            case kDeviceStateListening:
            case kDeviceStateSpeaking:
            case kDeviceStateMusicPlaying:
            case kDeviceStateAudioTesting:
                return true;
            default:
                return false;
        }
    }

    void ApplyStateLocked(DeviceState state) {
        if (sleeping_ || !IsConversationPageState(state)) {
            ESP_ERROR_CHECK(led_strip_clear(strip_));
            return;
        }

        for (int i = 0; i < LIGHT_STRIP_LED_COUNT; ++i) {
            ESP_ERROR_CHECK(led_strip_set_pixel(
                strip_, i, LIGHT_STRIP_RED, LIGHT_STRIP_GREEN, LIGHT_STRIP_BLUE));
        }
        ESP_ERROR_CHECK(led_strip_refresh(strip_));
    }
};

class ZHENGCHEN_1_54TFT_WIFI : public WifiBoard {
private:
    Button boot_button_;
    Button conversation_button_;
    ConversationLightStrip light_strip_;
    int64_t last_conversation_toggle_time_ = 0;  // 对话键上次触发时间(ms)，防重复触发
    ZHENGCHEN_LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(GPIO_NUM_9);
        power_manager_->OnTemperatureChanged([this](float chip_temp) {
            display_->UpdateHighTempWarning(chip_temp);
        });

        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
                ESP_LOGI("PowerManager", "Charging started");
            } else {
                power_save_timer_->SetEnabled(true);
                ESP_LOGI("PowerManager", "Charging stopped");
            }
        });
    
    }

    void InitializePowerSaveTimer() {
        rtc_gpio_init(GPIO_NUM_2);
        rtc_gpio_set_direction(GPIO_NUM_2, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(GPIO_NUM_2, 1);

        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            light_strip_.SetSleeping(true);
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
            light_strip_.SetSleeping(false);
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_SDA;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_SCL;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeButtons() {
        // GPIO39 使用锁存式电容按键，每次触摸只翻转一次电平，因此上升沿和下降沿都要触发对话。
        static constexpr int64_t kConversationEdgeGuardMs = 50;
        auto handle_conversation_edge = [this]() {
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state != kDeviceStateIdle && state != kDeviceStateListening &&
                state != kDeviceStateSpeaking && state != kDeviceStateMusicPlaying) {
                return;
            }

            int64_t now = esp_timer_get_time() / 1000;
            if (now - last_conversation_toggle_time_ < kConversationEdgeGuardMs) {
                return;
            }
            last_conversation_toggle_time_ = now;
            power_save_timer_->WakeUp();

            if (state == kDeviceStateIdle) {
                app.StartListening();
            } else if (state == kDeviceStateListening || state == kDeviceStateSpeaking) {
                app.CancelListening();
            } else {
                app.ToggleChatState();
            }
        };
        conversation_button_.OnPressDown(handle_conversation_edge);
        conversation_button_.OnPressUp(handle_conversation_edge);

        // BOOT（GPIO0）仅在应用运行时长按 1.5 秒进入配网；短按不做任何处理。
        boot_button_.OnLongPress([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            auto state = app.GetDeviceState();
            if (state == kDeviceStateListening || state == kDeviceStateSpeaking || state == kDeviceStateConnecting) {
                return;
            }
            EnterWifiConfigMode();
        });
    }

    void InitializeSt7789Display() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 3;
        io_config.pclk_hz = 80 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RES;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_));
        ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_, false));

        display_ = new ZHENGCHEN_LcdDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, 
            DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        display_->SetupHighTempWarningPopup();
    }

    void InitializeTools() {
    }

public:
    ZHENGCHEN_1_54TFT_WIFI() :
        boot_button_(BOOT_BUTTON_GPIO, false, 1500),
        conversation_button_(VOLUME_DOWN_BUTTON_GPIO, false, 0, 0, false, true) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeSt7789Display();  
        InitializeTools();
        GetBacklight()->RestoreBrightness();
    }

    // 获取音频编解码器
    virtual AudioCodec* GetAudioCodec() override {
        // 静态实例化NoAudioCodecSimplex类
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        // 返回音频编解码器
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Led* GetLed() override {
        return &light_strip_;
    }
    
    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = std::max<uint32_t>(power_manager_->GetBatteryLevel(), 20);
        return true;
    }

    virtual bool GetTemperature(float& esp32temp)  override {
        esp32temp = power_manager_->GetTemperature();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }
};

DECLARE_BOARD(ZHENGCHEN_1_54TFT_WIFI);
