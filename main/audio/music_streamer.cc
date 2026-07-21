#include "music_streamer.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <cstring>

#define TAG "MusicStreamer"

static const int kDownloadChunkSize = 16384;
static const int kOpusFrameDurationMs = 60;
static const int kMaxSongSize = 8 * 1024 * 1024;  // 8MB 最大歌曲大小

MusicStreamer::MusicStreamer() {
    data_ready_sem_ = xSemaphoreCreateBinary();
}

MusicStreamer::~MusicStreamer() {
    StopStream();
    if (data_ready_sem_) {
        vSemaphoreDelete(data_ready_sem_);
    }
}

void MusicStreamer::StartStream(const std::string& url,
    std::function<bool(std::unique_ptr<AudioStreamPacket>)> push_callback,
    std::function<void()> ready_callback) {
    if (playing_) {
        ESP_LOGW(TAG, "Already playing, stop first");
        StopStream();
    }

    url_ = url;
    push_callback_ = std::move(push_callback);
    ready_callback_ = std::move(ready_callback);
    playing_ = true;
    download_done_ = false;
    play_pos_ = 0;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_.clear();
    }
    demuxer_.Reset();

    // 创建下载任务（优先级 8，快速下载）
    xTaskCreate([](void* arg) {
        MusicStreamer* streamer = static_cast<MusicStreamer*>(arg);
        streamer->DoDownload();
        vTaskDelete(NULL);
    }, "music_dl", 4096, this, 8, &download_task_handle_);

    ESP_LOGI(TAG, "Starting streaming download: %s", url_.c_str());
}

void MusicStreamer::StopStream() {
    playing_ = false;
    download_done_ = false;
    // 唤醒可能正在等待 semaphore 的 Play task
    if (data_ready_sem_) {
        xSemaphoreGive(data_ready_sem_);
    }
    ESP_LOGI(TAG, "Stopping...");
}

// ============================================================
// 下载任务：流式下载，逐块写入共享 buffer
// ============================================================

void MusicStreamer::DoDownload() {
    ESP_LOGI(TAG, "Download thread started");

    esp_http_client_config_t config = {};
    config.url = url_.c_str();
    config.method = HTTP_METHOD_GET;
    config.timeout_ms = 60000;
    config.buffer_size = kDownloadChunkSize;
    config.buffer_size_tx = kDownloadChunkSize;
    config.is_async = false;
    config.skip_cert_common_name_check = true;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        playing_ = false;
        download_done_ = true;
        xSemaphoreGive(data_ready_sem_);  // 唤醒 Play task 让其退出
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        playing_ = false;
        download_done_ = true;
        xSemaphoreGive(data_ready_sem_);
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Content-Length: %d", content_length);

    if (content_length > kMaxSongSize) {
        ESP_LOGE(TAG, "Song too large: %d bytes (max %d)", content_length, kMaxSongSize);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        playing_ = false;
        download_done_ = true;
        xSemaphoreGive(data_ready_sem_);
        return;
    }

    // 预留空间减少重分配
    if (content_length > 0) {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_.reserve(content_length);
    }

    uint8_t* chunk = (uint8_t*)malloc(kDownloadChunkSize);
    if (chunk == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate download chunk buffer");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        playing_ = false;
        download_done_ = true;
        xSemaphoreGive(data_ready_sem_);
        return;
    }

    int total_read = 0;
    bool play_started = false;

    while (playing_) {
        int read_len = esp_http_client_read(client, (char*)chunk, kDownloadChunkSize);
        if (read_len < 0) {
            ESP_LOGE(TAG, "HTTP read error: %d", read_len);
            break;
        }

        if (read_len > 0) {
            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                buffer_.insert(buffer_.end(), chunk, chunk + read_len);
            }
            total_read += read_len;
            xSemaphoreGive(data_ready_sem_);  // 通知 Play task 有新数据

            // 初始缓冲就绪：启动播放任务并通知上层
            if (!play_started && total_read >= kInitialBufferThreshold) {
                play_started = true;
                ESP_LOGI(TAG, "Initial buffer ready (%d bytes), starting playback", total_read);

                // 通知主任务切换到音乐播放状态
                if (ready_callback_) {
                    ready_callback_();
                }

                // 短暂延迟让主任务处理状态切换
                vTaskDelay(pdMS_TO_TICKS(100));

                // 启动播放任务
                xTaskCreate([](void* arg) {
                    MusicStreamer* s = static_cast<MusicStreamer*>(arg);
                    s->DoPlay();
                    vTaskDelete(NULL);
                }, "music_play", 4096, this, 5, &play_task_handle_);
            }
        }

        if (read_len == 0) {
            // HTTP EOF — 下载完成
            ESP_LOGI(TAG, "Download complete: %d bytes", total_read);
            break;
        }
    }

    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (!playing_) {
        ESP_LOGW(TAG, "Download cancelled");
        download_done_ = true;
        xSemaphoreGive(data_ready_sem_);
        return;
    }

    download_done_ = true;
    xSemaphoreGive(data_ready_sem_);  // 最后通知 Play task：下载已完成

    // 如果下载量太小没触发播放，在这里兜底启动
    if (!play_started && total_read > 0) {
        ESP_LOGI(TAG, "Small file (%d bytes), starting playback directly", total_read);
        if (ready_callback_) {
            ready_callback_();
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        xTaskCreate([](void* arg) {
            MusicStreamer* s = static_cast<MusicStreamer*>(arg);
            s->DoPlay();
            vTaskDelete(NULL);
        }, "music_play", 3072, this, 5, &play_task_handle_);
    }

    ESP_LOGI(TAG, "Download thread exiting");
}

