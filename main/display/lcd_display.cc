#include "lcd_display.h"
#include "gif/lvgl_gif.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "assets/lang_config.h"
#include "assets.h"
#include <vector>
#include <algorithm>
#include <string>
#include <font_awesome.h>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_lvgl_port.h>
#include <esp_psram.h>
#include <cstring>
#include <src/misc/cache/lv_cache.h>

#include "board.h"

#define TAG "LcdDisplay"

namespace {
constexpr uint32_t kHoloBlack = 0x000000;
constexpr uint32_t kHoloDeep = 0x00384A;
constexpr uint32_t kHoloCyan = 0x00D4FF;
constexpr uint32_t kHoloBright = 0x7DF9FF;
constexpr uint32_t kHoloPurple = 0x8A2BE2;
constexpr uint32_t kHoloDanger = 0xFF4D5A;
constexpr uint32_t kHoloText = 0xF0FCFF;

void PrepareOverlayObject(lv_obj_t* object) {
    lv_obj_set_style_radius(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_scrollbar_mode(object, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
}
} // namespace

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_awesome_30_4);

void LcdDisplay::InitializeLcdThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_awesome_30_4);

    // light theme (已适配科幻深色背景)
    auto light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0x000A14));  // 深邃星空蓝
    light_theme->set_text_color(lv_color_hex(0xE0E0E0));        // 亮白色文字/图标
    light_theme->set_chat_background_color(lv_color_hex(0x0A1A2A));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0x1A2A3A));
    light_theme->set_system_bubble_color(lv_color_hex(0x001122));
    light_theme->set_system_text_color(lv_color_hex(0x00D4FF));  // 赛博青色系统文字
    light_theme->set_border_color(lv_color_hex(0x00D4FF));       // 赛博青色边框
    light_theme->set_low_battery_color(lv_color_hex(0xFF4444));   // 亮红色低电量
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);

    // dark theme
    auto dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x000000));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x000000));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

LcdDisplay::LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height)
    : panel_io_(panel_io), panel_(panel) {
    width_ = width;
    height_ = height;

    // Initialize LCD themes
    InitializeLcdThemes();

    // Load theme from settings
    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "light");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);

    // Create a timer to hide the preview image
    esp_timer_create_args_t preview_timer_args = {
        .callback = [](void* arg) {
            LcdDisplay* display = static_cast<LcdDisplay*>(arg);
            display->SetPreviewImage(nullptr);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "preview_timer",
        .skip_unhandled_events = false,
    };
    esp_timer_create(&preview_timer_args, &preview_timer_);
}

void LcdDisplay::SetupHolographicLayer(lv_obj_t* screen) {
    holographic_layer_ = lv_obj_create(screen);
    lv_obj_set_size(holographic_layer_, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(holographic_layer_, LV_ALIGN_CENTER, 0, 0);
    PrepareOverlayObject(holographic_layer_);
    lv_obj_set_style_bg_opa(holographic_layer_, LV_OPA_TRANSP, 0);

    // Speaking and music keep the compact three-bar energy indicator. The
    // rounded projection platform and center core were intentionally removed.
    for (int i = 0; i < 3; ++i) {
        holographic_energy_bars_[i] = lv_obj_create(holographic_layer_);
        lv_obj_set_size(holographic_energy_bars_[i], 3, 6);
        lv_obj_align(holographic_energy_bars_[i], LV_ALIGN_CENTER, (i - 1) * 10, 94);
        PrepareOverlayObject(holographic_energy_bars_[i]);
        lv_obj_set_style_radius(holographic_energy_bars_[i], 1, 0);
        lv_obj_add_flag(holographic_energy_bars_[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_add_flag(holographic_layer_, LV_OBJ_FLAG_HIDDEN);
    holographic_layer_visible_ = false;
    SetHolographicMode(HolographicMode::kIdle);
}

void LcdDisplay::SetHolographicLayerVisible(bool visible) {
    if (holographic_layer_ == nullptr || holographic_layer_visible_ == visible) {
        return;
    }

    holographic_layer_visible_ = visible;
    if (visible) {
        lv_obj_remove_flag(holographic_layer_, LV_OBJ_FLAG_HIDDEN);
        StartHolographicAnimations();
    } else {
        StopHolographicAnimations();
        lv_obj_add_flag(holographic_layer_, LV_OBJ_FLAG_HIDDEN);
    }
}

void LcdDisplay::SetHolographicMode(HolographicMode mode) {
    if (holographic_layer_ == nullptr) {
        holographic_mode_ = mode;
        return;
    }

    bool changed = holographic_mode_ != mode;
    holographic_mode_ = mode;

    lv_color_t primary = lv_color_hex(kHoloCyan);
    bool show_energy_bars = false;

    switch (mode) {
        case HolographicMode::kListening:
            primary = lv_color_hex(0x00F0FF);
            break;
        case HolographicMode::kSpeaking:
            primary = lv_color_hex(kHoloBright);
            show_energy_bars = true;
            break;
        case HolographicMode::kMusic:
            primary = lv_color_hex(kHoloPurple);
            show_energy_bars = true;
            break;
        case HolographicMode::kSleep:
            primary = lv_color_hex(kHoloDeep);
            break;
        case HolographicMode::kIdle:
        default:
            break;
    }

    for (auto* bar : holographic_energy_bars_) {
        lv_obj_set_style_bg_color(bar, primary, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_80, 0);
        if (show_energy_bars) {
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (holographic_layer_visible_ && (changed || mode == HolographicMode::kSleep)) {
        StartHolographicAnimations();
    }
}

void LcdDisplay::StartHolographicAnimations() {
    StopHolographicAnimations();
    if (!holographic_layer_visible_ ||
        (holographic_mode_ != HolographicMode::kSpeaking &&
         holographic_mode_ != HolographicMode::kMusic)) {
        return;
    }

    uint32_t energy_time = holographic_mode_ == HolographicMode::kSpeaking ? 600 : 800;

    lv_anim_t energy;
    lv_anim_init(&energy);
    lv_anim_set_var(&energy, this);
    lv_anim_set_values(&energy, 0, 100);
    lv_anim_set_time(&energy, energy_time);
    lv_anim_set_playback_time(&energy, energy_time);
    lv_anim_set_repeat_count(&energy, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&energy, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&energy, HolographicEnergyAnim);
    lv_anim_start(&energy);
}

void LcdDisplay::StopHolographicAnimations() {
    lv_anim_delete(this, nullptr);
}

void LcdDisplay::HolographicEnergyAnim(void* target, int32_t value) {
    auto* self = static_cast<LcdDisplay*>(target);
    if (self == nullptr || self->holographic_energy_bars_[0] == nullptr) {
        return;
    }

    int folded = value <= 50 ? value : 100 - value;
    int heights[3] = {
        6 + value / 8,
        18 - value / 8,
        6 + folded / 4,
    };
    for (int i = 0; i < 3; ++i) {
        lv_obj_set_height(self->holographic_energy_bars_[i], heights[i]);
        lv_obj_align(self->holographic_energy_bars_[i], LV_ALIGN_CENTER, (i - 1) * 10, 94);
    }
}

void LcdDisplay::TriggerBootParticleBurst() {
    static const int dx[4] = {0, 100, 0, -100};
    static const int dy[4] = {-100, 0, 100, 0};

    for (int i = 0; i < 4; ++i) {
        if (boot_particles_[i] == nullptr) {
            continue;
        }
        lv_anim_delete(boot_particles_[i], nullptr);
        lv_obj_align(boot_particles_[i], LV_ALIGN_CENTER, 0, -40);

        lv_anim_t particle;
        lv_anim_init(&particle);
        lv_anim_set_var(&particle, boot_particles_[i]);
        lv_anim_set_values(&particle, 0, 100);
        lv_anim_set_time(&particle, 520);
        lv_anim_set_delay(&particle, i * 45);
        lv_anim_set_path_cb(&particle, lv_anim_path_ease_out);
        lv_anim_set_user_data(&particle, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_anim_set_custom_exec_cb(&particle, [](lv_anim_t* anim, int32_t value) {
            auto* object = static_cast<lv_obj_t*>(anim->var);
            int index = static_cast<int>(reinterpret_cast<intptr_t>(anim->user_data));
            int distance = value * 82 / 100;
            lv_obj_align(object, LV_ALIGN_CENTER,
                distance * dx[index] / 100,
                -40 + distance * dy[index] / 100);
            lv_obj_set_style_bg_opa(object, static_cast<lv_opa_t>(255 - value * 255 / 100), 0);
            int size = 2 + value / 35;
            lv_obj_set_size(object, size, size);
        });
        lv_anim_start(&particle);
    }
}

void LcdDisplay::TriggerTerminalDataBurst() {
    for (int i = 0; i < 4; ++i) {
        if (terminal_particles_[i] == nullptr) {
            continue;
        }
        lv_anim_delete(terminal_particles_[i], nullptr);

        lv_anim_t particle;
        lv_anim_init(&particle);
        lv_anim_set_var(&particle, terminal_particles_[i]);
        lv_anim_set_values(&particle, 0, 100);
        lv_anim_set_time(&particle, 680 + i * 70);
        lv_anim_set_delay(&particle, i * 55);
        lv_anim_set_path_cb(&particle, lv_anim_path_ease_out);
        lv_anim_set_user_data(&particle, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_anim_set_custom_exec_cb(&particle, [](lv_anim_t* anim, int32_t value) {
            auto* object = static_cast<lv_obj_t*>(anim->var);
            int index = static_cast<int>(reinterpret_cast<intptr_t>(anim->user_data));
            int x = (index * 40) - 60;
            int y = 36 - value * 132 / 100;
            lv_obj_align(object, LV_ALIGN_CENTER, x, y - 50);
            int folded = value < 50 ? value : 100 - value;
            lv_obj_set_style_bg_opa(object, static_cast<lv_opa_t>(folded * 5), 0);
        });
        lv_anim_start(&particle);
    }
}

SpiLcdDisplay::SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    // Set the display to on
    ESP_LOGI(TAG, "Turning display on");
    {
        esp_err_t __err = esp_lcd_panel_disp_on_off(panel_, true);
        if (__err == ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "Panel does not support disp_on_off; assuming ON");
        } else {
            ESP_ERROR_CHECK(__err);
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

#if CONFIG_SPIRAM
    // lv image cache, currently only PNG is supported
    size_t psram_size_mb = esp_psram_get_size() / 1024 / 1024;
    if (psram_size_mb >= 8) {
        lv_image_cache_resize(2 * 1024 * 1024, true);
        ESP_LOGI(TAG, "Use 2MB of PSRAM for image cache");
    } else if (psram_size_mb >= 2) {
        lv_image_cache_resize(512 * 1024, true);
        ESP_LOGI(TAG, "Use 512KB of PSRAM for image cache");
    }
#endif

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 5;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
#if CONFIG_BOARD_TYPE_ZHENGCHEN_1_54TFT_WIFI
        .double_buffer = true,
#else
        .double_buffer = false,
#endif
        .trans_size = 0,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    display_ = lvgl_port_add_disp(&display_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}


// RGB LCD implementation
RgbLcdDisplay::RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                           int width, int height, int offset_x, int offset_y,
                           bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    // draw white
    std::vector<uint16_t> buffer(width_, 0xFFFF);
    for (int y = 0; y < height_; y++) {
        esp_lcd_panel_draw_bitmap(panel_, 0, y, width_, y + 1, buffer.data());
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 5;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = panel_io_,
        .panel_handle = panel_,
        .buffer_size = static_cast<uint32_t>(width_ * 20),
        .double_buffer = true,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = 1,
            .swap_bytes = 0,
            .full_refresh = 1,
            .direct_mode = 1,
        },
    };

    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        }
    };
    
    display_ = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add RGB display");
        return;
    }
    
    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

MipiLcdDisplay::MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                            int width, int height,  int offset_x, int offset_y,
                            bool mirror_x, bool mirror_y, bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();

    ESP_LOGI(TAG, "Initialize LVGL port");
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&port_cfg);

    ESP_LOGI(TAG, "Adding LCD display");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = panel_io,
        .panel_handle = panel,
        .control_handle = nullptr,
        .buffer_size = static_cast<uint32_t>(width_ * 50),
        .double_buffer = false,
        .hres = static_cast<uint32_t>(width_),
        .vres = static_cast<uint32_t>(height_),
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = swap_xy,
            .mirror_x = mirror_x,
            .mirror_y = mirror_y,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram =false,
            .sw_rotate = true,
        },
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = false,
        }
    };
    display_ = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);
    if (display_ == nullptr) {
        ESP_LOGE(TAG, "Failed to add display");
        return;
    }

    if (offset_x != 0 || offset_y != 0) {
        lv_display_set_offset(display_, offset_x, offset_y);
    }
}

