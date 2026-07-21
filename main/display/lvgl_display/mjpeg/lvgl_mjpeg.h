#pragma once

#include "../lvgl_animation.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <esp_jpeg_dec.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class LvglMjpeg final : public LvglAnimation {
public:
    LvglMjpeg(const void* data, size_t size);
    ~LvglMjpeg() override;

    const lv_img_dsc_t* image_dsc() const override;
    void Start() override;
    void Pause() override;
    void Resume() override;
    void Stop() override;
    bool IsPlaying() const override;
    bool IsLoaded() const override;
    void SetLoopCount(int32_t count) override;
    void SetFrameCallback(std::function<void()> callback) override;

    uint16_t width() const { return width_; }
    uint16_t height() const { return height_; }
    uint16_t fps() const { return fps_; }
    uint32_t frame_count() const { return frame_count_; }

private:
    static constexpr size_t kContainerHeaderSize = 32;
    static constexpr uint16_t kContainerVersion = 1;
    static constexpr uint32_t kStatsPeriodMs = 5000;

    const uint8_t* data_ = nullptr;
    size_t data_size_ = 0;
    uint16_t width_ = 0;
    uint16_t height_ = 0;
    uint16_t fps_ = 0;
    uint32_t frame_count_ = 0;
    uint32_t index_offset_ = 0;
    uint32_t frame_data_offset_ = 0;
    size_t frame_buffer_size_ = 0;

    jpeg_dec_handle_t decoder_ = nullptr;
    uint8_t* frame_buffers_[2] = {nullptr, nullptr};
    lv_img_dsc_t image_dsc_ = {};
    lv_timer_t* timer_ = nullptr;
    TaskHandle_t worker_task_ = nullptr;
    SemaphoreHandle_t worker_done_ = nullptr;

    std::atomic<bool> shutdown_{false};
    std::atomic<bool> request_pending_{false};
    std::atomic<uint32_t> requested_frame_{0};
    std::atomic<int> requested_buffer_{-1};
    std::atomic<uint32_t> ready_frame_{0};
    std::atomic<int> ready_buffer_{-1};

    std::atomic<uint32_t> stats_decode_us_{0};
    std::atomic<uint32_t> stats_decode_count_{0};
    std::atomic<uint32_t> stats_decode_max_us_{0};
    std::atomic<uint32_t> stats_decode_failures_{0};
    uint32_t stats_displayed_ = 0;
    uint32_t stats_dropped_ = 0;
    uint32_t last_stats_tick_ = 0;

    int front_buffer_ = 0;
    uint32_t current_frame_ = 0;
    uint32_t loops_completed_ = 0;
    int32_t loop_count_ = 0;
    uint32_t frame_period_ms_ = 50;
    uint32_t next_frame_tick_ = 0;
    bool deadline_missed_ = false;
    bool playing_ = false;
    bool loaded_ = false;
    bool started_ = false;

    std::function<void()> frame_callback_;

    bool ParseContainer();
    bool ReadU16(size_t offset, uint16_t& value) const;
    bool ReadU32(size_t offset, uint32_t& value) const;
    bool GetFrameSpan(uint32_t frame, const uint8_t*& ptr, size_t& size) const;
    bool DecodeFrame(uint32_t frame, uint8_t* output);
    bool QueueDecode(uint32_t frame, int buffer);
    void WorkerLoop();
    void OnTimer();
    void PublishReadyFrame(uint32_t now);
    void LogStats(uint32_t now);
    void Cleanup();
};