// ============================================================
// 播放任务：从共享 buffer 解封装并播放
// ============================================================

void MusicStreamer::DoPlay() {
    ESP_LOGI(TAG, "Playback started (streaming mode)");

    demuxer_.Reset();
    demuxer_.OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t len) {
        if (!playing_ || !push_callback_) {
            return;
        }
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = kOpusFrameDurationMs;
        packet->timestamp = 0;
        packet->payload.assign(data, data + len);
        // 背压控制：队列满时等待
        while (playing_) {
            if (push_callback_(std::make_unique<AudioStreamPacket>(*packet))) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    });

    size_t total_processed = 0;

    while (playing_) {
        // 取当前可用数据
        size_t available;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            available = buffer_.size() - play_pos_;
        }

        if (available == 0) {
            if (download_done_) {
                ESP_LOGI(TAG, "Playback finished, total demuxed: %zu bytes", total_processed);
                break;
            }
            // 等待下载任务写入更多数据
            xSemaphoreTake(data_ready_sem_, pdMS_TO_TICKS(200));
            continue;
        }

        // 喂数据给 demuxer
        size_t processed;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            processed = demuxer_.Process(
                buffer_.data() + play_pos_,
                buffer_.size() - play_pos_
            );
        }

        if (processed == 0) {
            // Demuxer 需要更多数据才能输出完整帧
            if (download_done_) {
                ESP_LOGI(TAG, "Demuxer done, total: %zu bytes", total_processed);
                break;
            }
            // 等待更多数据
            xSemaphoreTake(data_ready_sem_, pdMS_TO_TICKS(200));
            continue;
        }

        play_pos_ += processed;
        total_processed += processed;

        // 定期清理已消费数据，防止 buffer 无限增长
        if (play_pos_ > 131072) {  // 128KB 已消费
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            buffer_.erase(buffer_.begin(), buffer_.begin() + play_pos_);
            play_pos_ = 0;
        }

        // 每处理一批帧让出 CPU
        if ((total_processed % (kDownloadChunkSize * 2)) == 0) {
            vTaskDelay(1);
        }
    }

    // 安全清理
    playing_ = false;
    download_done_ = false;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        buffer_.clear();
        buffer_.shrink_to_fit();
    }
    demuxer_.Reset();
    ESP_LOGI(TAG, "Playback thread exiting");
}
