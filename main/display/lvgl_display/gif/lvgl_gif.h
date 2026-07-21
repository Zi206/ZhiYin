#pragma once

#include "../lvgl_animation.h"
#include "gifdec.h"
#include <lvgl.h>
#include <memory>
#include <functional>

/**
 * C++ implementation of LVGL GIF widget
 * Provides GIF animation functionality using gifdec library
 */
class LvglGif final : public LvglAnimation {
public:
    explicit LvglGif(const lv_img_dsc_t* img_dsc);
    ~LvglGif() override;

    // LvglImage interface implementation
    const lv_img_dsc_t* image_dsc() const override;

    void Start() override;
    void Pause() override;
    void Resume() override;
    void Stop() override;

    bool IsPlaying() const override;
    bool IsLoaded() const override;

    int32_t GetLoopCount() const;
    void SetLoopCount(int32_t count) override;

    uint32_t GetLoopDelay() const;
    void SetLoopDelay(uint32_t delay_ms);

    uint16_t width() const;
    uint16_t height() const;

    void SetFrameCallback(std::function<void()> callback) override;

private:
    gd_GIF* gif_;
    lv_img_dsc_t img_dsc_;
    lv_timer_t* timer_;
    uint32_t last_call_;
    bool playing_;
    bool loaded_;

    uint32_t loop_delay_ms_;
    bool loop_waiting_;
    uint32_t loop_wait_start_;

    std::function<void()> frame_callback_;

    void NextFrame();
    void Cleanup();
};
