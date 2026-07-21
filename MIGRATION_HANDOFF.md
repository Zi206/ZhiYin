# ZhiYin-HBG-V1.0.2 新对话迁移文档

> 将本文件内容提供给新的 Codex 对话后，从“下一步操作”继续。不要重新实现已经完成的修改。

## 0. 2026-07-20 表情同步与 Wi-Fi UTF-8 修复（最新、优先于下方历史记录）

本轮已完成代码、素材、构建、合并固件和发布包；首次烧录在连接阶段因设备未进入 ROM 下载模式停止，尚未擦除或写入。发布目录：

```text
A:\Study\esp32\Project\02_Releases\ZhiYin-HBG-V1.0.2\2026-07-20_expression-sync-wifi-utf8
```

### 表情播放协调器

新增 `main\expression_playback_coordinator.h/.cc`，`Application` 中所有设备状态、STT 动作、LLM emotion、TTS、Alert 和每秒过期检查均从主任务进入该协调器。`Display::SetEmotion()` 公共接口和服务器协议未修改。

- 每次主动新会话或取消会递增 generation；已经排队的旧 generation LLM/TTS/STT 请求会记录原因并丢弃。
- LLM 情绪先缓存，TTS start 时应用；Speaking 期间才到达的有效语义立即覆盖 speaking。TTS stop 清除语义并重新按 Listening/Idle 计算。
- 优先级：图片预览/终端页暂停显示 > Alert > 明确动作 > LLM 语义 > 本地临时提示 > 设备状态默认表情。
- MusicPlaying 持续 dancing，不再每 30 秒切回 neutral。
- 明确跳舞动作保留 12 秒，到期由协调器按当前状态重新计算；显示层不再拥有强制回 neutral 的定时器。
- sleepy 只播放一轮并停在末帧，下一次主动会话解除；本地 sad/angry 仅在 LLM 语义到达前临时提示。
- Alert 关闭后恢复当前状态，而不是固定写 neutral。
- 已删除 `g_force_dance`、`g_sleep_frozen`、`music_dance_ticks_`、`dance_stop_timer_` 及分散的应用层直接表情写入。

规范映射：

| 资源 | 输入别名 |
|---|---|
| neutral | neutral、relaxed、confident |
| listening | listening |
| speaking | speaking、thinking |
| happy | happy、funny、laughing、delicious、winking、silly、cool、kissy、loving、excited |
| surprised | surprised、shocked、confused |
| sad | sad、crying、embarrassed |
| angry | angry |
| sleepy | sleep、sleepy |
| dancing | dancing、明确跳舞动作、MusicPlaying |

未知 LLM 情绪只记录一次诊断并回退 Speaking/设备状态，不再切成错误图标。

### 显示切换与素材固定锚点

`LcdDisplay` 使用拥有所有权的动画资源名和原子请求 generation。新播放器在 LVGL 锁外完成首帧准备，切换前再次校验 generation；流程为旧动画 80 ms 淡出、原子替换、新动画 120 ms 淡入。同一资源别名不重建播放器，同时只保存 current 与 pending；图片预览或终端页期间只保存最后一个请求，退出页面后再应用。

`scripts\convert_dog_animations.py` 已增加边缘背景色差、最大连通区域和每套一次的固定仿射变换。九套资源均为 240×320、20 FPS、JPEG quality 85，目标中心 `x=120`、脚底 `y=288`、安全区 `x=12..228, y=16..304`。每套素材只使用一个变换，不逐帧居中；转换后总大小 2,494,449 bytes。对齐结果保存在：

```text
reports\expression_alignment\2026-07-20\alignment_manifest.json
reports\expression_alignment\2026-07-20\alignment_contact_sheet.png
```

九套素材中位中心为 119.5..120 px，中位脚底为 288..289 px，均满足不超过 3 px 的目标偏差；容器逐帧 JPEG 边界、尺寸、FPS 和 SHA-256 已通过脚本校验。

### Wi-Fi 配网页 UTF-8 修复

未修改 `managed_components`。组件 `78/esp-wifi-connect 3.1.4` 已机械复制到 `components\esp-wifi-connect`，并在 `main\idf_component.yml` 为该依赖声明：

