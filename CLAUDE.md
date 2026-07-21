# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`ZhiYin-HBG-V1.0.2` — ESP-IDF firmware for a holographic-projection AI dog toy. It is a **fork of `78/xiaozhi-esp32` (PROJECT_VER 2.2.4)** trimmed to a single board: `zhengchen-1.54tft-wifi` (ESP32-S3, 16 MB flash, 8 MB PSRAM, 240×320 ST7789 over SPI, ES8388 audio, Wi-Fi only).

Only `main/boards/common/` and `main/boards/zhengchen-1.54tft-wifi/` exist — every other board branch in `main/CMakeLists.txt` and `main/Kconfig.projbuild` is dead upstream code. Do not spend time on it, and do not "fix" it.

The Chinese design docs at the repo root (`01-项目总览与开发方案.md`, `02-嵌入式固件架构设计说明书.md`) and `MIGRATION_HANDOFF.md` are the product spec. `MIGRATION_HANDOFF.md` is the authoritative running log of intentional deviations from upstream — **read it before changing behavior**, since several apparent oddities (50 ms edge guard, GPIO39 with pulls disabled, 12 s dance hold) are deliberate and must not be reverted without the user asking.

## Build / merge / flash

The `.bat`/`.ps1` scripts hardcode a toolchain layout that is **not present on this machine** (`A:\esp-idf\.espressif\v5.5.4\esp-idf`, `C:\Espressif\tools\...`, ESP-IDF v5.5.4). Verify before running or promising a build; if IDF is installed elsewhere, edit the paths in the scripts rather than inventing new ones.

```bat
build_224.bat     :: idf.py build — deliberately does NOT run set-target (would clobber sdkconfig)
merge_224.bat     :: idf.py size, then esptool merge_bin -> build\xiaozhi_merged_full.bin
flash_merged.bat  :: prompts for COM port, writes the merged image at 0x0
```

```powershell
.\flash_idf_maxbaud.ps1 -Port COM14 -Baud 1152000   # auto-picks the sole USB-SERIAL CH340K if -Port omitted
```

Flash layout is five regions (`partitions/v2/16m.csv`): bootloader `0x0`, partition table `0x8000`, ota_data `0xd000`, app `0x20000`, assets `0x800000`. **Always flash all five** — writing `xiaozhi.bin` alone leaves stale assets and mismatched animations.

Identify the port by device description (`USB-SERIAL CH340K`), never by remembered COM number — JLink and Bluetooth virtual ports also enumerate.

There is no test suite. Verification is the on-device acceptance checklist in `MIGRATION_HANDOFF.md` §6 plus the serial `LvglMjpeg` FPS log; a clean build and a hash-verified flash are not acceptance.

## Architecture

`main.cc` → `Application::Initialize()` + `Run()`. Everything funnels through one main task:

- **Application** (`application.cc/.h`) — singleton, FreeRTOS event-group loop. Other tasks never touch state directly; they set `MAIN_EVENT_*` bits or call `Schedule()`. Device state lives in `DeviceStateMachine` (`device_state_machine.cc`), not in ad-hoc flags.
- **ExpressionPlaybackCoordinator** (`expression_playback_coordinator.cc/.h`) — **the single owner of which animation plays.** Every source (device state, STT action, LLM `emotion`, TTS start/stop, Alert, 1 Hz tick) enters here and is arbitrated by priority: preview/terminal page > Alert > explicit action > LLM semantic > local hint > device-state default. It carries a *generation* counter: `BeginSession()`/`CancelSession()` bump it, and late server responses from an old generation are dropped, which is how a cancelled conversation stops re-entering Speaking. Never write expressions from `application.cc` or the display layer directly — add a source to the coordinator instead.
- **Display** (`display/lcd_display.cc`, `display/lvgl_display/`) — LVGL 9.4 holographic UI. `UI_PAGES_REFERENCE.md` maps every page/popup to its file and line range; consult it before hunting through the 1700-line `lcd_display.cc`.
- **Animations** — nine dog states shipped as a custom **ZHMJ v1 indexed MJPEG** container (`main/assets/extra/dog_*.mjp`, 240×320, 20 FPS, JPEG q85), played by `display/lvgl_display/mjpeg/lvgl_mjpeg.cc` with a Core-1 background decode task and two PSRAM RGB565 buffers; LVGL only flips buffers. The GIF player (`gif/lvgl_gif.cc`) remains only as a fallback for app/assets partition version mismatch. Source GIFs in `main/assets/source_gif/` are offline masters — regenerate `.mjp` with `scripts/convert_dog_animations.py` (FFmpeg optical-flow interpolation, one fixed affine transform per set, alignment report written to `reports/expression_alignment/`), never hand-edit the containers.
- **Audio** (`audio/audio_service.cc`, codec in `audio/codecs/es8388_audio_codec.cc`) — capture/VAD/wake-word/playback; `music_streamer.cc` for MusicPlaying.
- **Protocols** (`protocols/`) — WebSocket or MQTT to the xiaozhi server; `mcp_server.cc` exposes device tools (volume, camera, etc.) to the LLM.
- **Board** (`main/boards/zhengchen-1.54tft-wifi/`) — pins in `config.h`, wiring in `zhengchen-1.54tft-wifi.cc`. GPIO39 is a latching capacitive dialog key (both edges trigger, `disable_pull=true`, 50 ms guard); GPIO0/BOOT long-press 1.5 s enters Wi-Fi provisioning; GPIO10 is inert; GPIO18 drives 2× WS2812 via the RMT `led_strip` driver (do not `gpio_set_level` it).

## Assets partition

`main/CMakeLists.txt` calls `scripts/build_default_assets.py` to pack wake-word models, fonts, `assets/common/*.ogg` and `assets/extra/*.mjp` into `build/generated_assets.bin` (8 MB partition). Adding or resizing a `.mjp` changes that image — the assets partition must be reflashed, not just the app.

## Conventions

- `sdkconfig` is **gitignored**; `sdkconfig.defaults` + `sdkconfig.defaults.esp32s3` are the source of truth. Changing target or regenerating sdkconfig loses local config — that is why `build_224.bat` skips `set-target`.
- `components/` and `managed_components/` are gitignored, but `main/idf_component.yml` pins `78/esp-wifi-connect` with `override_path: ../components/esp-wifi-connect`. The Wi-Fi provisioning page (UTF-8 fixes, cJSON responses) lives in that local override — editing `managed_components/` has no effect.
- All source is UTF-8; Chinese comments and strings are normal here. Keep them UTF-8 and never emit mojibake literals into HTML/C.
- `.clang-format` is present; match surrounding style.
- Root `*.docx` deliverables are generated by the `generate_*.py` / `modify_docx.py` helpers at the repo root.
