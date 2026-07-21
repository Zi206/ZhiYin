#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "lvgl_display.h"
#include "lvgl_animation.h"
#include "gif/lvgl_gif.h"
#include "mjpeg/lvgl_mjpeg.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <font_emoji.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#define PREVIEW_IMAGE_DURATION_MS 5000


class LcdDisplay : public LvglDisplay {
protected:
    enum class HolographicMode : uint8_t {
        kIdle,
        kListening,
        kSpeaking,
        kMusic,
        kSleep,
    };

    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    
    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_frame_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglAnimation> animation_controller_ = nullptr;
    std::unique_ptr<LvglAnimation> pending_animation_ = nullptr;
    std::string current_animation_name_;
    std::string pending_animation_name_;
    std::string pending_expression_;
    std::string deferred_emotion_;
    std::atomic<uint32_t> emotion_request_generation_{0};
    uint32_t pending_animation_generation_ = 0;
    HolographicMode pending_holographic_mode_ = HolographicMode::kIdle;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;

    // Main scene holographic energy layer. Objects are created once and reused
    // across states to avoid heap fragmentation during conversations.
    lv_obj_t* holographic_layer_ = nullptr;
    lv_obj_t* holographic_energy_bars_[3] = {nullptr};
    HolographicMode holographic_mode_ = HolographicMode::kSleep;
    bool holographic_layer_visible_ = false;

    // 科幻配网/终端数据面板
    lv_obj_t* terminal_panel_ = nullptr;
    lv_obj_t* terminal_label_ = nullptr;
    
    // 全息信号侦测终端专属动画组件
    lv_obj_t* radar_bg_ = nullptr;           // 雷达背景环
    lv_obj_t* radar_scanner_ = nullptr;      // 旋转扫描指针
    lv_obj_t* radar_core_ = nullptr;         // 雷达中心脉冲
    lv_obj_t* terminal_particles_[4] = {nullptr}; // 状态变化时触发的数据流粒子
    lv_obj_t* brackets_[4] = {nullptr};      // 四角 HUD 定位框
    lv_anim_t* radar_anim_ = nullptr;        // 扫描动画句柄
    
    bool is_terminal_mode_ = false;          // 记录当前是否处于终端模式

    // 方案三: 环形 Spinner + 毛玻璃卡片启动界面
    lv_obj_t* boot_overlay_ = nullptr;       // 全屏遮罩层
    lv_obj_t* boot_spinner_ = nullptr;       // 环形加载圈 (lv_arc)
    lv_obj_t* boot_card_ = nullptr;          // 毛玻璃信息卡片
    lv_obj_t* boot_status_label_ = nullptr;  // 卡片内状态文字

    // 星际引擎特有组件
    lv_obj_t* boot_core_ = nullptr;          // 核心脉冲
    lv_obj_t* boot_progress_bar_ = nullptr;  // 微型进度条
    lv_obj_t* boot_particles_[4] = {nullptr};// 状态变化时触发的曲率喷射粒子
    int current_boot_progress_ = 0;          // 当前进度
    
    bool is_booting_ = true;                 // 启动阶段标记
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles

    void InitializeLcdThemes();
    void SetupHolographicLayer(lv_obj_t* screen);
    void SetHolographicMode(HolographicMode mode);
    void SetHolographicLayerVisible(bool visible);
    void StartHolographicAnimations();
    void StopHolographicAnimations();
    void TriggerBootParticleBurst();
    void TriggerTerminalDataBurst();
    static void HolographicEnergyAnim(void* target, int32_t value);
    static void EmotionOpacityAnim(void* target, int32_t value);
    static void EmotionFadeOutCompleted(lv_anim_t* animation);
    void CommitPendingAnimation();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width, int height);
    
public:
    ~LcdDisplay();
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetStatus(const char* status) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    
    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                  int width, int height, int offset_x, int offset_y,
                  bool mirror_x, bool mirror_y, bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel,
                   int width, int height, int offset_x, int offset_y,
                   bool mirror_x, bool mirror_y, bool swap_xy);
};

#endif // LCD_DISPLAY_H
