#include "application.h"
#include "board.h"
#include "display.h"
#include "display/lcd_display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "mqtt_protocol.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "assets.h"
#include "settings.h"

#include <cstring>
#include <esp_log.h>
#include <cJSON.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"


Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) {
    return state_machine_.TransitionTo(state);
}

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    expression_coordinator_.SetRenderer([display](const char* expression) {
        display->SetEmotion(expression);
    });
    expression_coordinator_.OnDeviceState(GetDeviceState());
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
        // 用户停止说话时立即发 stop_listening，无需等 AFE VAD 超时
        // 但需保护进入 Listening 后的前 5 秒，防止麦克风热身/状态切换时 VAD 误触发
        if (!speaking && GetDeviceState() == kDeviceStateListening
                && listening_mode_ == kListeningModeAutoStop
                && listening_start_time_.load() > 0) {  // 时间戳已设置才继续
            int64_t now = esp_timer_get_time() / 1000;
            if (now - listening_start_time_ > 5000) {
                StopListening();
            }
        }
    };
    audio_service_.SetCallbacks(callbacks);

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();
        
        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                // 扫描中不代表网络断开，不应关闭音频通道
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "triangle_exclamation", Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "triangle_exclamation", Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS = 
        MAIN_EVENT_SCHEDULE |
        MAIN_EVENT_SEND_AUDIO |
        MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE |
        MAIN_EVENT_CLOCK_TICK |
        MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED |
        MAIN_EVENT_NETWORK_DISCONNECTED |
        MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING |
        MAIN_EVENT_STOP_LISTENING |
        MAIN_EVENT_CANCEL_LISTENING |
        MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_CANCEL_LISTENING) {
            HandleCancelListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
                // VAD 检测到静音，主动停止监听
                // 必须加 5 秒保护窗：刚进入 Listening 时 VAD 初始状态就是静音，
                // 不加保护的话会立即触发 StopListening（与 on_vad_change 回调保持一致）
                if (!audio_service_.IsVoiceDetected() && listening_mode_ == kListeningModeAutoStop
                        && listening_start_time_.load() > 0) {
                    int64_t now = esp_timer_get_time() / 1000;
                    if (now - listening_start_time_ > 5000) {
                        StopListening();
                    }
                }
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
            }

            if (GetDeviceState() == kDeviceStateIdle && clock_ticks_ == 65) {
                expression_coordinator_.OnExplicitAction(
                    ExpressionId::kSleepy, "idle_timeout",
                    expression_coordinator_.CurrentGeneration());
            }
            expression_coordinator_.Tick();

            // 歌曲下载失败/取消检测
            if (music_loading_ && !music_streamer_.IsPlaying()) {
                music_loading_ = false;
                if (GetDeviceState() != kDeviceStateMusicPlaying) {
                    display->SetStatus(Lang::Strings::STANDBY);
                    expression_coordinator_.OnDeviceState(GetDeviceState());
                }
            }

            // 音乐播放完成检测：播放自然结束 → 自动回 Idle
            if (GetDeviceState() == kDeviceStateMusicPlaying && !music_loading_ && !music_streamer_.IsPlaying()) {
                ESP_LOGI(TAG, "Music playback finished, returning to idle");
                SetDeviceState(kDeviceStateIdle);
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate([](void* arg) {
            Application* app = static_cast<Application*>(arg);
            app->ActivationTask();
            app->activation_task_handle_ = nullptr;
            vTaskDelete(NULL);
        }, "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    auto state = GetDeviceState();
    ESP_LOGI(TAG, "Network disconnected, current state: %d", (int)state);
    // 不在此处主动关闭音频通道，理由如下：
    // 1. 原版 xiaozhi-esp32 没有此逻辑，WiFi短暂断连不影响音频通道
    // 2. WiFi断连时lwIP会关闭TCP socket，WebSocket层会自然检测到断连
    //    并调用 OnAudioChannelClosed 完成清理
    // 3. WifiStation有自动重连机制（最多5次立即重试），若在TCP超时前
    //    重连成功，音频连接可能存活，用户聆听不中断
    // 4. 若WiFi确实长时间断开，WebSocket的OnAudioChannelClosed会自然触发

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });

    // 在初始化阶段预建立音频通道，之后按BOOT键可直接进入聆听，跳过"连接中"状态
    // 这从根本上避免了"连接中→网络波动→重新初始化"的Bug路径
    PreOpenAudioChannel();
}