```yaml
override_path: ../components/esp-wifi-connect
```

构建日志和 `build\project_description.json` 已确认实际组件目录是本地 override。错误字面量 `馃寪`、`馃敀` 已删除；开放/加密徽标分别使用 JS ASCII Unicode 转义 `\uD83D\uDD13`、`\uD83D\uDD12`，带中英文 `title`/`aria-label`。SSID/RSSI 与安全徽标为独立节点，扫描和已保存 SSID 均只经 `textContent` 写入页面及输入框。

`/scan`、`/saved/list` 已改用 cJSON，能正确转义 UTF-8 中文、引号和反斜杠。页面响应为 `text/html; charset=utf-8`，JSON 为 `application/json; charset=utf-8`，并设置 `Cache-Control: no-store`。

### 最新构建产物

ESP-IDF v5.5.4 构建退出码 0，本地 override 生效，应用分区检查通过：

```text
build\xiaozhi.bin                  3,023,536 bytes，分区剩余 1,105,232 bytes（27%）
build\generated_assets.bin         4,026,767 bytes
build\xiaozhi_merged_full.bin     12,415,375 bytes
```

`build\CMakeCache.txt` 的 `CMAKE_HOME_DIRECTORY` 仍是规范工程路径。发布包保存五分区、合并固件、flasher 参数、对齐报告、README 和 SHA-256 清单。

### 最新烧录状态与下一动作

2026-07-20 已按描述正确识别 `USB-SERIAL CH340K (COM14)`，以 1,152,000 波特率成功完整烧录五分区。目标为 ESP32-S3 rev0.2、8 MB PSRAM，MAC `3c:dc:75:fe:2a:00`；bootloader、应用、分区表、OTA data 和 generated assets 均完成写入并通过 esptool Hash 校验。

烧录命令：

```powershell
.\flash_idf_maxbaud.ps1 -Port COM14 -Baud 1152000
```

烧录后的首次串口检查显示 `boot:0x10 (DOWNLOAD(USB/UART0))`，表明当时 BOOT/GPIO0 在复位瞬间仍为低电平。固件无需重烧；完全松开 BOOT 后单独短按 RESET/EN，再读取启动日志。仍需进行 200 次切换、FPS/PSRAM、Android/iOS 配网页和 GPIO39 的板上验收；构建、Hash 校验和烧录成功不能替代这些硬件结果。

## 1. 工程与环境

- 工程目录：`A:\Study\esp32\Project\01_Active\ZhiYin-HBG-V1.0.2`
- 目标芯片：ESP32-S3，Flash 16 MB，PSRAM 8 MB
- 板型：`zhengchen-1.54tft-wifi`
- ESP-IDF：v5.5.4
- ESP-IDF 路径：`A:\esp-idf\.espressif\v5.5.4\esp-idf`
- 可用 Python：`C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe`
- 可用 Ninja：`A:\Espressif\tools\ninja\1.12.1\ninja.exe`
- 中文源码统一使用 UTF-8；临时文件无用后必须清理，尤其不要占用 C 盘。
- 工程目录当前不是 Git 仓库，不能依赖 `git diff/status` 判断改动。

`build\CMakeCache.txt` 已确认包含：

```text
CMAKE_HOME_DIRECTORY:INTERNAL=A:/Study/esp32/Project/01_Active/ZhiYin-HBG-V1.0.2
```

`build_224.bat`、`merge_224.bat` 和烧录脚本已经改为从自身目录定位工程，不再切换到旧项目路径。需要重新配置时仍应使用 ESP-IDF v5.5.4 的 PowerShell 配置脚本和明确的 Python 路径；确认缓存属于本工程后，增量编译可以直接使用 Ninja。

## 2. 已完成的功能修改

### GPIO39 浮空双边沿对话键与 GPIO0 BOOT 配网键

文件：`main\boards\zhengchen-1.54tft-wifi\zhengchen-1.54tft-wifi.cc`

