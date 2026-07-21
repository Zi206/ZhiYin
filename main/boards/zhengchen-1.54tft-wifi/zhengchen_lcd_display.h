#ifndef ZHENGCHEN_LCD_DISPLAY_H
#define ZHENGCHEN_LCD_DISPLAY_H

#include "display/lcd_display.h"
#include "lvgl_theme.h"
#include <esp_lvgl_port.h>

class ZHENGCHEN_LcdDisplay : public SpiLcdDisplay {
protected:
    lv_obj_t* high_temp_popup_ = nullptr;  // 高温警告弹窗
    lv_obj_t* high_temp_label_ = nullptr;  // 高温警告标签

public:
    // 继承构造函数
    using SpiLcdDisplay::SpiLcdDisplay;

    void SetupHighTempWarningPopup() {
        auto lvgl_theme = static_cast<LvglTheme*>(current_theme_);
        auto text_font = lvgl_theme->text_font()->font();
        // 创建高温警告弹窗
        high_temp_popup_ = lv_obj_create(lv_screen_active());  // 使用当前屏幕
        lv_obj_set_scrollbar_mode(high_temp_popup_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_size(high_temp_popup_, LV_HOR_RES * 0.9, text_font->line_height * 2);
        lv_obj_align(high_temp_popup_, LV_ALIGN_BOTTOM_MID, 0, -(text_font->line_height + 20));
        lv_obj_set_style_bg_color(high_temp_popup_, lv_color_hex(0x2A1200), 0);
        lv_obj_set_style_bg_opa(high_temp_popup_, LV_OPA_90, 0);
        lv_obj_set_style_radius(high_temp_popup_, 0, 0);
        lv_obj_set_style_border_width(high_temp_popup_, 2, 0);
        lv_obj_set_style_border_side(high_temp_popup_,
            static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT), 0);
        lv_obj_set_style_border_color(high_temp_popup_, lv_color_hex(0xFF8A00), 0);
        
        // 创建警告标签
        high_temp_label_ = lv_label_create(high_temp_popup_);
        lv_label_set_text(high_temp_label_, "警告：温度过高");
        lv_obj_set_style_text_color(high_temp_label_, lv_color_hex(0xFFF2DF), 0);
        lv_obj_center(high_temp_label_);
        
        // 默认隐藏
        lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
    }

    void UpdateHighTempWarning(float chip_temp, float threshold = 75.0f) {
        if (high_temp_popup_ == nullptr) {
            ESP_LOGW("ZHENGCHEN_LcdDisplay", "High temp popup not initialized!");
            return;
        }

        if (chip_temp >= threshold) {
            ShowHighTempWarning();
        } else {
            HideHighTempWarning();
        }
    }

    void ShowHighTempWarning() {
        if (high_temp_popup_ && lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
            lv_anim_delete(high_temp_popup_, nullptr);
            lv_anim_t pulse;
            lv_anim_init(&pulse);
            lv_anim_set_var(&pulse, high_temp_popup_);
            lv_anim_set_values(&pulse, LV_OPA_50, LV_OPA_COVER);
            lv_anim_set_time(&pulse, 220);
            lv_anim_set_playback_time(&pulse, 220);
            lv_anim_set_repeat_count(&pulse, 1);
            lv_anim_set_exec_cb(&pulse, [](void* object, int32_t opacity) {
                lv_obj_set_style_opa(static_cast<lv_obj_t*>(object), static_cast<lv_opa_t>(opacity), 0);
            });
            lv_anim_set_completed_cb(&pulse, [](lv_anim_t* anim) {
                lv_obj_set_style_opa(static_cast<lv_obj_t*>(anim->var), LV_OPA_COVER, 0);
            });
            lv_anim_start(&pulse);
        }
    }

    void HideHighTempWarning() {
        if (high_temp_popup_ && !lv_obj_has_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN)) {
            lv_anim_delete(high_temp_popup_, nullptr);
            lv_obj_add_flag(high_temp_popup_, LV_OBJ_FLAG_HIDDEN);
        }
    }
};



#endif // ZHENGCHEN_LCD_DISPLAY_H
