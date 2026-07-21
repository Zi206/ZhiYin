#ifndef MUSIC_STREAMER_H_
#define MUSIC_STREAMER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>

#include "protocol.h"
#include "audio/demuxer/ogg_demuxer.h"

/**
 * MusicStreamer - 音乐播放器（流式边下边播）
 *
 * 流程：
 * 1. 下载初始 64KB 数据到 PSRAM 缓冲区
 * 2. 立即启动播放任务，同时下载继续在后台进行
 * 3. 播放任务从缓冲区逐帧解封装，数据不足时等待下载任务补充
 * 4. 播放期间没有网络依赖（缓冲足够大），不会卡顿
 *
 * 同步：mutex 保护 buffer_，semaphore 通知新数据到达
 */
class MusicStreamer {
public:
    MusicStreamer();
    ~MusicStreamer();

    /**
     * 开始下载并播放音乐
     * @param url 文件服务器上的 .opus 文件 URL
     * @param push_callback 向解码队列推送音频包的回调
     * @param ready_callback 初始缓冲就绪、可以开始播放的回调
     */
    void StartStream(const std::string& url,
                     std::function<bool(std::unique_ptr<AudioStreamPacket>)> push_callback,
                     std::function<void()> ready_callback);

    /** 停止播放，释放缓冲区 */
    void StopStream();

    /** 是否正在播放 */
    bool IsPlaying() const { return playing_; }

private:
    void DoDownload();
    void DoPlay();

    TaskHandle_t download_task_handle_ = nullptr;
    TaskHandle_t play_task_handle_ = nullptr;
    std::atomic<bool> playing_{false};
    std::atomic<bool> download_done_{false};

    std::string url_;
    std::vector<uint8_t> buffer_;  // PSRAM: 共享下载缓冲
    std::mutex buffer_mutex_;      // 保护 buffer_ 读写
    SemaphoreHandle_t data_ready_sem_ = nullptr;  // 下载有新数据时通知播放任务
    size_t play_pos_ = 0;          // 播放任务已消费的位置（只在 Play task 中访问）

    std::function<bool(std::unique_ptr<AudioStreamPacket>)> push_callback_;
    std::function<void()> ready_callback_;
    OggDemuxer demuxer_;

    // 初始缓冲阈值：下载累积到此字节数后启动播放
    static constexpr int kInitialBufferThreshold = 65536;  // 64KB
};

#endif // MUSIC_STREAMER_H_
