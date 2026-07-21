#include "lvgl_mjpeg.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <utility>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <esp_timer.h>
#include <sdkconfig.h>

namespace {

constexpr char kTag[] = "LvglMjpeg";
constexpr char kMagic[] = {'Z', 'H', 'M', 'J'};

bool TickReached(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace

LvglMjpeg::LvglMjpeg(const void* data, size_t size)
    : data_(static_cast<const uint8_t*>(data)), data_size_(size) {
    if (!ParseContainer()) {
        ESP_LOGE(kTag, "Invalid MJPEG animation container");
        return;
    }

    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    config.rotate = JPEG_ROTATE_0D;
    if (jpeg_dec_open(&config, &decoder_) != JPEG_ERR_OK || decoder_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create JPEG decoder");
        Cleanup();
        return;
    }

    frame_buffer_size_ = static_cast<size_t>(width_) * height_ * sizeof(uint16_t);
    for (auto& buffer : frame_buffers_) {
        buffer = static_cast<uint8_t*>(heap_caps_aligned_calloc(
            16, 1, frame_buffer_size_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buffer == nullptr) {
            ESP_LOGE(kTag, "Failed to allocate %u-byte PSRAM frame buffer",
                     static_cast<unsigned>(frame_buffer_size_));
            Cleanup();
            return;
        }
    }

    if (!DecodeFrame(0, frame_buffers_[0])) {
        ESP_LOGE(kTag, "Failed to decode first animation frame");
        Cleanup();
        return;
    }

    std::memset(&image_dsc_, 0, sizeof(image_dsc_));
    image_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc_.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;
    image_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
    image_dsc_.header.w = width_;
    image_dsc_.header.h = height_;
    image_dsc_.header.stride = width_ * sizeof(uint16_t);
    image_dsc_.data = frame_buffers_[0];
    image_dsc_.data_size = frame_buffer_size_;

    worker_done_ = xSemaphoreCreateBinary();
    if (worker_done_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create worker completion semaphore");
        Cleanup();
        return;
    }

    BaseType_t task_result;
#if CONFIG_FREERTOS_UNICORE
    task_result = xTaskCreate(
        [](void* arg) { static_cast<LvglMjpeg*>(arg)->WorkerLoop(); },
        "mjpeg_decode", 6144, this, 3, &worker_task_);
#else
    task_result = xTaskCreatePinnedToCore(
        [](void* arg) { static_cast<LvglMjpeg*>(arg)->WorkerLoop(); },
        "mjpeg_decode", 6144, this, 3, &worker_task_, 1);
#endif
    if (task_result != pdPASS) {
        worker_task_ = nullptr;
        ESP_LOGE(kTag, "Failed to create MJPEG decode task");
        Cleanup();
        return;
    }

    loaded_ = true;
    ESP_LOGI(kTag,
             "Animation ready: %ux%u, %u FPS, %u frames, buffers=%s/%s",
             width_, height_, fps_, static_cast<unsigned>(frame_count_),
             esp_ptr_external_ram(frame_buffers_[0]) ? "PSRAM" : "internal",
             esp_ptr_external_ram(frame_buffers_[1]) ? "PSRAM" : "internal");
}

LvglMjpeg::~LvglMjpeg() {
    Cleanup();
}

const lv_img_dsc_t* LvglMjpeg::image_dsc() const {
    return loaded_ ? &image_dsc_ : nullptr;
}

void LvglMjpeg::Start() {
    if (!loaded_) {
        return;
    }
    if (started_) {
        Resume();
        return;
    }

    if (timer_ == nullptr) {
        timer_ = lv_timer_create(
            [](lv_timer_t* timer) {
                static_cast<LvglMjpeg*>(lv_timer_get_user_data(timer))->OnTimer();
            },
            5, this);
    }
    if (timer_ == nullptr) {
        ESP_LOGE(kTag, "Failed to create animation timer");
        return;
    }

    started_ = true;
    playing_ = true;
    current_frame_ = 0;
    loops_completed_ = 0;
    front_buffer_ = 0;
    image_dsc_.data = frame_buffers_[front_buffer_];
    ready_buffer_.store(-1, std::memory_order_release);
    request_pending_.store(false, std::memory_order_release);
    deadline_missed_ = false;
    const uint32_t now = lv_tick_get();
    next_frame_tick_ = now + frame_period_ms_;
    last_stats_tick_ = now;
    stats_displayed_ = 0;
    stats_dropped_ = 0;
    QueueDecode(frame_count_ > 1 ? 1 : 0, 1 - front_buffer_);
    lv_timer_resume(timer_);
    lv_timer_reset(timer_);
}

void LvglMjpeg::Pause() {
    playing_ = false;
    if (timer_ != nullptr) {
        lv_timer_pause(timer_);
    }
}

void LvglMjpeg::Resume() {
    if (!loaded_ || !started_ || playing_) {
        return;
    }
    playing_ = true;
    deadline_missed_ = false;
    next_frame_tick_ = lv_tick_get() + frame_period_ms_;
    if (timer_ != nullptr) {
        lv_timer_resume(timer_);
        lv_timer_reset(timer_);
    }
}

void LvglMjpeg::Stop() {
    Pause();
}

bool LvglMjpeg::IsPlaying() const {
    return playing_;
}

bool LvglMjpeg::IsLoaded() const {
    return loaded_;
}

void LvglMjpeg::SetLoopCount(int32_t count) {
    loop_count_ = count;
}

void LvglMjpeg::SetFrameCallback(std::function<void()> callback) {
    frame_callback_ = std::move(callback);
}

bool LvglMjpeg::ParseContainer() {
    if (data_ == nullptr || data_size_ < kContainerHeaderSize ||
        std::memcmp(data_, kMagic, sizeof(kMagic)) != 0) {
        return false;
    }

    uint16_t version = 0;
    uint16_t header_size = 0;
    uint16_t reserved = 0;
    uint32_t flags = 0;
    if (!ReadU16(4, version) || !ReadU16(6, header_size) ||
        !ReadU16(8, width_) || !ReadU16(10, height_) ||
        !ReadU16(12, fps_) || !ReadU16(14, reserved) ||
        !ReadU32(16, frame_count_) || !ReadU32(20, index_offset_) ||
        !ReadU32(24, frame_data_offset_) || !ReadU32(28, flags)) {
        return false;
    }
    (void)reserved;
    (void)flags;

    if (version != kContainerVersion || header_size < kContainerHeaderSize ||
        width_ == 0 || height_ == 0 || fps_ == 0 || fps_ > 60 || frame_count_ == 0) {
        return false;
    }
    if (index_offset_ < header_size || frame_count_ > (UINT32_MAX / sizeof(uint32_t)) - 1) {
        return false;
    }
    const size_t index_end = static_cast<size_t>(index_offset_) +
                             static_cast<size_t>(frame_count_ + 1) * sizeof(uint32_t);
    if (index_end > data_size_ || frame_data_offset_ < index_end || frame_data_offset_ > data_size_) {
        return false;
    }

    uint32_t previous = 0;
    for (uint32_t i = 0; i <= frame_count_; ++i) {
        uint32_t offset = 0;
        if (!ReadU32(index_offset_ + static_cast<size_t>(i) * sizeof(uint32_t), offset) ||
            offset < previous || static_cast<size_t>(frame_data_offset_) + offset > data_size_) {
            return false;
        }
        previous = offset;
    }

    const uint8_t* first_frame = nullptr;
    size_t first_size = 0;
    if (!GetFrameSpan(0, first_frame, first_size) || first_size < 4 ||
        first_frame[0] != 0xff || first_frame[1] != 0xd8 ||
        first_frame[first_size - 2] != 0xff || first_frame[first_size - 1] != 0xd9) {
        return false;
    }

    frame_period_ms_ = std::max<uint32_t>(1, (1000u + fps_ / 2u) / fps_);
    return true;
}

bool LvglMjpeg::ReadU16(size_t offset, uint16_t& value) const {
    if (offset > data_size_ || data_size_ - offset < sizeof(value)) {
        return false;
    }
    std::memcpy(&value, data_ + offset, sizeof(value));
    return true;
}

bool LvglMjpeg::ReadU32(size_t offset, uint32_t& value) const {
    if (offset > data_size_ || data_size_ - offset < sizeof(value)) {
        return false;
    }
    std::memcpy(&value, data_ + offset, sizeof(value));
    return true;
}

bool LvglMjpeg::GetFrameSpan(uint32_t frame, const uint8_t*& ptr, size_t& size) const {
    if (frame >= frame_count_) {
        return false;
    }
    uint32_t begin = 0;
    uint32_t end = 0;
    if (!ReadU32(index_offset_ + static_cast<size_t>(frame) * sizeof(uint32_t), begin) ||
        !ReadU32(index_offset_ + static_cast<size_t>(frame + 1) * sizeof(uint32_t), end) ||
        end <= begin || static_cast<size_t>(frame_data_offset_) + end > data_size_) {
        return false;
    }
    ptr = data_ + frame_data_offset_ + begin;
    size = end - begin;
    return true;
}

bool LvglMjpeg::DecodeFrame(uint32_t frame, uint8_t* output) {
    const uint8_t* jpeg = nullptr;
    size_t jpeg_size = 0;
    if (decoder_ == nullptr || output == nullptr || !GetFrameSpan(frame, jpeg, jpeg_size) ||
        jpeg_size > INT_MAX) {
        return false;
    }

    jpeg_dec_io_t io = {};
    jpeg_dec_header_info_t info = {};
    io.inbuf = const_cast<uint8_t*>(jpeg);
    io.inbuf_len = static_cast<int>(jpeg_size);
    io.outbuf = output;

    if (jpeg_dec_parse_header(decoder_, &io, &info) != JPEG_ERR_OK ||
        info.width != width_ || info.height != height_) {
        return false;
    }
    int required_size = 0;
    if (jpeg_dec_get_outbuf_len(decoder_, &required_size) != JPEG_ERR_OK ||
        required_size != static_cast<int>(frame_buffer_size_)) {
        return false;
    }
    return jpeg_dec_process(decoder_, &io) == JPEG_ERR_OK;
}

bool LvglMjpeg::QueueDecode(uint32_t frame, int buffer) {
    if (worker_task_ == nullptr || buffer < 0 || buffer > 1 || frame >= frame_count_ ||
        shutdown_.load(std::memory_order_acquire)) {
        return false;
    }
    if (request_pending_.load(std::memory_order_acquire)) {
        return false;
    }
    requested_frame_.store(frame, std::memory_order_relaxed);
    requested_buffer_.store(buffer, std::memory_order_relaxed);
    request_pending_.store(true, std::memory_order_release);
    xTaskNotifyGive(worker_task_);
    return true;
}

void LvglMjpeg::WorkerLoop() {
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (shutdown_.load(std::memory_order_acquire)) {
            break;
        }
        if (!request_pending_.exchange(false, std::memory_order_acq_rel)) {
            continue;
        }

        const uint32_t frame = requested_frame_.load(std::memory_order_relaxed);
        const int buffer = requested_buffer_.load(std::memory_order_relaxed);
        const int64_t start = esp_timer_get_time();
        const bool decoded = DecodeFrame(frame, frame_buffers_[buffer]);
        const uint32_t decode_us = static_cast<uint32_t>(esp_timer_get_time() - start);

        stats_decode_us_.fetch_add(decode_us, std::memory_order_relaxed);
        stats_decode_count_.fetch_add(1, std::memory_order_relaxed);
        uint32_t previous_max = stats_decode_max_us_.load(std::memory_order_relaxed);
        while (decode_us > previous_max &&
               !stats_decode_max_us_.compare_exchange_weak(previous_max, decode_us,
                                                            std::memory_order_relaxed)) {
        }
        if (!decoded) {
            stats_decode_failures_.fetch_add(1, std::memory_order_relaxed);
            ESP_LOGE(kTag, "JPEG decode failed at frame %u", static_cast<unsigned>(frame));
        }

        ready_frame_.store(frame, std::memory_order_relaxed);
        ready_buffer_.store(buffer, std::memory_order_release);
    }

    xSemaphoreGive(worker_done_);
    vTaskDelete(nullptr);
}

void LvglMjpeg::OnTimer() {
    if (!playing_) {
        return;
    }
    const uint32_t now = lv_tick_get();
    if (!TickReached(now, next_frame_tick_)) {
        LogStats(now);
        return;
    }

    if (ready_buffer_.load(std::memory_order_acquire) < 0) {
        if (!deadline_missed_) {
            ++stats_dropped_;
            deadline_missed_ = true;
        }
        LogStats(now);
        return;
    }

    PublishReadyFrame(now);
    LogStats(now);
}

void LvglMjpeg::PublishReadyFrame(uint32_t now) {
    const int ready_buffer = ready_buffer_.exchange(-1, std::memory_order_acq_rel);
    if (ready_buffer < 0) {
        return;
    }
    const uint32_t ready_frame = ready_frame_.load(std::memory_order_relaxed);
    const int old_front = front_buffer_;
    front_buffer_ = ready_buffer;
    current_frame_ = ready_frame;
    image_dsc_.data = frame_buffers_[front_buffer_];
    ++stats_displayed_;
    deadline_missed_ = false;

    if (frame_callback_) {
        frame_callback_();
    }

    const uint32_t lateness = now - next_frame_tick_;
    next_frame_tick_ = lateness > frame_period_ms_
                           ? now + frame_period_ms_
                           : next_frame_tick_ + frame_period_ms_;

    uint32_t next_frame = current_frame_ + 1;
    if (next_frame >= frame_count_) {
        ++loops_completed_;
        if (loop_count_ > 0 && loops_completed_ >= static_cast<uint32_t>(loop_count_)) {
            playing_ = false;
            if (timer_ != nullptr) {
                lv_timer_pause(timer_);
            }
            return;
        }
        next_frame = 0;
    }
    QueueDecode(next_frame, old_front);
}

void LvglMjpeg::LogStats(uint32_t now) {
    const uint32_t elapsed = lv_tick_elaps(last_stats_tick_);
    if (elapsed < kStatsPeriodMs) {
        return;
    }

    const uint32_t decode_us = stats_decode_us_.exchange(0, std::memory_order_relaxed);
    const uint32_t decode_count = stats_decode_count_.exchange(0, std::memory_order_relaxed);
    const uint32_t decode_max_us = stats_decode_max_us_.exchange(0, std::memory_order_relaxed);
    const uint32_t decode_failures = stats_decode_failures_.exchange(0, std::memory_order_relaxed);
    const float actual_fps = elapsed > 0 ? stats_displayed_ * 1000.0f / elapsed : 0.0f;
    const float average_decode_ms = decode_count > 0 ? decode_us / (decode_count * 1000.0f) : 0.0f;

    ESP_LOGI(kTag,
             "FPS %.1f/%u, decode avg/max %.1f/%.1f ms, dropped=%u, errors=%u, PSRAM free/min=%u/%u KB",
             actual_fps, fps_, average_decode_ms, decode_max_us / 1000.0f,
             static_cast<unsigned>(stats_dropped_), static_cast<unsigned>(decode_failures),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024));

    stats_displayed_ = 0;
    stats_dropped_ = 0;
    last_stats_tick_ = now;
}

void LvglMjpeg::Cleanup() {
    if (timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    playing_ = false;
    loaded_ = false;

    if (worker_task_ != nullptr) {
        shutdown_.store(true, std::memory_order_release);
        xTaskNotifyGive(worker_task_);
        if (worker_done_ != nullptr && xSemaphoreTake(worker_done_, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGE(kTag, "Decode task did not stop in time; forcing task deletion");
            vTaskDelete(worker_task_);
        }
        worker_task_ = nullptr;
    }
    if (worker_done_ != nullptr) {
        vSemaphoreDelete(worker_done_);
        worker_done_ = nullptr;
    }
    if (decoder_ != nullptr) {
        jpeg_dec_close(decoder_);
        decoder_ = nullptr;
    }
    for (auto& buffer : frame_buffers_) {
        if (buffer != nullptr) {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
    }
    std::memset(&image_dsc_, 0, sizeof(image_dsc_));
}