void Application::PreOpenAudioChannel() {
    if (!protocol_) {
        ESP_LOGW(TAG, "Protocol not initialized, skip pre-open audio channel");
        return;
    }
    if (protocol_->IsAudioChannelOpened()) {
        return;
    }

    ESP_LOGI(TAG, "Pre-opening audio channel during initialization");
    auto protocol = protocol_;
    xTaskCreate([](void* arg) {
        auto protocol = static_cast<std::shared_ptr<Protocol>*>(arg);
        bool success = false;
        for (int i = 0; i < 3; ++i) {
            if ((*protocol)->OpenAudioChannel()) {
                ESP_LOGI(TAG, "Audio channel pre-opened successfully");
                success = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        // 记录通道预建成功时间戳
        Application::GetInstance().Schedule([success]() {
            if (success) {
                // 记录通道建立的时间戳，用于后续 BOOT 键判断通道是否稳定
                Application::GetInstance().audio_channel_opened_time_.store(
                    esp_timer_get_time() / 1000);  // 微秒转毫秒
            }
        });
        delete protocol;
        vTaskDelete(NULL);
    }, "pre_open_audio", 4096 * 2, new std::shared_ptr<Protocol>(protocol), 2, NULL);
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
            Schedule([display, message = std::string(buffer)]() {
                display->SetChatMessage("system", message.c_str());
            });
        });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    Schedule([this]() {
        expression_coordinator_.OnDeviceState(GetDeviceState());
    });
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2; // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return; // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_shared<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_shared<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_shared<MqttProtocol>();
    }

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        if (GetDeviceState() == kDeviceStateConnecting) {
            return; // Ignore error during connection, connection task handles retries and final failure
        }
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (GetDeviceState() == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    
    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    
    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    
    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (cJSON_IsString(type) &&
            (strcmp(type->valuestring, "tts") == 0 ||
             strcmp(type->valuestring, "stt") == 0 ||
             strcmp(type->valuestring, "llm") == 0) &&
            ShouldIgnoreConversationResponse()) {
            ESP_LOGW(TAG, "Ignore canceled conversation response: %s", type->valuestring);
            return;
        }

        const uint32_t response_generation = expression_coordinator_.CurrentGeneration();

        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this, response_generation]() {
                    if (ShouldIgnoreConversationResponse() ||
                        response_generation != expression_coordinator_.CurrentGeneration()) {
                        ESP_LOGW(TAG, "Ignore canceled tts start");
                        return;
                    }
                    aborted_ = false;
                    expression_coordinator_.OnTtsStarted(response_generation);
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this, response_generation]() {
                    if (response_generation != expression_coordinator_.CurrentGeneration()) {
                        ESP_LOGW(TAG, "Ignore stale tts stop");
                        return;
                    }
                    expression_coordinator_.OnTtsStopped(response_generation);
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring), response_generation]() {
                        if (ShouldIgnoreConversationResponse() ||
                            response_generation != expression_coordinator_.CurrentGeneration()) {
                            ESP_LOGW(TAG, "Ignore canceled tts sentence");
                            return;
                        }
                        if (GetDeviceState() != kDeviceStateMusicPlaying && !music_loading_) {
                            display->SetChatMessage("assistant", message.c_str());
                        }
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::string stt_text = text->valuestring;
                // 跳舞意图：匹配"跳舞/跳个舞/来跳舞/跳一支"
                // 排除非跳舞表达的"跳"：吓一跳/跳高/跳水/心跳/蹦极跳...
                bool want_dance = (stt_text.find("跳舞") != std::string::npos ||
                                   stt_text.find("跳个舞") != std::string::npos ||
                                   stt_text.find("跳支舞") != std::string::npos ||
                                   stt_text.find("来跳舞") != std::string::npos);
                // 睡觉/休息意图检测：提前冻结，避免 Speaking/Idle 状态切换覆盖睡眠动画
                bool want_sleep = (stt_text.find("睡觉") != std::string::npos || stt_text.find("休息") != std::string::npos ||
                                   stt_text.find("睡吧") != std::string::npos || stt_text.find("睡了") != std::string::npos ||
                                   stt_text.find("困了") != std::string::npos || stt_text.find("累了") != std::string::npos);
                // 被骂/指责意图检测：小狗做出悲伤动作
                bool want_sad = (stt_text.find("笨蛋") != std::string::npos || stt_text.find("傻瓜") != std::string::npos ||
                                 stt_text.find("白痴") != std::string::npos || stt_text.find("蠢货") != std::string::npos ||
                                 stt_text.find("真蠢") != std::string::npos || stt_text.find("太笨") != std::string::npos ||
                                 stt_text.find("废物") != std::string::npos || stt_text.find("没用") != std::string::npos ||
                                 stt_text.find("混蛋") != std::string::npos || stt_text.find("坏蛋") != std::string::npos ||
                                 stt_text.find("你真讨厌") != std::string::npos || stt_text.find("滚蛋") != std::string::npos ||
                                 stt_text.find("你不行") != std::string::npos || stt_text.find("太差") != std::string::npos ||
                                 stt_text.find("道歉") != std::string::npos || stt_text.find("对不起") != std::string::npos ||
                                 stt_text.find("认错") != std::string::npos || stt_text.find("错了") != std::string::npos);
                // 狗叫意图：小狗做出愤怒/狗叫动作（dog_angry.gif）
                bool want_angry = (stt_text.find("叫一声") != std::string::npos || stt_text.find("叫一下") != std::string::npos ||
                                   stt_text.find("叫个") != std::string::npos || stt_text.find("汪汪") != std::string::npos ||
                                   stt_text.find("狗叫") != std::string::npos || stt_text.find("叫两声") != std::string::npos ||
                                   stt_text.find("吼一声") != std::string::npos);
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring), want_dance,
                          want_sleep, want_sad, want_angry, response_generation]() {
                    if (ShouldIgnoreConversationResponse() ||
                        response_generation != expression_coordinator_.CurrentGeneration()) {
                        ESP_LOGW(TAG, "Ignore canceled stt update");
                        return;
                    }
                    if (GetDeviceState() != kDeviceStateMusicPlaying && !music_loading_) {
                        display->SetChatMessage("user", message.c_str());
                        if (want_dance) {
                            expression_coordinator_.OnExplicitAction(
                                ExpressionId::kDancing, "stt_dance", response_generation, 12000);
                        } else if (want_sleep) {
                            expression_coordinator_.OnExplicitAction(
                                ExpressionId::kSleepy, "stt_sleep", response_generation);
                        } else if (want_angry) {
                            expression_coordinator_.OnTemporaryHint(
                                ExpressionId::kAngry, "stt_bark", response_generation);
                        } else if (want_sad) {
                            expression_coordinator_.OnTemporaryHint(
                                ExpressionId::kSad, "stt_negative", response_generation);
                        }
                    }
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, emotion_str = std::string(emotion->valuestring), response_generation]() {
                    if (ShouldIgnoreConversationResponse() ||
                        response_generation != expression_coordinator_.CurrentGeneration()) {
                        ESP_LOGW(TAG, "Ignore canceled llm update");
                        return;
                    }
                    if (GetDeviceState() != kDeviceStateMusicPlaying && !music_loading_) {
                        expression_coordinator_.OnLlmEmotion(emotion_str, response_generation);
                    }
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });
    
    protocol_->Start();
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{{
        digit_sound{'0', Lang::Sounds::OGG_0},
        digit_sound{'1', Lang::Sounds::OGG_1}, 
        digit_sound{'2', Lang::Sounds::OGG_2},
        digit_sound{'3', Lang::Sounds::OGG_3},
        digit_sound{'4', Lang::Sounds::OGG_4},
        digit_sound{'5', Lang::Sounds::OGG_5},
        digit_sound{'6', Lang::Sounds::OGG_6},
        digit_sound{'7', Lang::Sounds::OGG_7},
        digit_sound{'8', Lang::Sounds::OGG_8},
        digit_sound{'9', Lang::Sounds::OGG_9}
    }};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
            [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetChatMessage("system", message);
    Schedule([this, raw_emotion = std::string(emotion ? emotion : "")]() {
        expression_coordinator_.ShowAlert(raw_emotion);
    });
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    Schedule([this]() {
        expression_coordinator_.DismissAlert();
    });
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetChatMessage("system", "");
    }
}

