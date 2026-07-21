#pragma once

#include <cstdint>
#include <functional>

#include <lvgl.h>

class LvglAnimation {
public:
    virtual ~LvglAnimation() = default;

    virtual const lv_img_dsc_t* image_dsc() const = 0;
    virtual void Start() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual void Stop() = 0;
    virtual bool IsPlaying() const = 0;
    virtual bool IsLoaded() const = 0;
    virtual void SetLoopCount(int32_t count) = 0;
    virtual void SetFrameCallback(std::function<void()> callback) = 0;
};