LcdDisplay::~LcdDisplay() {
    emotion_request_generation_.fetch_add(1, std::memory_order_acq_rel);
    deferred_emotion_.clear();
    SetPreviewImage(nullptr);
    StopHolographicAnimations();
    if (emoji_image_ != nullptr) {
        lv_anim_delete(emoji_image_, EmotionOpacityAnim);
    }
    pending_animation_.reset();
    
    // Clean up animation controller
    if (animation_controller_) {
        animation_controller_->Stop();
        animation_controller_.reset();
    }
    
    if (preview_timer_ != nullptr) {
        esp_timer_stop(preview_timer_);
        esp_timer_delete(preview_timer_);
    }

    if (preview_image_ != nullptr) {
        lv_obj_del(preview_image_);
    }
    if (preview_frame_ != nullptr) {
        lv_obj_del(preview_frame_);
    }
    if (chat_message_label_ != nullptr) {
        lv_obj_del(chat_message_label_);
    }
    if (emoji_label_ != nullptr) {
        lv_obj_del(emoji_label_);
    }
    if (emoji_image_ != nullptr) {
        lv_obj_del(emoji_image_);
    }
    if (emoji_box_ != nullptr) {
        lv_obj_del(emoji_box_);
    }
    if (holographic_layer_ != nullptr) {
        lv_obj_del(holographic_layer_);
    }
    if (content_ != nullptr) {
        lv_obj_del(content_);
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_del(bottom_bar_);
    }
    if (status_bar_ != nullptr) {
        lv_obj_del(status_bar_);
    }
    if (top_bar_ != nullptr) {
        lv_obj_del(top_bar_);
    }
    if (side_bar_ != nullptr) {
        lv_obj_del(side_bar_);
    }
    if (container_ != nullptr) {
        lv_obj_del(container_);
    }
    if (display_ != nullptr) {
        lv_display_delete(display_);
    }

    if (panel_ != nullptr) {
        esp_lcd_panel_del(panel_);
    }
    if (panel_io_ != nullptr) {
        esp_lcd_panel_io_del(panel_io_);
    }
}

bool LcdDisplay::Lock(int timeout_ms) {
    return lvgl_port_lock(timeout_ms);
}