bool Application::ShouldIgnoreConversationResponse() const {
    return ignore_conversation_response_.load() && GetDeviceState() == kDeviceStateIdle;
}

void Application::ToggleChatState() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT);
}

void Application::StartListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING);
}

void Application::StopListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING);
}

void Application::CancelListening() {
    xEventGroupSetBits(event_group_, MAIN_EVENT_CANCEL_LISTENING);
}

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        // 激活期间不响应 BOOT 键，避免打断后台初始化任务
        // 之前的逻辑会强制切到 Idle，但 ActivationTask 仍在后台运行，
        // 两者竞争导致设备行为异常（高概率表现为重新初始化）
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ignore_conversation_response_.store(false);
        expression_coordinator_.BeginSession("toggle_chat");
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            // 进入连接状态前切换到性能模式，确保WebSocket建立期间WiFi稳定
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }

        // 快路径前检查：通道是否稳定存活了足够时间（至少2秒）
        // 配网刚完成时 WiFi 可能还不稳定，PreOpenAudioChannel 建立的通道容易被打断
        // 如果通道建立不足2秒就直接用，很可能在 SendStartListening 时断连
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t channel_age_ms = now_ms - audio_channel_opened_time_.load();
        if (channel_age_ms < 300) {
            ESP_LOGW(TAG, "Audio channel too young (%lld ms), rebuilding for stability", channel_age_ms);
            // 通道刚建立不久，关闭后走慢路径重建一个更稳定的连接
            protocol_->CloseAudioChannel();
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            SetDeviceState(kDeviceStateConnecting);
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }

        SetListeningMode(mode);
    } else if (state == kDeviceStateMusicPlaying) {
        // 音乐播放中，按 BOOT 键停止播放
        StopMusicStream();
        protocol_->CloseAudioChannel();
        SetDeviceState(kDeviceStateIdle);
    } else if (state == kDeviceStateSpeaking) {
        HandleCancelListeningEvent();
    } else if (state == kDeviceStateListening) {
        HandleCancelListeningEvent();
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol is null");
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (protocol_->IsAudioChannelOpened()) {
        SetListeningMode(mode);
        return;
    }

    struct OpenAudioArgs {
        std::shared_ptr<Protocol> protocol;
        ListeningMode mode;
    };

    auto args = new OpenAudioArgs{protocol_, mode};
    xTaskCreate([](void* arg) {
        auto args = static_cast<OpenAudioArgs*>(arg);
        auto protocol = args->protocol;
        auto mode = args->mode;
        delete args;

        bool success = false;
        for (int i = 0; i < 3; ++i) {
            if (protocol->OpenAudioChannel()) {
                success = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        Application::GetInstance().Schedule([protocol, mode, success]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateConnecting) {
                return;
            }
            if (!success) {
                app.SetDeviceState(kDeviceStateIdle);
                app.Alert(Lang::Strings::ERROR, Lang::Strings::SERVER_NOT_CONNECTED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
                app.audio_service_.EnableWakeWordDetection(true);
                return;
            }

            app.SetListeningMode(mode);
        });
        vTaskDelete(NULL);
    }, "open_audio", 4096 * 2, args, 5, NULL);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }
    
    ListeningMode mode = GetDefaultListeningMode();
    if (state == kDeviceStateIdle) {
        ignore_conversation_response_.store(false);
        expression_coordinator_.BeginSession("start_listening");
        if (!protocol_->IsAudioChannelOpened()) {
            // 进入连接状态前切换到性能模式，确保WebSocket建立期间WiFi稳定
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() {
                ContinueOpenAudioChannel(mode);
            });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        expression_coordinator_.BeginSession("interrupt_speaking");
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(mode);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();
    
    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleCancelListeningEvent() {
    auto state = GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateListening &&
        state != kDeviceStateSpeaking && state != kDeviceStateConnecting) {
        return;
    }

    ignore_conversation_response_.store(true);
    expression_coordinator_.CancelSession("cancel_listening");
    if (protocol_) {
        protocol_->SendStopListening();
    }
    AbortSpeaking(kAbortReasonNone);
    SetDeviceState(kDeviceStateIdle);
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        expression_coordinator_.BeginSession("wake_word");
        audio_service_.EncodeWakeWord();
        auto wake_word = audio_service_.GetLastWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update),
            // then continue with OpenAudioChannel which may block for ~1 second
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        expression_coordinator_.BeginSession("wake_word_interrupt");
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue());

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            // Re-enable wake word detection as it was stopped by the detection itself
            audio_service_.EnableWakeWordDetection(true);
        } else {
            // Play popup sound and start listening again
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol is null");
        SetDeviceState(kDeviceStateIdle);
        return;
    }

    if (protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
        // Encode and send the wake word data to the server
        while (auto packet = audio_service_.PopWakeWordPacket()) {
            protocol_->SendAudio(std::move(packet));
        }
        // Set the chat state to wake word detected
        protocol_->SendWakeWordDetected(wake_word);

        // Set flag to play popup sound after state changes to listening
        play_popup_on_listening_ = true;
        SetListeningMode(GetDefaultListeningMode());
#else
        play_popup_on_listening_ = true;
        SetListeningMode(GetDefaultListeningMode());
#endif
        return;
    }

    struct WakeWordArgs {
        std::shared_ptr<Protocol> protocol;
        std::string wake_word;
    };

    auto args = new WakeWordArgs{protocol_, wake_word};
    xTaskCreate([](void* arg) {
        auto args = static_cast<WakeWordArgs*>(arg);
        auto protocol = args->protocol;
        auto wake_word = args->wake_word;
        delete args;

        bool success = false;
        for (int i = 0; i < 3; ++i) {
            if (protocol->OpenAudioChannel()) {
                success = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        Application::GetInstance().Schedule([protocol, wake_word, success]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() != kDeviceStateConnecting) {
                return;
            }
            if (!success) {
                app.SetDeviceState(kDeviceStateIdle);
                app.Alert(Lang::Strings::ERROR, Lang::Strings::SERVER_NOT_CONNECTED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
                app.audio_service_.EnableWakeWordDetection(true);
                return;
            }

            ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
#if CONFIG_SEND_WAKE_WORD_DATA
            while (auto packet = app.audio_service_.PopWakeWordPacket()) {
                protocol->SendAudio(std::move(packet));
            }
            protocol->SendWakeWordDetected(wake_word);
            app.play_popup_on_listening_ = true;
            app.SetListeningMode(app.GetDefaultListeningMode());
#else
            app.play_popup_on_listening_ = true;
            app.SetListeningMode(app.GetDefaultListeningMode());
#endif
        });
        vTaskDelete(NULL);
    }, "wake_word", 4096 * 2, args, 5, NULL);
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    expression_coordinator_.OnDeviceState(new_state);

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            display->SetStatus(Lang::Strings::STANDBY);
            // 异步执行 GIF 加载和 LVGL 对象操作，避免阻塞音频管道
            Schedule([display]() {
                display->ClearChatMessages();
            });
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            listening_start_time_ = esp_timer_get_time() / 1000;  // ⚡ 第一行，VAD 最早感知
            display->SetStatus(Lang::Strings::LISTENING);

            // Make sure the audio processor is running
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning()) {
                // For auto mode, wait for playback queue to be empty before enabling voice processing
                // This prevents audio truncation when STOP arrives late due to network jitter
                if (listening_mode_ == kListeningModeAutoStop) {
                    audio_service_.WaitForPlaybackQueueEmpty();
                }
                
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }

#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
            // Enable wake word detection in listening mode (configured via Kconfig)
            audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
            // Disable wake word detection in listening mode
            audio_service_.EnableWakeWordDetection(false);
#endif
            
            // Play popup sound after ResetDecoder (in EnableVoiceProcessing) has been called
            if (play_popup_on_listening_) {
                play_popup_on_listening_ = false;
                audio_service_.PlaySound(Lang::Sounds::OGG_POPUP);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (listening_mode_ != kListeningModeRealtime) {
                audio_service_.EnableVoiceProcessing(false);
                // Only AFE wake word can be detected in speaking mode
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateMusicPlaying:
            display->SetStatus("Music Playing");
            display->ClearChatMessages();  // 清除 AI 说话的字幕
            audio_service_.EnableVoiceProcessing(true);   // 保持麦克风上行，用户说"停止播放"能到云端
            audio_service_.EnableWakeWordDetection(false); // 音乐播放时关闭唤醒词，避免误触发
            audio_service_.ResetDecoder();
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    ignore_conversation_response_.store(false);
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG, "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start(); // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER); // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::WakeWordInvoke(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    
    if (state == kDeviceStateIdle) {
        audio_service_.EncodeWakeWord();

        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, wake_word]() {
                ContinueWakeWordInvoke(wake_word);
            });
            return;
        }
        // Channel already opened, continue directly
        ContinueWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking) {
        Schedule([this]() {
            AbortSpeaking(kAbortReasonNone);
        });
    } else if (state == kDeviceStateListening) {   
        Schedule([this]() {
            if (protocol_) {
                protocol_->CloseAudioChannel();
            }
        });
    }
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

    // Now it is safe to enter sleep mode
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload = std::move(payload)]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    audio_service_.PlaySound(sound);
}

void Application::StartMusicStream(const std::string& url) {
    music_loading_ = true;  // 阻止歌曲加载期间的 emoji 切换
    Schedule([this, url]() {
        // 先显示加载中状态（初始64KB缓冲很快完成，随即切换到音乐播放）
        auto display = Board::GetInstance().GetDisplay();
        if (display) {
            display->SetStatus("歌曲加载中...");
            expression_coordinator_.OnDeviceState(GetDeviceState());
        }
        // 开始流式下载+播放（边下边播）
        music_streamer_.StartStream(url,
            [this](std::unique_ptr<AudioStreamPacket> packet) -> bool {
                return audio_service_.PushPacketToDecodeQueue(std::move(packet));
            },
            [this]() {
                // 初始缓冲就绪：切换到音乐播放状态（下载仍在后台继续）
                music_loading_ = false;
                Schedule([this]() {
                    SetDeviceState(kDeviceStateMusicPlaying);
                });
            });
    });
}

void Application::StopMusicStream() {
    music_loading_ = false;  // 清除加载标记
    // 直接停止，不需要 Schedule — 此函数总是在主循环中调用
    music_streamer_.StopStream();
    audio_service_.ResetDecoder();  // 清空解码+播放队列，立刻静音
}

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
