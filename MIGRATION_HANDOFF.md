# ZhiYin-HBG-V1.0.2 Migration Handoff

> Give this file to a new Codex session and continue from “Next actions.” Do not re-implement changes that are already complete.

## 0. Latest Expression and Wi-Fi UTF-8 Work

The code, animation assets, build, merged image, and release package for the expression and Wi-Fi work are complete. The latest upstream branch is `fix/remove-subtitle-divider` at commit `e5d402f`.

### Expression Playback Coordinator

`main/expression_playback_coordinator.h/.cc` is the single entry point for device state, STT actions, LLM emotion, TTS, alerts, and one-second expiry checks. The public `Display::SetEmotion()` API and server protocol are unchanged.

- Every active session and cancellation increments a generation. Queued events from an old generation are logged and discarded.
- LLM emotion is cached first and applied when TTS starts. A valid emotion arriving during Speaking overrides the speaking default immediately.
- TTS stop clears semantic emotion and recomputes the expression from Listening or Idle.
- Priority is preview/terminal pause > Alert > explicit action > LLM semantic emotion > local hint > device-state default.
- MusicPlaying remains dancing instead of periodically returning to neutral.
- An explicit dance action lasts 12 seconds and then recomputes the current state.
- Sleepy plays once and freezes on the final frame until the next active session.
- Local sad/angry keywords are temporary hints until an LLM emotion arrives.
- Closing an alert restores the current state instead of forcing neutral.
- Removed flags include `g_force_dance`, `g_sleep_frozen`, `music_dance_ticks_`, and `dance_stop_timer_`.

| Resource | Input aliases |
|---|---|
| neutral | neutral, relaxed, confident |
| listening | listening |
| speaking | speaking, thinking |
| happy | happy, funny, laughing, delicious, winking, silly, cool, kissy, loving, excited |
| surprised | surprised, shocked, confused |
| sad | sad, crying, embarrassed |
| angry | angry |
| sleepy | sleep, sleepy |
| dancing | dancing, explicit dance action, MusicPlaying |

Unknown LLM emotions are logged once and fall back to Speaking or the device-state expression; they never select an error icon.

### Display Switching and Fixed Asset Anchors

`LcdDisplay` owns the animation resource name and atomic request generation. The first frame of a new player is prepared outside the LVGL lock; the generation is checked again before switching. The sequence is 80 ms fade-out, atomic replacement, and 120 ms fade-in. An alias change within the same resource does not rebuild the player. Only current and pending players are retained. Preview and terminal pages keep the latest request and apply it when they close.

`scripts/convert_dog_animations.py` uses edge/background color difference, the largest connected component, and one fixed affine transform per set. The nine assets are 240×320 at 20 FPS, JPEG quality 85, with target center `x=120`, foot baseline `y=288`, and safe area `x=12..228, y=16..304`. No per-frame auto-centering is used.

### Wi-Fi Provisioning UTF-8 Fix

The managed component is not edited. `78/esp-wifi-connect 3.1.4` is provided through `components/esp-wifi-connect` and selected with `override_path: ../components/esp-wifi-connect` in `main/idf_component.yml`.

- Incorrect mojibake literals were removed.
- Open and secured network badges use JavaScript ASCII Unicode escapes `\uD83D\uDD13` and `\uD83D\uDD12`.
- SSID and RSSI are separate text nodes. SSIDs are inserted through `textContent` and never through `innerHTML`.
- HTML responses use `text/html; charset=utf-8`.
- `/scan` and `/saved/list` return cJSON with `application/json; charset=utf-8` and `Cache-Control: no-store`.

## 1. Build and Flash Environment

- Project: `A:\Study\esp32\Project\01_Active\ZhiYin-HBG-V1.0.2`
- Target: ESP32-S3, 16 MB flash, 8 MB PSRAM, board `zhengchen-1.54tft-wifi`
- ESP-IDF: v5.5.4
- IDF path: `A:\esp-idf\.espressif\v5.5.4\esp-idf`
- Python: `C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe`
- Ninja: `C:\Espressif\tools\ninja\1.12.1\ninja.exe`
- Source and documentation use UTF-8. Temporary files must stay on drive A and be removed after use.

`build/CMakeCache.txt` must contain:

```text
CMAKE_HOME_DIRECTORY:INTERNAL=A:/Study/esp32/Project/01_Active/ZhiYin-HBG-V1.0.2
```

`build_224.bat`, `merge_224.bat`, and `flash_idf_maxbaud.ps1` resolve the project from their own directory. Do not run `set-target` again if it would overwrite the board's `sdkconfig`.

## 2. Completed Hardware and Firmware Changes

### GPIO39 Floating Dual-Edge Conversation Key

File: `main/boards/zhengchen-1.54tft-wifi/zhengchen-1.54tft-wifi.cc`

