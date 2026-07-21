#include "lvgl_gif.h"
#include <esp_log.h>
#include <cstring>

#define TAG "LvglGif"

LvglGif::LvglGif(const lv_img_dsc_t* img_dsc)
    : gif_(nullptr), timer_(nullptr), last_call_(0), playing_(false), loaded_(false),
      loop_delay_ms_(0), loop_waiting_(false), loop_wait_start_(0) {
    if (!img_dsc || !img_dsc->data) {
        ESP_LOGE(TAG, "Invalid image descriptor");
        return;
    }

    gif_ = gd_open_gif_data(img_dsc->data);
    if (!gif_) {
        ESP_LOGE(TAG, "Failed to open GIF from image descriptor");
        return;
    }

    // Setup LVGL image descriptor — RGB565 直接输出，无需软转换
    memset(&img_dsc_, 0, sizeof(img_dsc_));
    img_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    img_dsc_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    img_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    img_dsc_.header.w = gif_->width;
    img_dsc_.header.h = gif_->height;
    img_dsc_.header.stride = gif_->width * 2;
    img_dsc_.data = gif_->canvas;
    img_dsc_.data_size = gif_->width * gif_->height * 2;

    // Render first frame
    if (gif_->canvas) {
        gd_render_frame(gif_, gif_->canvas);
    }

    loaded_ = true;
    ESP_LOGD(TAG, "GIF loaded from image descriptor: %dx%d", gif_->width, gif_->height);
}

LvglGif::~LvglGif() { Cleanup(); }

const lv_img_dsc_t* LvglGif::image_dsc() const { return loaded_ ? &img_dsc_ : nullptr; }

void LvglGif::Start() {
    if (!loaded_ || !gif_) { ESP_LOGW(TAG, "GIF not loaded, cannot start"); return; }
    if (!timer_) {
        timer_ = lv_timer_create([](lv_timer_t* timer) {
            LvglGif* gif_obj = static_cast<LvglGif*>(lv_timer_get_user_data(timer));
            gif_obj->NextFrame();
        }, 10, this);
    }
    if (timer_) {
        playing_ = true; loop_waiting_ = false;
        last_call_ = lv_tick_get();
        lv_timer_resume(timer_); lv_timer_reset(timer_);
        NextFrame();
        ESP_LOGD(TAG, "GIF animation started");
    }
}

void LvglGif::Pause() { if (timer_) { playing_ = false; lv_timer_pause(timer_); } }

void LvglGif::Resume() {
    if (!loaded_ || !gif_) return;
    if (timer_) { playing_ = true; lv_timer_resume(timer_); }
}

void LvglGif::Stop() {
    if (timer_) { playing_ = false; lv_timer_pause(timer_); }
    loop_waiting_ = false;
    if (gif_) {
        gd_rewind(gif_);
        if (gif_->canvas) gd_render_frame(gif_, gif_->canvas);
        ESP_LOGD(TAG, "GIF animation stopped and rewound");
    }
}

bool LvglGif::IsPlaying() const { return playing_; }
bool LvglGif::IsLoaded() const { return loaded_; }
int32_t LvglGif::GetLoopCount() const { return gif_ ? gif_->loop_count : -1; }
void LvglGif::SetLoopCount(int32_t count) { if (gif_) gif_->loop_count = count; }
uint32_t LvglGif::GetLoopDelay() const { return loop_delay_ms_; }
void LvglGif::SetLoopDelay(uint32_t delay_ms) { loop_delay_ms_ = delay_ms; }
uint16_t LvglGif::width() const { return gif_ ? gif_->width : 0; }
uint16_t LvglGif::height() const { return gif_ ? gif_->height : 0; }
void LvglGif::SetFrameCallback(std::function<void()> callback) { frame_callback_ = callback; }

void LvglGif::NextFrame() {
    if (!loaded_ || !gif_ || !playing_) return;

    if (loop_waiting_) {
        if (lv_tick_elaps(loop_wait_start_) < loop_delay_ms_) return;
        loop_waiting_ = false;
    }

    if (lv_tick_elaps(last_call_) < gif_->gce.delay * 10) return;
    last_call_ = lv_tick_get();

    int has_next = gd_get_frame(gif_);
    if (has_next == 0) { playing_ = false; if (timer_) lv_timer_pause(timer_); return; }

    if (gif_->canvas) { gd_render_frame(gif_, gif_->canvas); if (frame_callback_) frame_callback_(); }
}

void LvglGif::Cleanup() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    if (gif_) { gd_close_gif(gif_); gif_ = nullptr; }
    playing_ = false; loaded_ = false;
    memset(&img_dsc_, 0, sizeof(img_dsc_));
}
