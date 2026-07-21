# 2.2.4 UI Page Reference

## 2026-07-14 Holographic UI Unification

- The 240×320 ST7789 home screen keeps the realistic dog and uses a persistent LVGL holographic overlay.
- `SetEmotion()` maps Idle, Listening, Speaking, Music, and Sleep states. UI objects are created once and are not reallocated on every state transition.
- The boot page keeps only the segmented-ring rotation and core breathing animations; four particles are emitted briefly when boot status changes.
- Provisioning and upgrade pages keep radar scanning and core breathing. Full-screen fault flashing was removed, and data particles run only when the page is entered.
- Notifications use finite fade-in, hold, and fade-out timing. Low-battery and high-temperature warnings share a bottom warning rail and pulse only twice.
- Image preview uses a content area up to 200×200 with a cyan viewfinder frame. Dog animation and the main holographic scene pause while preview is visible.
- Nine dog animations keep their 240×320 names, emotion mappings, and durations. Runtime assets use the optimized MJPEG/ZHMJ path documented in `MIGRATION_HANDOFF.md`.

Palette: pure black `#000000`, deep cyan `#00384A`, primary cyan `#00D4FF`, highlight `#7DF9FF`, secondary violet `#8A2BE2`, warning orange `#FF8A00`, fault red `#FF4D5A`, and body text `#F0FCFF`.

## Modified Pages

| Page | Code location | Description |
|------|----------|------|
| Main AI dialog page | `main/display/lcd_display.cc` (`SetupUI`) | Realistic dog animation, subtitles, state bar, and state energy bars |
| LCD provisioning terminal | `main/display/lcd_display.cc` | `terminal_panel_` and dual-animation radar interface |
| Boot page | `main/display/lcd_display.cc` | `boot_overlay_`, segmented core ring, and HUD status card |
| Sonic provisioning HTML | `scripts/sonic_wifi_config.html` | Browser provisioning page |
| Wi-Fi provisioning HTML | `components/esp-wifi-connect/assets/wifi_configuration.html` | Hotspot provisioning page |
| Wi-Fi completion HTML | `components/esp-wifi-connect/assets/wifi_configuration_done.html` | Provisioning-success page |

---

## Pages and Components

### 1. Low-Battery Popup

- **Code:** `main/display/lcd_display.cc` in both UI setup paths.
- **Trigger:** battery level below 20%.
- **Components:** `low_battery_popup_`, `low_battery_label_`.
- **Text:** `Lang::Strings::BATTERY_NEED_CHARGE`.
- **Style:** deep red-black `#240308` with red warning rails on both sides.
- **Animation:** two pulses on first appearance, then static.
- **Theme update:** handled by `SetTheme()`.

```cpp
low_battery_popup_ = lv_obj_create(screen);
lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0x240308), 0);
lv_obj_set_style_border_color(low_battery_popup_, lv_color_hex(0xFF4D5A), 0);
low_battery_label_ = lv_label_create(low_battery_popup_);
```

### 2. Notification Popup

- **Code:** created in `LcdDisplay::SetupUI()` and called through `Display::ShowNotification()`.
- **Trigger:** volume prompts, Wi-Fi scan status, provisioning progress, and similar notifications.
- **Component:** `notification_label_`.
- **Style:** deep cyan-black background, cyan side borders, and high-contrast `#F0FCFF` text.
- **Animation:** one finite fade-in, hold, and fade-out sequence in `LvglDisplay::ShowNotification()`.

### 3. Mute Icon

- **Code:** `main/display/lcd_display.cc` in `SetupUI()`.
- **Trigger:** mute/unmute state changes.
- **Component:** `mute_label_`.
- **Text color:** `#F0FCFF`.

### 4. Activation-Code Notification

- **Code:** `Application::ShowActivationCode()`.
- **Trigger:** OTA activation when the server returns an activation code.
- **Implementation:** displayed through `Alert()` and `Display::ShowNotification()` rather than a separate LVGL page.

### 5. Image Preview

- **Code:** `SetupUI()` and `SetPreviewImage()` in `main/display/lcd_display.cc`.
- **Trigger:** an incoming image message.
- **Components:** `preview_frame_`, `preview_image_`.
- **Layout:** centered, maximum 200×200 content area, preserved aspect ratio, and cyan viewfinder frame.
- **Behavior:** dog and holographic animation pause during preview and resume when it closes.

### 6. Top Status Bar

- **Components:** `top_bar_`, `network_label_`, `battery_label_`, and `status_label_`.
- **Background:** pure black. The obsolete pale-blue divider under the bar has been removed.
- **Icons and text:** `#F0FCFF`.

### 7. Bottom Subtitle Bar

- **Components:** `bottom_bar_`, `chat_message_label_`.
- **Style:** 70% opaque pure-black background, no border, and `#F0FCFF` body text.
- The former 1 px cyan top divider was removed both at creation and in `SetTheme()`.

### 8. WeChat-Style Message Area

- **Condition:** `CONFIG_USE_WECHAT_MESSAGE_STYLE`.
- **Components:** `content_` and chat bubbles.
- Verify the macro in `sdkconfig` before assuming this path is active on the target board.

### 9. Emoji/Icon Fallback

- **Component:** `emoji_label_`.
- **Trigger:** used only when no dog animation matches the requested emotion.
- **Text color:** follows the active theme.

### 10. Boot Overlay

- **Components:** `boot_overlay_`, `boot_spinner_`, `boot_card_`, `boot_status_label_`, `boot_progress_bar_`, `boot_core_`, and `boot_particles_[4]`.
- **Continuous-animation budget:** one segmented-ring rotation and one core-breathing animation.
- **Particles:** finite effects triggered only by a boot-status update.
- **Status updates:** handled by `UpdateBootStatus()`.

---

## Additional HTML Pages

| Page | Path | Description |
|------|------|------|
| Wi-Fi provisioning | `components/esp-wifi-connect/assets/wifi_configuration.html` | Hotspot provisioning home page |
| Provisioning complete | `components/esp-wifi-connect/assets/wifi_configuration_done.html` | Success prompt |

---

## Quick Location Guide

| Target | File | Search term |
|----------|-----------|-----------|
| Main dialog UI | `main/display/lcd_display.cc` | `SetupUI()` |
| Provisioning terminal | `main/display/lcd_display.cc` | `terminal_panel_` |
| Boot animation | `main/display/lcd_display.cc` | `boot_overlay_` |
| Low-battery popup | `main/display/lcd_display.cc` | `low_battery_popup_` |
| Notification popup | `main/display/lcd_display.cc` | `notification_label_` |
| Image preview | `main/display/lcd_display.cc` | `preview_image_` |
| Animation/emotion display | `main/display/lcd_display.cc` | `SetEmotion()` |
| Status text | `main/display/lcd_display.cc` | `SetStatus()` |
| Subtitle text | `main/display/lcd_display.cc` | `SetChatMessage()` |
| Theme updates | `main/display/lcd_display.cc` | `SetTheme()` |
| Activation code | `main/application.cc` | `ShowActivationCode()` |
| Provisioning HTML | `scripts/sonic_wifi_config.html` | — |