- GPIO39 is the latching capacitive conversation key.
- `Button(..., disable_pull=true)` disables both internal pull-up and pull-down.
- The same conversation handler is attached to `OnPressDown()` and `OnPressUp()`.
- `kConversationEdgeGuardMs = 50` filters rapid glitches.
- Idle starts listening; Listening/Speaking cancels; MusicPlaying preserves its toggle behavior; unsupported states are ignored.
- GPIO0/BOOT has only a 1.5-second long-press provisioning action. A short press has no application behavior, and holding GPIO0 low during reset still enters ROM download mode.
- GPIO10 is inert at the application layer. GPIO10 and GPIO39 do not directly change volume; voice commands control volume.

Do not restore a generic toggle handler or change the 50 ms guard without an explicit request.

### Late-Response Cancellation

`Application::CancelListening()` sets `ignore_conversation_response_`, stops the active request, and returns to Idle. Late STT, LLM, or TTS events cannot update the UI or re-enter Speaking. A new active session clears the flag; normal VAD stop does not set it.

### Conversation Light Strip

Two WS2812/WS2812B LEDs use 5 V power, common ground, GRB order, and GPIO18 data. The ESP-IDF `led_strip` RMT driver is used instead of direct GPIO writes. Idle, Connecting, Listening, Speaking, MusicPlaying, and AudioTesting keep both LEDs on at soft white `RGB(51, 51, 51)`. Starting, provisioning, activation, upgrade, and sleep turn them off; wake restores the current-state behavior.

### Holographic UI and Dog Assets

- The realistic dog and three Speaking/Music energy bars remain; the blue double platform and center line were removed.
- The pale line under the status bar, side light rails, and moving scan line were removed from the main dialog page. Preview framing and provisioning radar remain.
- The subtitle bar keeps a 70% black background and no cyan top border.
- Energy animations are created only for Speaking/Music; Idle, Listening, and Sleep do not create empty infinite animations.
- Boot keeps only segmented-ring rotation and core breathing; provisioning/upgrade pages keep radar scanning and core breathing.
- Preview is limited to 200×200, preserves aspect ratio, pauses dog/holographic animation, and resumes the current frame when closed.
- Nine 240×320 source GIFs are converted to indexed ZHMJ/MJPEG assets. `LvglMjpeg` uses two aligned RGB565 PSRAM buffers, a Core-1 decode task, and a 50 ms frame cadence.

## 3. Build and Flash Results

On 2026-07-22, commit `e5d402f` was built with ESP-IDF v5.5.4. The application image is 3,023,376 bytes (`0x2e2210`), with 27% of the smallest app partition free. The resource image is 4,026,767 bytes. The merged image is `build/xiaozhi_merged_full.bin` (12,415,375 bytes).

The verified five-partition offsets are:

```text
0x000000  build/bootloader/bootloader.bin
0x008000  build/partition_table/partition-table.bin
0x00D000  build/ota_data_initial.bin
0x020000  build/xiaozhi.bin
0x800000  build/generated_assets.bin
```

On 2026-07-22 the image was flashed through `USB-SERIAL CH340K (COM14)` at 1,152,000 baud. ESP32-S3 revision v0.2, 8 MB PSRAM, MAC `3c:dc:75:fe:2a:00` were identified. All five writes reported `Hash of data verified`, followed by an RTS hard reset. COM13 (J-Link) and COM18/COM19 (Bluetooth) were not used.

## 4. Board Acceptance Checklist

- Boot page shows only the segmented ring, breathing core, and finite status particles.
- Main dialog has no pale divider, blue side rails, scan line, platform, or center line.
- Speaking/Music show three smooth energy bars; Idle, Listening, and Sleep do not show a platform.
- Preview pauses and resumes the current dog animation. All nine MJPEG assets loop, freeze sleepy on the final frame, and time out explicit dance actions correctly.
- `LvglMjpeg` should reach approximately `FPS 20.0/20` with `errors=0` and decode time below 50 ms.
- GPIO39 high-to-low and low-to-high transitions each trigger one conversation action; glitches within 50 ms are suppressed.
- GPIO10 has no application action. BOOT short press is ignored; a 1.5-second runtime long press enters provisioning; holding BOOT during reset enters ROM download.
- Verify volume through voice commands, not physical volume keys.
- If LEDs flicker, check common ground, DIN direction, 3.3 V logic level, and add a 74AHCT level shifter plus an approximately 330 ohm series resistor if required.

## 5. Next Actions

1. Complete the 200-cycle expression-switch, FPS/PSRAM, Wi-Fi provisioning, and GPIO39 hardware acceptance tests.
2. Release BOOT before reset when validating application boot; `boot:0x10 (DOWNLOAD(USB/UART0))` means GPIO0 was still low at reset.
3. Record board-side results here. A clean build, hash verification, and a successful flash do not replace visual, audio, and key-input acceptance.