- GPIO39（原音量减）现在是锁存式电容对话键，每次触摸会翻转并保持输出电平。
- GPIO39 通过 `Button(..., disable_pull=true)` 初始化为纯输入，ESP32-S3 内部上拉和下拉均关闭，电平完全由外部电容按键输出驱动。
- 同一个对话处理器同时注册到 `OnPressDown()` 和 `OnPressUp()`，高到低、低到高都能各触发一次。
- 防重复触发窗口为 50 ms：`kConversationEdgeGuardMs = 50`。
- 状态行为是显式分发：
  - Idle：`StartListening()`
  - Listening/Speaking：`CancelListening()` 并回到待机
  - MusicPlaying：保留原有 Toggle 行为
  - 其他状态忽略
- GPIO0 BOOT 仅注册 1.5 秒长按回调进入配网，短按无应用行为；复位时拉低 GPIO0 进入 ROM 下载的功能不受影响。
- GPIO10（原音量加）不再创建应用层 `Button`，也不触发对话或配网。
- GPIO10、GPIO39 均不再直接调节音量，音量改由语音控制。

不要把该逻辑改回通用 Toggle，也不要修改 50 ms，除非用户明确提出。

### 取消录音后屏蔽迟到回复

文件：`main\application.h`、`main\application.cc`

- 已增加 `CancelListening()` 和 `ignore_conversation_response_`。
- 用户通过 GPIO39 对话键取消后，会发送停止/中止请求并切到 Idle。
- 取消后迟到的 `stt`、`llm`、`tts` 不再更新 UI，也不能再次进入 Speaking。
- 下一次主动开始聆听时清除取消标志。
- VAD 自动停止仍使用普通 `StopListening()`，不会屏蔽正常回答。

### 小狗对话页面灯带

硬件约定：两颗 WS2812/WS2812B，5V 供电、共地，DIN 接 GPIO18，GRB 顺序。

文件：

- `main\boards\zhengchen-1.54tft-wifi\config.h`
- `main\boards\zhengchen-1.54tft-wifi\zhengchen-1.54tft-wifi.cc`
- `main\application.cc`

已实现：

- 使用 ESP-IDF `led_strip` RMT 驱动，不再对 GPIO18 直接输出高低电平。
- 参数为 GPIO18、2 颗灯、柔和白光 `RGB(51, 51, 51)`。
- `ConversationLightStrip` 通过板级 `GetLed()` 接入已有状态变化回调。
- Idle、Connecting、Listening、Speaking、MusicPlaying、AudioTesting：两颗灯常亮。
- Starting、WifiConfiguring、Activating、Upgrading 及其他状态：灯带关闭。
- 进入省电休眠时强制关闭，唤醒后按当前设备状态恢复。
- `application.cc` 中原来的 `WL_LED_PIN`、`gpio_reset_pin()`、`gpio_set_level()` 已删除，避免与 RMT 冲突。

### 全息投影 UI 与写实小狗资源优化

主要文件：

- `main\display\lcd_display.h`
- `main\display\lcd_display.cc`
- `main\display\lvgl_display\mjpeg\lvgl_mjpeg.h/.cc`
- `main\display\lvgl_display\lvgl_display.cc`
- `main\boards\zhengchen-1.54tft-wifi\zhengchen_lcd_display.h`
- `main\assets\source_gif\dog_*.gif`
- `main\assets\extra\dog_*.mjp`
- `scripts\convert_dog_animations.py`

已完成：