void LcdDisplay::Unlock() {
    lvgl_port_unlock();
}

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_flex_flow(container_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_pad_row(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_color(container_, lvgl_theme->border_color(), 0);

    /* Layer 1: Top bar - for status icons */
    top_bar_ = lv_obj_create(container_);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_50, 0);  // 50% opacity background
    lv_obj_set_style_bg_color(top_bar_, lvgl_theme->background_color(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);

    // Left icon
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lvgl_theme->text_color(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_black(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_black(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.8);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_SIZE_CONTENT);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(status_label_, lvgl_theme->spacing(2), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

    /* Content - Chat area */
    content_ = lv_obj_create(container_);
    lv_obj_set_style_radius(content_, 0, 0);
    lv_obj_set_width(content_, LV_HOR_RES);
    lv_obj_set_flex_grow(content_, 1);
    lv_obj_set_style_pad_all(content_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_border_width(content_, 0, 0);
    lv_obj_set_style_bg_color(content_, lvgl_theme->chat_background_color(), 0); // Background for chat area

    // Enable scrolling for chat content
    lv_obj_set_scrollbar_mode(content_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(content_, LV_DIR_VER);
    
    // Create a flex container for chat messages
    lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(content_, lvgl_theme->spacing(4), 0); // Space between messages

    // We'll create chat messages dynamically in SetChatMessage
    chat_message_label_ = nullptr;

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lvgl_theme->low_battery_color(), 0);
    lv_obj_set_style_radius(low_battery_popup_, lvgl_theme->spacing(4), 0);
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_white(), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    emoji_image_ = lv_img_create(screen);
    lv_obj_align(emoji_image_, LV_ALIGN_TOP_MID, 0, text_font->line_height + lvgl_theme->spacing(8));

    // Display AI logo while booting
    emoji_label_ = lv_label_create(screen);
    lv_obj_center(emoji_label_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);
}
#if CONFIG_IDF_TARGET_ESP32P4
#define  MAX_MESSAGES 40
#else
#define  MAX_MESSAGES 20
#endif
void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: content_ is nullptr (SetupUI() was called but container not created)", role, content);
        }
        return;
    }

    // Check if terminal panel is visible
    if (terminal_panel_ != nullptr && !lv_obj_has_flag(terminal_panel_, LV_OBJ_FLAG_HIDDEN)) {
        if (terminal_label_ != nullptr && content != nullptr) {
            lv_label_set_text(terminal_label_, content);
        }
        return; // Skip normal chat message rendering
    }
    
    // Check if message count exceeds limit
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (child_count >= MAX_MESSAGES) {
        // Delete the oldest message (first child object)
        lv_obj_t* first_child = lv_obj_get_child(content_, 0);
        if (first_child != nullptr) {
            lv_obj_del(first_child);
            // Refresh child count after deletion
            child_count = lv_obj_get_child_cnt(content_);
        }
        // Scroll to the last message immediately (get last_child after deletion)
        if (child_count > 0) {
            lv_obj_t* last_child = lv_obj_get_child(content_, child_count - 1);
            if (last_child != nullptr && lv_obj_is_valid(last_child)) {
                lv_obj_scroll_to_view_recursive(last_child, LV_ANIM_OFF);
            }
        }
    }
    
    // Collapse system messages (if it's a system message, check if the last message is also a system message)
    if (strcmp(role, "system") == 0) {
        // Refresh child count to get accurate count after potential deletion above
        child_count = lv_obj_get_child_cnt(content_);
        if (child_count > 0) {
            // Get the last message container
            lv_obj_t* last_container = lv_obj_get_child(content_, child_count - 1);
            if (last_container != nullptr && lv_obj_is_valid(last_container) && lv_obj_get_child_cnt(last_container) > 0) {
                // Get the bubble inside the container
                lv_obj_t* last_bubble = lv_obj_get_child(last_container, 0);
                if (last_bubble != nullptr && lv_obj_is_valid(last_bubble)) {
                    // Check if bubble type is system message
                    void* bubble_type_ptr = lv_obj_get_user_data(last_bubble);
                    if (bubble_type_ptr != nullptr && strcmp((const char*)bubble_type_ptr, "system") == 0) {
                        // If the last message is also a system message, delete it
                        lv_obj_del(last_container);
                    }
                }
            }
        }
    } else {
        // Hide the centered AI logo
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }

    // Avoid empty message boxes
    if(strlen(content) == 0) {
        return;
    }

    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);

    // Create a message bubble
    lv_obj_t* msg_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(msg_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(msg_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(msg_bubble, 0, 0);
    lv_obj_set_style_pad_all(msg_bubble, lvgl_theme->spacing(4), 0);

    // Create the message text
    lv_obj_t* msg_text = lv_label_create(msg_bubble);
    lv_label_set_text(msg_text, content);
    
    // Calculate bubble width constraints
    lv_coord_t max_width = LV_HOR_RES * 85 / 100 - 16;  // 85% of screen width
    lv_coord_t min_width = 20;  
    
    // Let LVGL calculate the natural text width first
    lv_obj_set_width(msg_text, LV_SIZE_CONTENT);
    lv_obj_update_layout(msg_text);
    lv_coord_t text_width = lv_obj_get_width(msg_text);
    
    // Ensure text width is not less than minimum width
    if (text_width < min_width) {
        text_width = min_width;
    }

    // Constrain to max width
    lv_coord_t bubble_width = (text_width < max_width) ? text_width : max_width;
    
    // Set message text width
    lv_obj_set_width(msg_text, bubble_width);
    lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);

    // Set bubble width
    lv_obj_set_width(msg_bubble, bubble_width);
    lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);

    // Set alignment and style based on message role
    if (strcmp(role, "user") == 0) {
        // User messages are right-aligned with green background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->user_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"user");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "assistant") == 0) {
        // Assistant messages are left-aligned with white background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->assistant_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"assistant");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    } else if (strcmp(role, "system") == 0) {
        // System messages are center-aligned with light gray background
        lv_obj_set_style_bg_color(msg_bubble, lvgl_theme->system_bubble_color(), 0);
        lv_obj_set_style_bg_opa(msg_bubble, LV_OPA_70, 0);
        // Set text color for contrast
        lv_obj_set_style_text_color(msg_text, lvgl_theme->system_text_color(), 0);
        
        // Set custom attribute to mark bubble type
        lv_obj_set_user_data(msg_bubble, (void*)"system");
        
        // Set appropriate width for content
        lv_obj_set_width(msg_bubble, LV_SIZE_CONTENT);
        lv_obj_set_height(msg_bubble, LV_SIZE_CONTENT);
        
        // Don't grow
        lv_obj_set_style_flex_grow(msg_bubble, 0, 0);
    }
    
    // Create a full-width container for user messages to ensure right alignment
    if (strcmp(role, "user") == 0) {
        // Create a full-width container
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        // Make container transparent and borderless
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        // Move the message bubble into this container
        lv_obj_set_parent(msg_bubble, container);
        
        // Right align the bubble in the container
        lv_obj_align(msg_bubble, LV_ALIGN_RIGHT_MID, -25, 0);
        
        // Auto-scroll to this container
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else if (strcmp(role, "system") == 0) {
        // Create full-width container for system messages to ensure center alignment
        lv_obj_t* container = lv_obj_create(content_);
        lv_obj_set_width(container, LV_HOR_RES);
        lv_obj_set_height(container, LV_SIZE_CONTENT);
        
        lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(container, 0, 0);
        lv_obj_set_style_pad_all(container, 0, 0);
        
        lv_obj_set_parent(msg_bubble, container);
        lv_obj_align(msg_bubble, LV_ALIGN_CENTER, 0, 0);
        lv_obj_scroll_to_view_recursive(container, LV_ANIM_ON);
    } else {
        // For assistant messages
        // Left align assistant messages
        lv_obj_align(msg_bubble, LV_ALIGN_LEFT_MID, 0, 0);

        // Auto-scroll to the message bubble
        lv_obj_scroll_to_view_recursive(msg_bubble, LV_ANIM_ON);
    }
    
    // Store reference to the latest message label
    chat_message_label_ = msg_text;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }

    if (image == nullptr) {
        return;
    }
    
    auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    // Create a message bubble for image preview
    lv_obj_t* img_bubble = lv_obj_create(content_);
    lv_obj_set_style_radius(img_bubble, 8, 0);
    lv_obj_set_scrollbar_mode(img_bubble, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(img_bubble, 0, 0);
    lv_obj_set_style_pad_all(img_bubble, lvgl_theme->spacing(4), 0);
    
    // Set image bubble background color (similar to system message)
    lv_obj_set_style_bg_color(img_bubble, lvgl_theme->assistant_bubble_color(), 0);
    lv_obj_set_style_bg_opa(img_bubble, LV_OPA_70, 0);
    
    // Set custom attribute to mark bubble type
    lv_obj_set_user_data(img_bubble, (void*)"image");

    // Create the image object inside the bubble
    lv_obj_t* preview_image = lv_image_create(img_bubble);
    
    // Calculate appropriate size for the image
    lv_coord_t max_width = LV_HOR_RES * 70 / 100;  // 70% of screen width
    lv_coord_t max_height = LV_VER_RES * 50 / 100; // 50% of screen height
    
    // Calculate zoom factor to fit within maximum dimensions
    auto img_dsc = image->image_dsc();
    lv_coord_t img_width = img_dsc->header.w;
    lv_coord_t img_height = img_dsc->header.h;
    if (img_width == 0 || img_height == 0) {
        img_width = max_width;
        img_height = max_height;
        ESP_LOGW(TAG, "Invalid image dimensions: %ld x %ld, using default dimensions: %ld x %ld", img_width, img_height, max_width, max_height);
    }
    
    lv_coord_t zoom_w = (max_width * 256) / img_width;
    lv_coord_t zoom_h = (max_height * 256) / img_height;
    lv_coord_t zoom = (zoom_w < zoom_h) ? zoom_w : zoom_h;
    
    // Ensure zoom doesn't exceed 256 (100%)
    if (zoom > 256) zoom = 256;
    
    // Set image properties
    lv_image_set_src(preview_image, img_dsc);
    lv_image_set_scale(preview_image, zoom);
    
    // Add event handler to clean up LvglImage when image is deleted
    // We need to transfer ownership of the unique_ptr to the event callback
    LvglImage* raw_image = image.release(); // Release ownership of smart pointer
    lv_obj_add_event_cb(preview_image, [](lv_event_t* e) {
        LvglImage* img = (LvglImage*)lv_event_get_user_data(e);
        if (img != nullptr) {
            delete img; // Properly release memory by deleting LvglImage object
        }
    }, LV_EVENT_DELETE, (void*)raw_image);
    
    // Calculate actual scaled image dimensions
    lv_coord_t scaled_width = (img_width * zoom) / 256;
    lv_coord_t scaled_height = (img_height * zoom) / 256;
    
    // Set bubble size to be 16 pixels larger than the image (8 pixels on each side)
    lv_obj_set_width(img_bubble, scaled_width + 16);
    lv_obj_set_height(img_bubble, scaled_height + 16);
    
    // Don't grow in flex layout
    lv_obj_set_style_flex_grow(img_bubble, 0, 0);
    
    // Center the image within the bubble
    lv_obj_center(preview_image);
    
    // Left align the image bubble like assistant messages
    lv_obj_align(img_bubble, LV_ALIGN_LEFT_MID, 0, 0);

    // Auto-scroll to the image bubble
    lv_obj_scroll_to_view_recursive(img_bubble, LV_ANIM_ON);
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (content_ == nullptr) {
        return;
    }
    
    // Use lv_obj_clean to delete all children of content_ (chat message bubbles)
    lv_obj_clean(content_);
    
    // Reset chat_message_label_ as it has been deleted
    chat_message_label_ = nullptr;
    
    // Show the centered AI logo (emoji_label_) again
    if (emoji_label_ != nullptr) {
        lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    
    ESP_LOGI(TAG, "Chat messages cleared");
}
#else
void LcdDisplay::SetupUI() {
    // Prevent duplicate calls - if already called, return early
    if (setup_ui_called_) {
        ESP_LOGW(TAG, "SetupUI() called multiple times, skipping duplicate call");
        return;
    }
    
    Display::SetupUI();  // Mark SetupUI as called
    DisplayLockGuard lock(this);
    LvglTheme* lvgl_theme = static_cast<LvglTheme*>(current_theme_);
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    auto screen = lv_screen_active();
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);
    lv_obj_set_style_bg_color(screen, lvgl_theme->background_color(), 0);

    /* Container - used as background */
    container_ = lv_obj_create(screen);
    lv_obj_set_size(container_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(container_, 0, 0);
    lv_obj_set_style_pad_all(container_, 0, 0);
    lv_obj_set_style_border_width(container_, 0, 0);
    lv_obj_set_style_bg_color(container_, lv_color_black(), 0); // 全息投影纯黑底色

    /* Bottom layer: emoji_box_ - centered display */
    emoji_box_ = lv_obj_create(screen);
    lv_obj_set_size(emoji_box_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(emoji_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(emoji_box_, 0, 0);
    lv_obj_set_style_border_width(emoji_box_, 0, 0);
    lv_obj_align(emoji_box_, LV_ALIGN_CENTER, 0, 0);

    emoji_label_ = lv_label_create(emoji_box_);
    lv_obj_set_style_text_font(emoji_label_, large_icon_font, 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    lv_label_set_text(emoji_label_, FONT_AWESOME_MICROCHIP_AI);

    emoji_image_ = lv_img_create(emoji_box_);
    lv_obj_center(emoji_image_);
    lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    /* Compact status energy layer above the realistic character. */
    SetupHolographicLayer(screen);

    /* Middle layer: framed preview image. */
    preview_frame_ = lv_obj_create(screen);
    lv_obj_set_size(preview_frame_, 216, 216);
    lv_obj_align(preview_frame_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(preview_frame_, 0, 0);
    lv_obj_set_style_pad_all(preview_frame_, 6, 0);
    lv_obj_set_style_bg_color(preview_frame_, lv_color_hex(kHoloBlack), 0);
    lv_obj_set_style_bg_opa(preview_frame_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(preview_frame_, 1, 0);
    lv_obj_set_style_border_color(preview_frame_, lv_color_hex(kHoloCyan), 0);
    lv_obj_set_style_border_opa(preview_frame_, LV_OPA_70, 0);
    lv_obj_set_scrollbar_mode(preview_frame_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(preview_frame_, LV_OBJ_FLAG_CLICKABLE);

    preview_image_ = lv_image_create(preview_frame_);
    lv_obj_align(preview_image_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(preview_frame_, LV_OBJ_FLAG_HIDDEN);

    /* Layer 1: Top bar - for status icons (全息投影：纯黑背景) */
    top_bar_ = lv_obj_create(screen);
    lv_obj_set_size(top_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(top_bar_, 0, 0);
    lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(top_bar_, lv_color_black(), 0);
    lv_obj_set_style_border_width(top_bar_, 0, 0);
    lv_obj_set_style_shadow_width(top_bar_, 0, 0);
    lv_obj_set_style_pad_all(top_bar_, 0, 0);
    lv_obj_set_style_pad_top(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(top_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_left(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(top_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_flex_flow(top_bar_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(top_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(top_bar_, LV_ALIGN_TOP_MID, 0, 0);

    // Left icon (全息投影 → 纯白)
    network_label_ = lv_label_create(top_bar_);
    lv_label_set_text(network_label_, "");
    lv_obj_set_style_text_font(network_label_, icon_font, 0);
    lv_obj_set_style_text_color(network_label_, lv_color_white(), 0);

    // Right icons container
    lv_obj_t* right_icons = lv_obj_create(top_bar_);
    lv_obj_set_size(right_icons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_icons, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_icons, 0, 0);
    lv_obj_set_style_pad_all(right_icons, 0, 0);
    lv_obj_set_flex_flow(right_icons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    mute_label_ = lv_label_create(right_icons);
    lv_label_set_text(mute_label_, "");
    lv_obj_set_style_text_font(mute_label_, icon_font, 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_white(), 0);

    battery_label_ = lv_label_create(right_icons);
    lv_label_set_text(battery_label_, "");
    lv_obj_set_style_text_font(battery_label_, icon_font, 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_white(), 0);
    lv_obj_set_style_margin_left(battery_label_, lvgl_theme->spacing(2), 0);

    /* Layer 2: Status bar - for center text labels */
    status_bar_ = lv_obj_create(screen);
    lv_obj_set_size(status_bar_, LV_HOR_RES, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(status_bar_, 0, 0);
    lv_obj_set_style_bg_opa(status_bar_, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_border_width(status_bar_, 0, 0);
    lv_obj_set_style_pad_all(status_bar_, 0, 0);
    lv_obj_set_style_pad_top(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_style_pad_bottom(status_bar_, lvgl_theme->spacing(2), 0);
    lv_obj_set_scrollbar_mode(status_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_layout(status_bar_, LV_LAYOUT_NONE, 0);  // Use absolute positioning
    lv_obj_align(status_bar_, LV_ALIGN_TOP_MID, 0, 0);  // Overlap with top_bar_

    notification_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(notification_label_, LV_HOR_RES * 0.75);
    lv_obj_set_style_text_align(notification_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_hex(kHoloText), 0);
    lv_obj_set_style_bg_color(notification_label_, lv_color_hex(0x00131C), 0);
    lv_obj_set_style_bg_opa(notification_label_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(notification_label_, 1, 0);
    lv_obj_set_style_border_side(notification_label_,
        static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0);
    lv_obj_set_style_border_color(notification_label_, lv_color_hex(kHoloCyan), 0);
    lv_obj_set_style_border_opa(notification_label_, LV_OPA_70, 0);
    lv_obj_set_style_pad_hor(notification_label_, 6, 0);
    lv_obj_set_style_pad_ver(notification_label_, 2, 0);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    status_label_ = lv_label_create(status_bar_);
    lv_obj_set_width(status_label_, LV_SIZE_CONTENT);
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kHoloText), 0);
    lv_obj_set_style_pad_hor(status_label_, lvgl_theme->spacing(2), 0);
    lv_label_set_text(status_label_, Lang::Strings::INITIALIZING);
    lv_obj_align(status_label_, LV_ALIGN_CENTER, 0, 0);

#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    /* Bottom bar - auto height, grows upward with wrapped text */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_width(bottom_bar_, LV_HOR_RES);
    lv_obj_set_height(bottom_bar_, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_bar_, 0, 0);
    lv_obj_set_style_shadow_width(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_all(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, multiline wrapped display */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#else
    /* Top layer: Bottom bar - fixed height at bottom */
    bottom_bar_ = lv_obj_create(screen);
    lv_obj_set_size(bottom_bar_, LV_HOR_RES, text_font->line_height + lvgl_theme->spacing(8));
    lv_obj_set_style_radius(bottom_bar_, 0, 0);
    
    lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(kHoloBlack), 0);
    lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_70, 0);
    lv_obj_set_style_border_width(bottom_bar_, 1, 0);
    lv_obj_set_style_border_side(bottom_bar_, (lv_border_side_t)LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(kHoloCyan), 0);
    lv_obj_set_style_border_opa(bottom_bar_, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(bottom_bar_, 0, 0);
    lv_obj_set_style_text_color(bottom_bar_, lv_color_hex(kHoloText), 0);
    lv_obj_set_style_pad_all(bottom_bar_, 0, 0);
    lv_obj_set_style_pad_left(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_style_pad_right(bottom_bar_, lvgl_theme->spacing(4), 0);
    lv_obj_set_scrollbar_mode(bottom_bar_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* chat_message_label_ placed in bottom_bar_, single-line horizontal scroll */
    chat_message_label_ = lv_label_create(bottom_bar_);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, LV_HOR_RES - lvgl_theme->spacing(8));
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(chat_message_label_, lvgl_theme->text_color(), 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_CENTER, 0, 0);

    // Start scrolling after a delay (short text won't scroll)
    static lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_delay(&a, 1000);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_obj_set_style_anim(chat_message_label_, &a, LV_PART_MAIN);
    lv_obj_set_style_anim_duration(chat_message_label_, lv_anim_speed_clamped(60, 300, 60000), LV_PART_MAIN);
    lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);  // Hide until there is content
#endif

    low_battery_popup_ = lv_obj_create(screen);
    lv_obj_set_scrollbar_mode(low_battery_popup_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(low_battery_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
    lv_obj_align(low_battery_popup_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(4));
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0x240308), 0);
    lv_obj_set_style_bg_opa(low_battery_popup_, LV_OPA_90, 0);
    lv_obj_set_style_radius(low_battery_popup_, 0, 0);
    lv_obj_set_style_border_width(low_battery_popup_, 2, 0);
    lv_obj_set_style_border_side(low_battery_popup_,
        static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_hex(kHoloDanger), 0);
    
    low_battery_label_ = lv_label_create(low_battery_popup_);
    lv_label_set_text(low_battery_label_, Lang::Strings::BATTERY_NEED_CHARGE);
    lv_obj_set_style_text_color(low_battery_label_, lv_color_hex(0xFFE6E9), 0);
    lv_obj_center(low_battery_label_);
    lv_obj_add_flag(low_battery_popup_, LV_OBJ_FLAG_HIDDEN);

    /* ========== 科技风 配网/OTA 终端数据面板 ========== */
    terminal_panel_ = lv_obj_create(screen);
    lv_obj_set_size(terminal_panel_, LV_HOR_RES, LV_VER_RES); // 占据全屏
    lv_obj_align(terminal_panel_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(terminal_panel_, lv_color_hex(0x000A14), 0);
    lv_obj_set_style_bg_opa(terminal_panel_, LV_OPA_90, 0);
    lv_obj_set_style_border_width(terminal_panel_, 0, 0);
    lv_obj_set_style_pad_all(terminal_panel_, 0, 0);
    lv_obj_set_scrollbar_mode(terminal_panel_, LV_SCROLLBAR_MODE_OFF);
    
    // 1. 雷达背景圈
    radar_bg_ = lv_arc_create(terminal_panel_);
    lv_obj_set_size(radar_bg_, 140, 140);
    lv_obj_align(radar_bg_, LV_ALIGN_CENTER, 0, -50); // 向上移动30px (原-20)
    lv_arc_set_bg_angles(radar_bg_, 0, 360);
    lv_arc_set_angles(radar_bg_, 0, 0);
    lv_obj_set_style_arc_width(radar_bg_, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_color(radar_bg_, lv_color_hex(0x004466), LV_PART_MAIN); // 暗青色背景环
    lv_obj_remove_style(radar_bg_, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(radar_bg_, LV_OBJ_FLAG_CLICKABLE);

    // 2. 雷达扫描指针
    radar_scanner_ = lv_arc_create(terminal_panel_);
    lv_obj_set_size(radar_scanner_, 140, 140);
    lv_obj_align(radar_scanner_, LV_ALIGN_CENTER, 0, -50); // 向上移动30px
    lv_arc_set_bg_angles(radar_scanner_, 0, 0);
    lv_arc_set_angles(radar_scanner_, 0, 45); // 45度扇形扫描指针
    lv_obj_set_style_arc_width(radar_scanner_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(radar_scanner_, lv_color_hex(0x00D4FF), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(radar_scanner_, true, LV_PART_INDICATOR);
    lv_obj_remove_style(radar_scanner_, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(radar_scanner_, LV_OBJ_FLAG_CLICKABLE);

    // 3. 中心能量核心
    radar_core_ = lv_obj_create(terminal_panel_);
    lv_obj_set_size(radar_core_, 8, 8);
    lv_obj_set_style_radius(radar_core_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(radar_core_, lv_color_hex(0x00D4FF), 0);
    lv_obj_set_style_bg_opa(radar_core_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(radar_core_, 0, 0);
    lv_obj_set_style_shadow_color(radar_core_, lv_color_hex(0x00D4FF), 0);
    lv_obj_set_style_shadow_width(radar_core_, 15, 0);
    lv_obj_set_style_shadow_opa(radar_core_, LV_OPA_80, 0);
    lv_obj_align(radar_core_, LV_ALIGN_CENTER, 0, -50); // 向上移动30px

    // 4. 四角瞄准框 (Targeting Brackets)
    for (int i = 0; i < 4; i++) {
        brackets_[i] = lv_obj_create(terminal_panel_);
        lv_obj_set_size(brackets_[i], 20, 20);
        lv_obj_set_style_bg_opa(brackets_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(brackets_[i], lv_color_hex(0x00D4FF), 0);
        lv_obj_set_style_border_width(brackets_[i], 3, 0);
        lv_obj_set_style_radius(brackets_[i], 0, 0);
        
        lv_border_side_t side = LV_BORDER_SIDE_NONE;
        // 0: 左上, 1: 右上, 2: 左下, 3: 右下 (全部向上移动30px)
        if (i == 0) { side = (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT); lv_obj_align(brackets_[i], LV_ALIGN_CENTER, -85, -135); }
        if (i == 1) { side = (lv_border_side_t)(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_RIGHT); lv_obj_align(brackets_[i], LV_ALIGN_CENTER, 85, -135); }
        if (i == 2) { side = (lv_border_side_t)(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT); lv_obj_align(brackets_[i], LV_ALIGN_CENTER, -85, 35); }
        if (i == 3) { side = (lv_border_side_t)(LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT); lv_obj_align(brackets_[i], LV_ALIGN_CENTER, 85, 35); }
        lv_obj_set_style_border_side(brackets_[i], side, 0);
    }
    
    // 5. 数据粒子只在状态变化时短暂触发，不持续占用刷新预算。
    for (int i = 0; i < 4; i++) {
        terminal_particles_[i] = lv_obj_create(terminal_panel_);
        lv_obj_set_size(terminal_particles_[i], 3, 3);
        lv_obj_set_style_radius(terminal_particles_[i], LV_RADIUS_CIRCLE, 0);
        lv_color_t p_color = (i % 2 == 0) ? lv_color_hex(kHoloCyan) : lv_color_hex(kHoloPurple);
        lv_obj_set_style_bg_color(terminal_particles_[i], p_color, 0);
        lv_obj_set_style_bg_opa(terminal_particles_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(terminal_particles_[i], 0, 0);
    }

    // 6. 底部文字提示区域
    terminal_label_ = lv_label_create(terminal_panel_);
    lv_obj_set_width(terminal_label_, LV_HOR_RES * 0.9);
    lv_label_set_long_mode(terminal_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(terminal_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(terminal_label_, lv_color_hex(0x00D4FF), 0); // 青色文字
    lv_obj_set_style_text_font(terminal_label_, text_font, 0);
    lv_obj_align(terminal_label_, LV_ALIGN_BOTTOM_MID, 0, -lvgl_theme->spacing(2)); // 向下移动，远离上方雷达特效框
    lv_label_set_text(terminal_label_, "");
    
    lv_obj_add_flag(terminal_panel_, LV_OBJ_FLAG_HIDDEN);

    /* ========== 方案C: 星际引擎 (Stellar Engine) 启动界面 ========== */
    // 1. 全屏半透明遮罩层 - 深空黑蓝背景
    boot_overlay_ = lv_obj_create(screen);
    lv_obj_set_size(boot_overlay_, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_radius(boot_overlay_, 0, 0);
    lv_obj_set_style_bg_color(boot_overlay_, lv_color_hex(0x050510), 0);
    lv_obj_set_style_bg_opa(boot_overlay_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_overlay_, 0, 0);
    lv_obj_set_style_pad_all(boot_overlay_, 0, 0);
    lv_obj_set_scrollbar_mode(boot_overlay_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(boot_overlay_, LV_ALIGN_CENTER, 0, 0);

    // 2. 中心能量核心 (呼吸脉冲圆点)
    boot_core_ = lv_obj_create(boot_overlay_);
    lv_obj_set_size(boot_core_, 16, 16);
    lv_obj_set_style_radius(boot_core_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(boot_core_, lv_color_hex(0x00D4FF), 0);
    lv_obj_set_style_bg_opa(boot_core_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(boot_core_, 0, 0);
    lv_obj_set_style_shadow_color(boot_core_, lv_color_hex(0x00D4FF), 0);
    lv_obj_set_style_shadow_width(boot_core_, 15, 0);
    lv_obj_set_style_shadow_opa(boot_core_, LV_OPA_80, 0);
    lv_obj_align(boot_core_, LV_ALIGN_CENTER, 0, -40);

    lv_anim_t core_anim;
    lv_anim_init(&core_anim);
    lv_anim_set_var(&core_anim, boot_core_);
    lv_anim_set_values(&core_anim, 10, 20); // 改变大小
    lv_anim_set_time(&core_anim, 800);
    lv_anim_set_playback_time(&core_anim, 800);
    lv_anim_set_repeat_count(&core_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&core_anim, lv_anim_path_ease_in_out);
    lv_anim_set_custom_exec_cb(&core_anim, [](lv_anim_t* anim, int32_t val) {
        lv_obj_set_size((lv_obj_t*)anim->var, val, val);
        lv_obj_align((lv_obj_t*)anim->var, LV_ALIGN_CENTER, 0, -40); // 重新居中
    });
    lv_anim_start(&core_anim);

    // 2.5 四向粒子仅在启动状态变化时喷射一次。
    for (int i = 0; i < 4; i++) {
        boot_particles_[i] = lv_obj_create(boot_overlay_);
        lv_obj_set_size(boot_particles_[i], 4, 4);
        lv_obj_set_style_radius(boot_particles_[i], LV_RADIUS_CIRCLE, 0);
        lv_color_t p_color = (i % 2 == 0) ? lv_color_hex(kHoloCyan) : lv_color_hex(kHoloBright);
        lv_obj_set_style_bg_color(boot_particles_[i], p_color, 0);
        lv_obj_set_style_bg_opa(boot_particles_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(boot_particles_[i], 0, 0);
        lv_obj_align(boot_particles_[i], LV_ALIGN_CENTER, 0, -40);
    }

    // 3. 单分段环形加载动画；与核心呼吸组成两组持续动画。
    boot_spinner_ = lv_arc_create(boot_overlay_);
    lv_arc_set_rotation(boot_spinner_, 270);
    lv_arc_set_bg_angles(boot_spinner_, 0, 360);
    lv_arc_set_angles(boot_spinner_, 0, 105);
    lv_obj_remove_style(boot_spinner_, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(boot_spinner_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(boot_spinner_, 90, 90);
    lv_obj_align(boot_spinner_, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_arc_color(boot_spinner_, lv_color_hex(kHoloDeep), LV_PART_MAIN);
    lv_obj_set_style_arc_width(boot_spinner_, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(boot_spinner_, lv_color_hex(kHoloCyan), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(boot_spinner_, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(boot_spinner_, true, LV_PART_INDICATOR);

    lv_anim_t spin_outer;
    lv_anim_init(&spin_outer);
    lv_anim_set_var(&spin_outer, boot_spinner_);
    lv_anim_set_values(&spin_outer, 0, 360);
    lv_anim_set_time(&spin_outer, 1600);
    lv_anim_set_repeat_count(&spin_outer, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&spin_outer, [](void* obj, int32_t val) {
        lv_arc_set_rotation((lv_obj_t*)obj, val);
    });
    lv_anim_start(&spin_outer);

    // 4. 科技风 HUD 信息卡片 - spinner 下方
    boot_card_ = lv_obj_create(boot_overlay_);
    lv_obj_set_size(boot_card_, LV_HOR_RES * 0.85, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(boot_card_, 0, 0); // 锋利的直角
    lv_obj_set_style_bg_color(boot_card_, lv_color_hex(0x001122), 0); // 更深邃的科幻底色
    lv_obj_set_style_bg_opa(boot_card_, LV_OPA_60, 0);
    lv_obj_set_style_border_width(boot_card_, 2, 0);
    lv_obj_set_style_border_side(boot_card_, static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0); // 仅左右边框，HUD风格
    lv_obj_set_style_border_color(boot_card_, lv_color_hex(0x00D4FF), 0);
    lv_obj_set_style_border_opa(boot_card_, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(boot_card_, lvgl_theme->spacing(2), 0);
    // 增加外发光
    lv_obj_set_style_shadow_width(boot_card_, 15, 0);
    lv_obj_set_style_shadow_color(boot_card_, lv_color_hex(0x00D4FF), 0);
    lv_obj_set_style_shadow_opa(boot_card_, LV_OPA_30, 0);
    // 上下微弱科技感内描边
    lv_obj_set_style_outline_width(boot_card_, 1, 0);
    lv_obj_set_style_outline_color(boot_card_, lv_color_hex(0x00D4FF), 0);
    lv_obj_set_style_outline_opa(boot_card_, LV_OPA_20, 0);
    
    lv_obj_set_scrollbar_mode(boot_card_, LV_SCROLLBAR_MODE_OFF);
    // 使用 flex 布局让文字和进度条垂直排列
    lv_obj_set_flex_flow(boot_card_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(boot_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(boot_card_, LV_ALIGN_CENTER, 0, 50);

    // 6. 卡片内状态文字 (HUD 终端字体风格)
    boot_status_label_ = lv_label_create(boot_card_);
    lv_label_set_text(boot_status_label_, Lang::Strings::INITIALIZING); 
    lv_obj_set_style_text_color(boot_status_label_, lv_color_hex(0x00FFFF), 0); // 高亮青色
    lv_obj_set_style_text_letter_space(boot_status_label_, 2, 0); // 字间距，增加科技感
    lv_obj_set_width(boot_status_label_, LV_PCT(100));
    lv_obj_set_style_text_align(boot_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(boot_status_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    
    // 7. 卡片内微型进度条 (HUD 数据线风格)
    boot_progress_bar_ = lv_bar_create(boot_card_);
    lv_obj_set_size(boot_progress_bar_, LV_PCT(100), 2); // 更细，横跨满屏
    lv_obj_set_style_radius(boot_progress_bar_, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(boot_progress_bar_, 0, LV_PART_INDICATOR);
    lv_obj_set_style_margin_top(boot_progress_bar_, 6, 0);
    lv_obj_set_style_bg_color(boot_progress_bar_, lv_color_hex(0x002244), LV_PART_MAIN);
    lv_obj_set_style_bg_color(boot_progress_bar_, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
    // 为进度条添加发光效果
    lv_obj_set_style_shadow_color(boot_progress_bar_, lv_color_hex(0x00FFFF), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(boot_progress_bar_, 8, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(boot_progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(boot_progress_bar_, 300, 0); // 响应更快
    lv_bar_set_value(boot_progress_bar_, 0, LV_ANIM_OFF);
    current_boot_progress_ = 0;
    
    is_booting_ = true;
}

void LcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    std::string deferred_to_apply;
    {
        DisplayLockGuard lock(this);
        if (preview_image_ == nullptr || preview_frame_ == nullptr) {
            ESP_LOGE(TAG, "Preview image is not initialized");
            return;
        }

        if (image == nullptr) {
            esp_timer_stop(preview_timer_);
            lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(preview_frame_, LV_OBJ_FLAG_HIDDEN);
            preview_image_cached_.reset();
            if (animation_controller_) {
                animation_controller_->Resume();
            }
            if (!is_booting_ && !is_terminal_mode_) {
                SetHolographicLayerVisible(true);
                deferred_to_apply = std::move(deferred_emotion_);
                deferred_emotion_.clear();
            }
        } else {
            preview_image_cached_ = std::move(image);
            auto img_dsc = preview_image_cached_->image_dsc();
            lv_image_set_src(preview_image_, img_dsc);
            if (img_dsc->header.w > 0 && img_dsc->header.h > 0) {
                constexpr int kPreviewMaxSize = 200;
                int32_t width_scale = 256 * kPreviewMaxSize / img_dsc->header.w;
                int32_t height_scale = 256 * kPreviewMaxSize / img_dsc->header.h;
                int32_t scale = std::min<int32_t>(256, std::min(width_scale, height_scale));
                lv_image_set_scale(preview_image_, scale);
                int32_t scaled_width = img_dsc->header.w * scale / 256;
                int32_t scaled_height = img_dsc->header.h * scale / 256;
                lv_obj_set_size(preview_frame_, scaled_width + 12, scaled_height + 12);
                lv_obj_align(preview_frame_, LV_ALIGN_CENTER, 0, 0);
            }

            if (animation_controller_) {
                animation_controller_->Pause();
            }
            SetHolographicLayerVisible(false);
            lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(preview_frame_, LV_OBJ_FLAG_HIDDEN);
            esp_timer_stop(preview_timer_);
            ESP_ERROR_CHECK(esp_timer_start_once(preview_timer_, PREVIEW_IMAGE_DURATION_MS * 1000));
        }
    }

    if (!deferred_to_apply.empty()) {
        ESP_LOGI(TAG, "Apply deferred preview emotion=%s", deferred_to_apply.c_str());
        SetEmotion(deferred_to_apply.c_str());
    }
}

void LcdDisplay::SetChatMessage(const char* role, const char* content) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetChatMessage('%s', '%s') called before SetupUI() - message will be lost!", role, content);
    }
    DisplayLockGuard lock(this);
    if (chat_message_label_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetChatMessage('%s', '%s') failed: chat_message_label_ is nullptr (SetupUI() was called but label not created)", role, content);
        }
        return;
    }

    // Check if terminal panel is visible
    if (terminal_panel_ != nullptr && !lv_obj_has_flag(terminal_panel_, LV_OBJ_FLAG_HIDDEN)) {
        if (terminal_label_ != nullptr && content != nullptr) {
            lv_label_set_text(terminal_label_, content);
        }
        if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        return; // Skip normal chat message rendering
    }
    lv_label_set_text(chat_message_label_, content);
    // Show bottom_bar_ only when there is content (and subtitle is not globally hidden)
    if (bottom_bar_ != nullptr) {
        if (content == nullptr || content[0] == '\0') {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else if (!hide_subtitle_) {
            lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
#if CONFIG_USE_MULTILINE_CHAT_MESSAGE
    // Re-align bottom_bar_ after text change so it stays anchored to the bottom
    // as its height adapts to the wrapped content.
    if (bottom_bar_ != nullptr) {
        lv_obj_align(bottom_bar_, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
#endif
}

void LcdDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    // In non-wechat mode, just clear the chat message label and hide the bar
    if (chat_message_label_ != nullptr) {
        lv_label_set_text(chat_message_label_, "");
    }
    if (bottom_bar_ != nullptr) {
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

void LcdDisplay::SetEmotion(const char* emotion) {
    // The first frame is decoded outside the LVGL lock. The current animation
    // therefore remains visible while the pending player is prepared.
    if (emotion == nullptr || emotion[0] == '\0') {
        ESP_LOGW(TAG, "Ignore empty emotion request");
        return;
    }
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetEmotion('%s') called before SetupUI() - emotion will not be displayed!", emotion);
    }

    if (emoji_image_ == nullptr) {
        if (setup_ui_called_) {
            ESP_LOGW(TAG, "SetEmotion('%s') failed: emoji_image_ is nullptr (SetupUI() was called but emoji image not created)", emotion);
        }
        return;
    }

    auto emoji_collection = static_cast<LvglTheme*>(current_theme_)->emoji_collection();
    const char* canonical = nullptr;
    HolographicMode next_holographic_mode = HolographicMode::kSpeaking;

    if (strcmp(emotion, "neutral") == 0 || strcmp(emotion, "relaxed") == 0 || strcmp(emotion, "confident") == 0) {
        canonical = "neutral";
        next_holographic_mode = HolographicMode::kIdle;
    } else if (strcmp(emotion, "listening") == 0) {
        canonical = "listening";
        next_holographic_mode = HolographicMode::kListening;
    } else if (strcmp(emotion, "speaking") == 0 || strcmp(emotion, "thinking") == 0) {
        canonical = "speaking";
    } else if (strcmp(emotion, "happy") == 0 || strcmp(emotion, "funny") == 0 ||
               strcmp(emotion, "laughing") == 0 || strcmp(emotion, "delicious") == 0 ||
               strcmp(emotion, "winking") == 0 || strcmp(emotion, "silly") == 0 ||
               strcmp(emotion, "cool") == 0 || strcmp(emotion, "kissy") == 0 ||
               strcmp(emotion, "loving") == 0 || strcmp(emotion, "excited") == 0) {
        canonical = "happy";
    } else if (strcmp(emotion, "surprised") == 0 || strcmp(emotion, "shocked") == 0 ||
               strcmp(emotion, "confused") == 0) {
        canonical = "surprised";
    } else if (strcmp(emotion, "sad") == 0 || strcmp(emotion, "crying") == 0 ||
               strcmp(emotion, "embarrassed") == 0) {
        canonical = "sad";
    } else if (strcmp(emotion, "angry") == 0) {
        canonical = "angry";
    } else if (strcmp(emotion, "dancing") == 0) {
        canonical = "dancing";
        next_holographic_mode = HolographicMode::kMusic;
    } else if (strcmp(emotion, "sleepy") == 0 || strcmp(emotion, "sleep") == 0) {
        canonical = "sleepy";
        next_holographic_mode = HolographicMode::kSleep;
    } else {
        ESP_LOGW(TAG, "Unknown display emotion '%s'; keeping state-safe neutral animation", emotion);
        canonical = "neutral";
        next_holographic_mode = HolographicMode::kIdle;
    }

    const char* animation_name = nullptr;
    const char* legacy_gif_name = nullptr;

    if (strcmp(canonical, "neutral") == 0) {
        animation_name = "dog_neutral.mjp";
        legacy_gif_name = "dog_neutral.gif";
    } else if (strcmp(canonical, "listening") == 0) {
        animation_name = "dog_listening.mjp";
        legacy_gif_name = "dog_listening.gif";
    } else if (strcmp(canonical, "dancing") == 0) {
        animation_name = "dog_dancing.mjp";
        legacy_gif_name = "dog_dancing.gif";
    } else if (strcmp(canonical, "speaking") == 0) {
        animation_name = "dog_speaking.mjp";
        legacy_gif_name = "dog_speaking.gif";
    } else if (strcmp(canonical, "sleepy") == 0) {
        animation_name = "dog_sleepy.mjp";
        legacy_gif_name = "dog_sleepy.gif";
    } else if (strcmp(canonical, "angry") == 0) {
        animation_name = "dog_angry.mjp";
        legacy_gif_name = "dog_angry.gif";
    } else if (strcmp(canonical, "sad") == 0) {
        animation_name = "dog_sad.mjp";
        legacy_gif_name = "dog_sad.gif";
    } else if (strcmp(canonical, "surprised") == 0) {
        animation_name = "dog_surprised.mjp";
        legacy_gif_name = "dog_surprised.gif";
    } else if (strcmp(canonical, "happy") == 0) {
        animation_name = "dog_happy.mjp";
        legacy_gif_name = "dog_happy.gif";
    }

    {
        DisplayLockGuard lock(this);
        const bool preview_visible = preview_frame_ != nullptr &&
                                     !lv_obj_has_flag(preview_frame_, LV_OBJ_FLAG_HIDDEN);
        if (is_terminal_mode_ || preview_visible) {
            emotion_request_generation_.fetch_add(1, std::memory_order_acq_rel);
            deferred_emotion_ = canonical;
            ESP_LOGI(TAG, "Defer emotion=%s terminal=%d preview=%d", canonical,
                     is_terminal_mode_, preview_visible);
            return;
        }
        const bool same_animation = animation_controller_ &&
            (current_animation_name_ == animation_name || current_animation_name_ == legacy_gif_name);
        if (same_animation) {
            return;
        }
        if (pending_animation_ &&
            (pending_animation_name_ == animation_name || pending_animation_name_ == legacy_gif_name)) {
            return;
        }
    }

    const uint32_t request_generation =
        emotion_request_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

    std::unique_ptr<LvglAnimation> next_animation;
    std::string loaded_animation_name;
    if (animation_name != nullptr) {
        void* ptr = nullptr;
        size_t size = 0;
        if (Assets::GetInstance().GetAssetData(animation_name, ptr, size)) {
            auto player = std::make_unique<LvglMjpeg>(ptr, size);
            if (player->IsLoaded()) {
                next_animation = std::move(player);
                loaded_animation_name = animation_name;
            }
        }
        if (!next_animation && legacy_gif_name != nullptr &&
            Assets::GetInstance().GetAssetData(legacy_gif_name, ptr, size)) {
            LvglRawImage legacy_image(ptr, size);
            auto player = std::make_unique<LvglGif>(legacy_image.image_dsc());
            if (player->IsLoaded()) {
                ESP_LOGW(TAG, "MJPEG asset unavailable; using legacy GIF %s", legacy_gif_name);
                next_animation = std::move(player);
                loaded_animation_name = legacy_gif_name;
            }
        }
    }

    const LvglImage* image = nullptr;
    if (!next_animation && emoji_collection != nullptr) {
        image = emoji_collection->GetEmojiImage(canonical);
    }
    if (!next_animation && image != nullptr && image->IsGif()) {
        auto player = std::make_unique<LvglGif>(image->image_dsc());
        if (player->IsLoaded()) {
            next_animation = std::move(player);
        }
    }

    if (!next_animation && image == nullptr) {
        ESP_LOGE(TAG, "No animation asset available for canonical emotion=%s", canonical);
        return;
    }

    if (request_generation != emotion_request_generation_.load(std::memory_order_acquire)) {
        ESP_LOGW(TAG, "Discard decoded stale animation=%s generation=%u current=%u",
                 canonical, static_cast<unsigned>(request_generation),
                 static_cast<unsigned>(emotion_request_generation_.load()));
        return;
    }

    DisplayLockGuard lock(this);
    const bool preview_visible = preview_frame_ != nullptr &&
                                 !lv_obj_has_flag(preview_frame_, LV_OBJ_FLAG_HIDDEN);
    if (is_terminal_mode_ || preview_visible) {
        deferred_emotion_ = canonical;
        ESP_LOGI(TAG, "Defer decoded emotion=%s terminal=%d preview=%d", canonical,
                 is_terminal_mode_, preview_visible);
        return;
    }

    if (next_animation) {
        pending_animation_ = std::move(next_animation);
        pending_animation_name_ = std::move(loaded_animation_name);
        pending_expression_ = canonical;
        pending_animation_generation_ = request_generation;
        pending_holographic_mode_ = next_holographic_mode;

        lv_anim_delete(emoji_image_, EmotionOpacityAnim);
        if (animation_controller_) {
            lv_obj_set_style_opa(emoji_image_, LV_OPA_COVER, 0);
            lv_anim_t fade_out;
            lv_anim_init(&fade_out);
            lv_anim_set_var(&fade_out, emoji_image_);
            lv_anim_set_values(&fade_out, LV_OPA_COVER, LV_OPA_TRANSP);
            lv_anim_set_time(&fade_out, 80);
            lv_anim_set_path_cb(&fade_out, lv_anim_path_ease_in);
            lv_anim_set_exec_cb(&fade_out, EmotionOpacityAnim);
            lv_anim_set_user_data(&fade_out, this);
            lv_anim_set_completed_cb(&fade_out, EmotionFadeOutCompleted);
            lv_anim_start(&fade_out);
        } else {
            CommitPendingAnimation();
        }
    } else {
        if (animation_controller_) {
            animation_controller_->Stop();
            animation_controller_.reset();
        }
        current_animation_name_.clear();
        SetHolographicMode(next_holographic_mode);
        lv_image_set_src(emoji_image_, image->image_dsc());
        lv_obj_set_style_opa(emoji_image_, LV_OPA_COVER, 0);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
    }

#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // In WeChat message style, if emotion is neutral, don't display it
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    if (strcmp(canonical, "neutral") == 0 && child_count > 0) {
        // Stop animation if running
        if (animation_controller_) {
            animation_controller_->Stop();
            animation_controller_.reset();
            current_animation_name_.clear();
        }
        
        lv_obj_add_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
#endif
}

void LcdDisplay::EmotionOpacityAnim(void* target, int32_t value) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(target), value, 0);
}

void LcdDisplay::EmotionFadeOutCompleted(lv_anim_t* animation) {
    auto* self = static_cast<LcdDisplay*>(animation->user_data);
    if (self != nullptr) {
        self->CommitPendingAnimation();
    }
}

void LcdDisplay::CommitPendingAnimation() {
    if (!pending_animation_) {
        if (emoji_image_ != nullptr) {
            lv_obj_set_style_opa(emoji_image_, LV_OPA_COVER, 0);
        }
        return;
    }
    const uint32_t current_generation =
        emotion_request_generation_.load(std::memory_order_acquire);
    if (pending_animation_generation_ != current_generation) {
        ESP_LOGW(TAG, "Drop stale pending animation=%s generation=%u current=%u",
                 pending_expression_.c_str(), static_cast<unsigned>(pending_animation_generation_),
                 static_cast<unsigned>(current_generation));
        pending_animation_.reset();
        pending_animation_name_.clear();
        pending_expression_.clear();
        if (emoji_image_ != nullptr) {
            lv_obj_set_style_opa(emoji_image_, LV_OPA_COVER, 0);
        }
        return;
    }

    if (animation_controller_) {
        animation_controller_->Stop();
        animation_controller_.reset();
    }
    animation_controller_ = std::move(pending_animation_);
    current_animation_name_ = std::move(pending_animation_name_);
    const bool freeze_last_frame = pending_expression_ == "sleepy";
    const std::string expression = std::move(pending_expression_);
    pending_animation_generation_ = 0;

    SetHolographicMode(pending_holographic_mode_);
    animation_controller_->SetLoopCount(freeze_last_frame ? 1 : 0);
    animation_controller_->SetFrameCallback([this]() {
        if (animation_controller_) {
            lv_image_set_src(emoji_image_, animation_controller_->image_dsc());
        }
    });
    lv_image_set_src(emoji_image_, animation_controller_->image_dsc());
    lv_obj_set_style_opa(emoji_image_, LV_OPA_TRANSP, 0);
    animation_controller_->Start();
    lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(emoji_image_, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t fade_in;
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, emoji_image_);
    lv_anim_set_values(&fade_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&fade_in, 120);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&fade_in, EmotionOpacityAnim);
    lv_anim_start(&fade_in);

    ESP_LOGI(TAG, "Animation switched expression=%s resource=%s generation=%u fade=80/120ms",
             expression.c_str(), current_animation_name_.c_str(),
             static_cast<unsigned>(current_generation));
}

void LcdDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    
    auto lvgl_theme = static_cast<LvglTheme*>(theme);
    
    // Get the active screen
    lv_obj_t* screen = lv_screen_active();

    // Set font
    auto text_font = lvgl_theme->text_font()->font();
    auto icon_font = lvgl_theme->icon_font()->font();
    auto large_icon_font = lvgl_theme->large_icon_font()->font();

    if (text_font->line_height >= 40) {
        lv_obj_set_style_text_font(mute_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, large_icon_font, 0);
        lv_obj_set_style_text_font(network_label_, large_icon_font, 0);
    } else {
        lv_obj_set_style_text_font(mute_label_, icon_font, 0);
        lv_obj_set_style_text_font(battery_label_, icon_font, 0);
        lv_obj_set_style_text_font(network_label_, icon_font, 0);
    }

    // Set parent text color
    lv_obj_set_style_text_font(screen, text_font, 0);
    lv_obj_set_style_text_color(screen, lvgl_theme->text_color(), 0);

    // Physical holographic reflection requires true black regardless of the
    // selected legacy light/dark theme.
    lv_obj_set_style_bg_image_src(container_, nullptr, 0);
    lv_obj_set_style_bg_color(container_, lv_color_hex(kHoloBlack), 0);

    // Update top bar (全息投影：纯黑不透明)
    if (top_bar_ != nullptr) {
        lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(top_bar_, lv_color_black(), 0);
    }

    // Update status bar elements (全息投影 → 纯白可见)
    lv_obj_set_style_text_color(network_label_, lv_color_hex(kHoloText), 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kHoloText), 0);
    lv_obj_set_style_text_color(notification_label_, lv_color_hex(kHoloText), 0);
    lv_obj_set_style_text_color(mute_label_, lv_color_hex(kHoloText), 0);
    lv_obj_set_style_text_color(battery_label_, lv_color_hex(kHoloText), 0);
    lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    // If we have the chat message style, update all message bubbles
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
    // Set content background opacity
    lv_obj_set_style_bg_opa(content_, LV_OPA_TRANSP, 0);

    // Iterate through all children of content (message containers or bubbles)
    uint32_t child_count = lv_obj_get_child_cnt(content_);
    for (uint32_t i = 0; i < child_count; i++) {
        lv_obj_t* obj = lv_obj_get_child(content_, i);
        if (obj == nullptr) continue;
        
        lv_obj_t* bubble = nullptr;
        
        // Check if this object is a container or bubble
        // If it's a container (user or system message), get its child as bubble
        // If it's a bubble (assistant message), use it directly
        if (lv_obj_get_child_cnt(obj) > 0) {
            // Might be a container, check if it's a user or system message container
            // User and system message containers are transparent
            lv_opa_t bg_opa = lv_obj_get_style_bg_opa(obj, LV_PART_MAIN);
            if (bg_opa == LV_OPA_TRANSP) {
                // This is a user or system message container
                bubble = lv_obj_get_child(obj, 0);
            } else {
                // This might be an assistant message bubble itself
                bubble = obj;
            }
        } else {
            // No child elements, might be other UI elements, skip
            continue;
        }
        
        if (bubble == nullptr) continue;
        
        // Use saved user data to identify bubble type
        void* bubble_type_ptr = lv_obj_get_user_data(bubble);
        if (bubble_type_ptr != nullptr) {
            const char* bubble_type = static_cast<const char*>(bubble_type_ptr);
            
            // Apply correct color based on bubble type
            if (strcmp(bubble_type, "user") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->user_bubble_color(), 0);
            } else if (strcmp(bubble_type, "assistant") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->assistant_bubble_color(), 0); 
            } else if (strcmp(bubble_type, "system") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            } else if (strcmp(bubble_type, "image") == 0) {
                lv_obj_set_style_bg_color(bubble, lvgl_theme->system_bubble_color(), 0);
            }
            
            // Update border color
            lv_obj_set_style_border_color(bubble, lvgl_theme->border_color(), 0);
            
            // Update text color for the message
            if (lv_obj_get_child_cnt(bubble) > 0) {
                lv_obj_t* text = lv_obj_get_child(bubble, 0);
                if (text != nullptr) {
                    // Set text color based on bubble type
                    if (strcmp(bubble_type, "system") == 0) {
                        lv_obj_set_style_text_color(text, lvgl_theme->system_text_color(), 0);
                    } else {
                        lv_obj_set_style_text_color(text, lvgl_theme->text_color(), 0);
                    }
                }
            }
        } else {
            ESP_LOGW(TAG, "child[%lu] Bubble type is not found", i);
        }
    }
#else
    // Simple UI mode - just update the main chat message
    if (chat_message_label_ != nullptr) {
        lv_obj_set_style_text_color(chat_message_label_, lv_color_hex(kHoloText), 0);
    }
    
    if (emoji_label_ != nullptr) {
        lv_obj_set_style_text_color(emoji_label_, lvgl_theme->text_color(), 0);
    }
    
    // Update bottom bar (全息投影：纯黑背景)
    if (bottom_bar_ != nullptr) {
        lv_obj_set_style_bg_color(bottom_bar_, lv_color_hex(kHoloBlack), 0);
        lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_70, 0);
        lv_obj_set_style_border_color(bottom_bar_, lv_color_hex(kHoloCyan), 0);
        lv_obj_set_style_border_opa(bottom_bar_, LV_OPA_40, 0);
    }
#endif
    
    // Update low battery popup
    lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0x240308), 0);
    lv_obj_set_style_border_color(low_battery_popup_, lv_color_hex(kHoloDanger), 0);

    // No errors occurred. Save theme to settings
    Display::SetTheme(lvgl_theme);
}

void LcdDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    hide_subtitle_ = hide;
    
    // Immediately update UI visibility based on the setting
    if (bottom_bar_ != nullptr) {
        if (hide) {
            lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            // Only show if there is actual content to display
            const char* text = (chat_message_label_ != nullptr) ? lv_label_get_text(chat_message_label_) : nullptr;
            if (text != nullptr && text[0] != '\0') {
                lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void LcdDisplay::SetStatus(const char* status) {
    if (!setup_ui_called_) {
        ESP_LOGW(TAG, "SetStatus('%s') called before SetupUI()", status ? status : "null");
        return;
    }

    std::string deferred_to_apply;
    {
    DisplayLockGuard lock(this);

    // 判断是否为启动阶段的状态
    bool is_boot_status = false;
    if (status != nullptr) {
        if (strcmp(status, Lang::Strings::INITIALIZING) == 0 ||
            strcmp(status, Lang::Strings::DETECTING_MODULE) == 0 ||
            strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0 ||
            strcmp(status, Lang::Strings::SCANNING_WIFI) == 0 ||
            strcmp(status, Lang::Strings::CHECKING_NEW_VERSION) == 0 ||
            strcmp(status, Lang::Strings::ACTIVATION) == 0 ||
            strcmp(status, Lang::Strings::LOADING_PROTOCOL) == 0 ||
            strcmp(status, Lang::Strings::CONNECTING) == 0) {
            is_boot_status = true;
        }
    }

    if (is_boot_status && is_booting_) {
        // 使用固定阶段进度，避免重复状态通知导致进度虚增。
        if (boot_progress_bar_ != nullptr) {
            if (strcmp(status, Lang::Strings::INITIALIZING) == 0) current_boot_progress_ = 8;
            else if (strcmp(status, Lang::Strings::DETECTING_MODULE) == 0) current_boot_progress_ = 18;
            else if (strcmp(status, Lang::Strings::REGISTERING_NETWORK) == 0) current_boot_progress_ = 32;
            else if (strcmp(status, Lang::Strings::SCANNING_WIFI) == 0) current_boot_progress_ = 44;
            else if (strcmp(status, Lang::Strings::CHECKING_NEW_VERSION) == 0) current_boot_progress_ = 58;
            else if (strcmp(status, Lang::Strings::ACTIVATION) == 0) current_boot_progress_ = 72;
            else if (strcmp(status, Lang::Strings::LOADING_PROTOCOL) == 0) current_boot_progress_ = 84;
            else if (strcmp(status, Lang::Strings::CONNECTING) == 0) current_boot_progress_ = 94;
            lv_bar_set_value(boot_progress_bar_, current_boot_progress_, LV_ANIM_ON);
        }
        TriggerBootParticleBurst();

        // ---- 启动阶段: 更新毛玻璃卡片中的文字 ----
        if (boot_status_label_ != nullptr) {
            // 文字切换时的纵向滑入动画
            lv_anim_t slide;
            lv_anim_init(&slide);
            lv_anim_set_var(&slide, boot_status_label_);
            lv_anim_set_values(&slide, 10, 0);
            lv_anim_set_time(&slide, 300);
            lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
            lv_anim_set_exec_cb(&slide, [](void* obj, int32_t val) {
                lv_obj_set_style_translate_y((lv_obj_t*)obj, val, 0);
            });
            lv_anim_start(&slide);

            // 透明度渐入效果
            lv_anim_t fade;
            lv_anim_init(&fade);
            lv_anim_set_var(&fade, boot_status_label_);
            lv_anim_set_values(&fade, LV_OPA_0, LV_OPA_COVER);
            lv_anim_set_time(&fade, 300);
            lv_anim_set_exec_cb(&fade, [](void* obj, int32_t val) {
                lv_obj_set_style_text_opa((lv_obj_t*)obj, val, 0);
            });
            lv_anim_start(&fade);

            // 恢复直接使用系统定义的状态字符串，避免由于字体库裁剪导致的乱码和显示不全
            lv_label_set_text(boot_status_label_, status);
        }
        // 同步更新底层的 status_label_ (以便 overlay 关闭后状态正确)
        if (status_label_ != nullptr) {
            lv_label_set_text(status_label_, status);
        }
    } else {
        if (is_booting_) {
            // ---- 启动完成: 进度条填满，淡出遮罩层，显示主 UI ----
            is_booting_ = false;
            SetHolographicLayerVisible(true);
            
            if (boot_overlay_ != nullptr) {
                if (boot_progress_bar_ != nullptr) {
                    lv_bar_set_value(boot_progress_bar_, 100, LV_ANIM_ON);
                }

                // 停止所有动画
                if (boot_spinner_ != nullptr) lv_anim_delete(boot_spinner_, nullptr);
                if (boot_core_ != nullptr) lv_anim_delete(boot_core_, nullptr);
                for (int i = 0; i < 4; i++) {
                    if (boot_particles_[i] != nullptr) lv_anim_delete(boot_particles_[i], nullptr);
                }

                // 遮罩淡出动画
                lv_anim_t fadeout;
                lv_anim_init(&fadeout);
                lv_anim_set_var(&fadeout, boot_overlay_);
                lv_anim_set_values(&fadeout, LV_OPA_COVER, LV_OPA_0);
                lv_anim_set_time(&fadeout, 600); // 稍微加长淡出时间显得更从容
                lv_anim_set_path_cb(&fadeout, lv_anim_path_ease_in);
                lv_anim_set_exec_cb(&fadeout, [](void* obj, int32_t val) {
                    lv_obj_set_style_opa((lv_obj_t*)obj, val, 0);
                });
                // 动画结束后隐藏 overlay
                lv_anim_set_completed_cb(&fadeout, [](lv_anim_t* anim) {
                    lv_obj_add_flag((lv_obj_t*)anim->var, LV_OBJ_FLAG_HIDDEN);
                });
                lv_anim_start(&fadeout);
            }
        }

        // 判断是否为终端数据面板状态 (配网、OTA)
        bool is_terminal_status = false;
        if (status != nullptr) {
            if (strcmp(status, Lang::Strings::WIFI_CONFIG_MODE) == 0 ||
                strcmp(status, Lang::Strings::UPGRADING) == 0) {
                is_terminal_status = true;
            }
        }

        if (is_terminal_status) {
            // ---- 配网/升级阶段: 显示赛博终端界面 ----
            SetHolographicLayerVisible(false);
            if (terminal_panel_ != nullptr) {
                lv_obj_remove_flag(terminal_panel_, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(terminal_label_, status);
                
                // 隐藏顶部和底部常规栏位，让出全屏空间
                if (top_bar_ != nullptr) lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
                if (bottom_bar_ != nullptr) lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
                
                if (!is_terminal_mode_) {
                    is_terminal_mode_ = true;
                    
                    // 1. 启动雷达扫描动画
                    if (radar_anim_ == nullptr) {
                        radar_anim_ = new lv_anim_t;
                        lv_anim_init(radar_anim_);
                        lv_anim_set_var(radar_anim_, radar_scanner_);
                        lv_anim_set_values(radar_anim_, 0, 360);
                        lv_anim_set_time(radar_anim_, 1500); // 1.5秒转一圈
                        lv_anim_set_repeat_count(radar_anim_, LV_ANIM_REPEAT_INFINITE);
                        lv_anim_set_exec_cb(radar_anim_, [](void* obj, int32_t val) {
                            lv_arc_set_rotation((lv_obj_t*)obj, val);
                        });
                        lv_anim_start(radar_anim_);
                    }
                    
                    // 2. 启动核心呼吸脉冲动画
                    lv_anim_t core_anim;
                    lv_anim_init(&core_anim);
                    lv_anim_set_var(&core_anim, radar_core_);
                    lv_anim_set_values(&core_anim, 8, 16); // 核心大小脉冲
                    lv_anim_set_time(&core_anim, 800);
                    lv_anim_set_playback_time(&core_anim, 800);
                    lv_anim_set_repeat_count(&core_anim, LV_ANIM_REPEAT_INFINITE);
                    lv_anim_set_path_cb(&core_anim, lv_anim_path_ease_in_out);
                    lv_anim_set_custom_exec_cb(&core_anim, [](lv_anim_t* anim, int32_t val) {
                        lv_obj_set_size((lv_obj_t*)anim->var, val, val);
                        lv_obj_align((lv_obj_t*)anim->var, LV_ALIGN_CENTER, 0, -50); // 修改：将动画运行时的对齐也改成 -50
                    });
                    lv_anim_start(&core_anim);
                    TriggerTerminalDataBurst();
                }
            }
            if (emoji_box_ != nullptr) {
                lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            if (status_label_ != nullptr) {
                lv_obj_add_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (notification_label_ != nullptr) {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            // ---- 正常运行阶段: 隐藏终端，恢复常规 UI ----
            if (is_terminal_mode_) {
                is_terminal_mode_ = false;
                // 清理所有终端动画
                if (radar_anim_ != nullptr) {
                    lv_anim_delete(radar_scanner_, nullptr);
                    delete radar_anim_;
                    radar_anim_ = nullptr;
                }
                if (radar_core_ != nullptr) lv_anim_delete(radar_core_, nullptr);
                for (int i = 0; i < 4; i++) {
                    if (terminal_particles_[i] != nullptr) lv_anim_delete(terminal_particles_[i], nullptr);
                }
                if (!deferred_emotion_.empty() &&
                    (preview_frame_ == nullptr || lv_obj_has_flag(preview_frame_, LV_OBJ_FLAG_HIDDEN))) {
                    deferred_to_apply = std::move(deferred_emotion_);
                    deferred_emotion_.clear();
                }
            }
            
            if (terminal_panel_ != nullptr) {
                lv_obj_add_flag(terminal_panel_, LV_OBJ_FLAG_HIDDEN);
            }
            if (top_bar_ != nullptr) {
                lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
            }
            if (emoji_box_ != nullptr) {
                lv_obj_remove_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
            }
            if (preview_frame_ == nullptr || lv_obj_has_flag(preview_frame_, LV_OBJ_FLAG_HIDDEN)) {
                SetHolographicLayerVisible(true);
            }
            if (bottom_bar_ != nullptr && !hide_subtitle_ && chat_message_label_ != nullptr) {
                const char* subtitle = lv_label_get_text(chat_message_label_);
                if (subtitle != nullptr && subtitle[0] != '\0') {
                    lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (status_label_ != nullptr) {
                const char* new_text = status ? status : "";
                // status_label_ 有匹配 top_bar_ 的背景色（LV_OPA_70, 0x001122），
                // lv_label_set_text 的清除-重绘不会露出屏幕底色
                const char* current_text = lv_label_get_text(status_label_);
                if (current_text == nullptr || strcmp(current_text, new_text) != 0) {
                    lv_label_set_text(status_label_, new_text);
                }
                lv_obj_remove_flag(status_label_, LV_OBJ_FLAG_HIDDEN);
            }
            if (notification_label_ != nullptr) {
                lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    last_status_update_time_ = std::chrono::system_clock::now();
    }

    if (!deferred_to_apply.empty()) {
        ESP_LOGI(TAG, "Apply deferred terminal emotion=%s", deferred_to_apply.c_str());
        SetEmotion(deferred_to_apply.c_str());
    }
}