- 保留写实小狗和 Speaking/Music 状态下的三段能量条，删除脚下双层蓝色圆角底座与中央能量横线。
- 主对话页已删除状态栏下方的淡蓝横线、小狗两侧的蓝色竖向光轨和上下移动的横向扫描线；图片预览取景框与配网页雷达不受影响。
- 字幕栏 `bottom_bar_` 顶部 1px 青色分隔线已按用户要求删除（创建处与 `SetTheme()` 重新应用处都已删除），只保留 70% 纯黑背景以保证字幕可读；不要重新加回该边框。
- 能量动画只在 Speaking/Music 状态创建，Idle、Listening 和 Sleep 不启动空的无限动画。
- 启动页仅保留分段环旋转和核心呼吸两组持续动画，四个粒子只在启动状态变化时短暂喷射。
- 配网/升级页删除全屏故障闪烁，只保留雷达扫描和核心呼吸；粒子仅在进入页面时触发一次。
- 通知改为有限淡入—停留—淡出；低电量和高温统一为底部警告轨道并只脉冲两轮。
- 图片预览限制在最大 200×200，保持比例并增加青色取景框；预览期间暂停小狗动画和全息层，关闭后从当前进度恢复。
- 九个 240×320 GIF 作为离线源素材保存在 `assets\source_gif`，运行时资源改为自定义 ZHMJ v1 索引式 MJPEG。
- 转换脚本使用 FFmpeg 光流插帧到固定 20 FPS，再以 JPEG 4:2:0、质量 85 编码；原 RGB565 渲染器的 R+8%/B-8% 暖色滤镜已烘焙进资源。
- MJP 总大小为 2,791,350 字节，较 3,740,051 字节 GIF 减少约 25.4%；每个容器的尺寸、帧数、索引、JPEG 边界和逐帧解码均已在 PC 端校验。
- `LvglMjpeg` 使用两个 16 字节对齐的 240×320 RGB565 PSRAM 缓冲；Core 1、优先级 3 的后台任务提前解码下一帧，LVGL 只按 50 ms 节拍切换缓冲，不再在界面线程执行 LZW 解码。
- 当前板型的 LVGL SPI 绘制缓冲启用双缓冲，以便绘制和 80 MHz SPI DMA 传输重叠；旧 GIF 播放器保留用于应用/资源分区版本不一致时回退。
- 每 5 秒记录实际/目标 FPS、平均/最大解码耗时、丢帧、解码错误和 PSRAM 剩余；睡眠末帧定格、跳舞 12 秒回待机、情绪映射和防闪黑行为保持不变。

## 3. 编译状态

2026-07-17 在工作区整理后的规范路径中完成全量重建；2026-07-18 完成对话页装饰清理、GPIO10 对话键、BOOT 长按配网和 20 FPS MJPEG 动画播放器后完成构建。2026-07-20 又将对话键迁移至 GPIO39，并关闭 GPIO39 的内部上拉和下拉；该版本继续使用 ESP-IDF v5.5.4 构建：

```powershell
. 'C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1'
$python = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
& $python (Join-Path $env:IDF_PATH 'tools\idf.py') build
```

生成文件：

```text
A:\Study\esp32\Project\01_Active\ZhiYin-HBG-V1.0.2\build\xiaozhi.bin
大小：3,016,368 字节（0x2e06b0）
应用分区剩余：1,112,400 字节，约 27%

A:\Study\esp32\Project\01_Active\ZhiYin-HBG-V1.0.2\build\generated_assets.bin
大小：4,323,668 字节
资源分区大小：8 MB，剩余约 3.88 MB
```

构建和增量复核均以退出码 0 完成，没有编译、链接或分区尺寸错误。`build\CMakeCache.txt` 指向 `A:/Study/esp32/Project/01_Active/ZhiYin-HBG-V1.0.2`。完整合并固件为 `build\xiaozhi_merged_full.bin`，大小 12,712,276 字节；GPIO39 浮空双边沿版本及 SHA-256 清单保存在 `02_Releases\ZhiYin-HBG-V1.0.2\2026-07-20_gpio39-floating-dialog`，发布文件已逐项与 build 哈希比对一致。

## 4. 烧录状态与阻塞点

2026-07-14 早些时候已成功烧录按键版本，目标为 ESP32-S3 rev0.2、16 MB Flash、8 MB PSRAM，MAC `3c:dc:75:fc:86:f8`。

2026-07-18 已通过 `USB-SERIAL CH340K (COM14)` 以 1,152,000 波特率将 `2026-07-18_clean-ui-gpio10-dialog` 完整五分区烧录到 ESP32-S3，所有写入均通过 Hash 校验并硬复位。

`2026-07-18_smooth-mjpeg-20fps` 的早期烧录尝试曾因板子未进入 ROM 下载模式而返回 `Invalid head of packet (0x49)` 和 `No serial data received`，失败均发生在擦除/写入之前。2026-07-20 已通过 `USB-SERIAL CH340K (COM14)` 以 1,152,000 波特率将 `2026-07-20_gpio39-floating-dialog` 完整五分区烧录到 ESP32-S3 rev0.2（MAC `3c:dc:75:fe:2a:00`），所有分区均通过 Hash 校验并完成硬复位。最近一次串口枚举包括：

- COM13：JLink CDC UART Port
- COM14：USB-SERIAL CH340K
- COM18、COM19：蓝牙虚拟串口

烧录时仍必须按设备描述识别 `USB-SERIAL CH340K`，不得依赖历史 COM 号或误用其他端口。下次必须烧录完整五分区，不能只写入 `xiaozhi.bin`。

## 5. 下一步操作

1. 连接并上电设备，重新枚举串口，按描述确认 `USB-SERIAL CH340K`。
2. 必要时按住 BOOT 后复位，使 ESP32-S3 进入 ROM 下载模式。
3. 使用最大波特率脚本烧录完整五分区：

```powershell
.\flash_idf_maxbaud.ps1 -Baud 1152000
```

脚本会自动选择唯一连接的 `USB-SERIAL CH340K`；如果同时连接多块板，再显式传入 `-Port <PORT>`。

4. 如果 1152000 不稳定，降至 921600；如果出现 `Invalid head of packet` 或 `No serial data received`，先重新进入下载模式，不要改固件。
5. 烧录成功后完成 UI、按键、灯带和语音联合验收。

串口枚举：

```powershell
@'
from serial.tools import list_ports
for p in list_ports.comports():
    print(f'{p.device}\t{p.description}\t{p.hwid}')
'@ | & 'C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe' -
```

## 6. 板上验收

- 启动页只显示单分段环、核心呼吸和状态触发粒子，不出现高负载连续粒子或全屏闪烁。
- 主界面保留写实小狗；状态栏下方无淡蓝横线，脚下无双层圆角底座和中央横线，画面两侧无蓝色竖框，主体区域无移动扫描线。
- Idle、Listening 和 Sleep 不显示脚底装饰；Speaking/Music 仍显示三根动态能量条，切换时无黑闪、白闪或残影。
- 配网/升级页面保持雷达扫描与核心呼吸，通知、低电量、高温和图片预览样式统一。
- 图片预览期间小狗动画和全息动画暂停，关闭预览后恢复；九个 MJP 均能正常循环、睡眠定格和跳舞回待机。
- 串口 `LvglMjpeg` 性能日志在稳定页面达到约 `FPS 20.0/20`、`errors=0`；平均/最大 JPEG 解码耗时低于 50 ms，不出现持续丢帧。
- 聆听、说话和音乐播放期间动画保持流畅，同时语音采集、播放和按键响应无卡顿、爆音或延迟恶化。
- 启动画面期间灯带关闭，进入小狗待机页面后两颗灯同时显示柔和白光。
- 待机、聆听、等待回复、说话和音乐状态切换时灯带持续常亮且不闪烁。
- 进入配网、升级或省电休眠时关闭，唤醒回到小狗页面后恢复。
- GPIO39 内部上拉、下拉均关闭；锁存输出无论从高变低还是从低变高，每次触摸都只触发一次对话操作，50 ms 内的快速毛刺受防重复保护。
- Idle 时触摸 GPIO39 进入聆听；Listening/Speaking 时再次触摸后保持待机，迟到的服务器回复不能重新进入 Speaking。
- GPIO10 不调音量、不触发对话或配网；BOOT 短按无应用行为，运行时长按 1.5 秒进入配网，复位时按住仍进入 ROM 下载模式。
- 通过语音命令验证音量仍能正常加减。
- 如果灯带颜色错乱或闪烁，先检查共地、DIN 方向和 3.3V 数据电平；必要时增加 74AHCT 电平转换器，并在 DIN 串联约 330Ω 电阻。

## 7. 给新对话的直接任务

请先阅读本迁移文档和上述源码。20 FPS MJPEG 播放器、对话页装饰清理、GPIO39 浮空双边沿对话键和 BOOT 长按配网均已实现并完成完整五分区烧录。下一步依据 `LvglMjpeg` 性能日志及 GPIO39 实际电平完成板上验收。
