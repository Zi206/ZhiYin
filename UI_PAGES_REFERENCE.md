# 2.2.4 工程 UI 页面参考文档

## 2026-07-14 全息投影 UI 统一

- 240×320 ST7789 主界面保留写实小狗，新增常驻 LVGL 全息覆盖层：投影光轨、扫描线、双椭圆底座、能量核心和三段状态能量条。
- 全息层由 `SetEmotion()` 映射 Idle、Listening、Speaking、Music、Sleep；对象只创建一次，状态切换不重复分配。
- 启动页仅保留“单分段环＋核心呼吸”两组持续动画，四向粒子只在启动状态变化时短暂喷射。
- 配网/升级页仅保留雷达扫描和核心呼吸，删除全屏故障闪烁；数据粒子只在进入页面时触发一次。
- 通知使用有限的淡入—停留—淡出动画；低电量和高温统一为底部警告轨道并只脉冲两轮。
- 图片预览使用最大 200×200 内容区和青色取景框，显示期间暂停小狗 GIF 与主场景全息动画。
- 九个小狗 GIF 保持 240×320、文件名、情绪映射和时长，统一优化为 15 FPS；资源包由 7,728,295 字节降至 5,272,369 字节。

全息色板：纯黑 `#000000`、深青 `#00384A`、主青 `#00D4FF`、高光 `#7DF9FF`、辅助紫 `#8A2BE2`、警告橙 `#FF8A00`、故障红 `#FF4D5A`、正文 `#F0FCFF`。

## 已修改 ✅

| 页面 | 代码位置 | 说明 |
|------|----------|------|
| AI 对话主页面 | `main/display/lcd_display.cc` (`SetupUI`) | 写实小狗 GIF + 全息覆盖层 + 字幕 + 状态栏 |
| LCD 配网终端页面 | `main/display/lcd_display.cc` | `terminal_panel_` + 双动画雷达界面 |
| 初始化启动页面 | `main/display/lcd_display.cc` | `boot_overlay_` + 单环核心 + HUD 状态卡 |
| 配网热点 HTML | `scripts/sonic_wifi_config.html` | 浏览器配网页 |
| WiFi 配网 HTML | `managed_components/78__esp-wifi-connect/assets/wifi_configuration.html` | 热点配网页 |
| WiFi 配网完成 HTML | `managed_components/78__esp-wifi-connect/assets/wifi_configuration_done.html` | 配网成功页 |

---

## 页面与组件说明

### 1. 低电量弹窗
- **代码**: `main/display/lcd_display.cc:480-490` 和 `1003-1014`（两套 UI 路径）
- **触发**: 电量 < 20% 时自动显示
- **组件**: `low_battery_popup_`, `low_battery_label_`
- **文字**: `Lang::Strings::BATTERY_NEED_CHARGE`
- **背景色**: 深红黑 `#240308`，左右红色警告轨道
- **动画**: 首次出现脉冲两轮，随后静止
- **主题切换**: `lcd_display.cc:1650`

```cpp
// 位置：line 480-490（路径A）和 line 1003-1014（路径B）
low_battery_popup_ = lv_obj_create(screen);
lv_obj_set_style_bg_color(low_battery_popup_, lv_color_hex(0x240308), 0);
lv_obj_set_style_border_color(low_battery_popup_, lv_color_hex(0xFF4D5A), 0);
low_battery_label_ = lv_label_create(low_battery_popup_);
```

### 2. 通知弹窗
- **代码**: `main/display/lcd_display.cc:442-448`（SetupUI 创建） + `main/display/display.cc:27-32`（ShowNotification 调用）
- **触发**: `ShowNotification()` — 音量提示、WiFi 扫描、配网进度等
- **组件**: `notification_label_`
- **样式**: 深青黑底、青色左右边框、`#F0FCFF` 高对比文字
- **动画**: 统一在 `LvglDisplay::ShowNotification()` 内执行有限淡入—停留—淡出

```cpp
// 位置：line 442
notification_label_ = lv_label_create(status_bar_);
lv_obj_set_style_text_color(notification_label_, lv_color_hex(0xF0FCFF), 0);
```

### 3. 静音图标
- **代码**: `main/display/lcd_display.cc:418-421`（SetupUI 创建）
- **触发**: 音量状态变化（mute/unmute）
- **组件**: `mute_label_`
- **文字颜色**: `#F0FCFF`

```cpp
// 位置：line 418
mute_label_ = lv_label_create(right_icons);
lv_obj_set_style_text_color(mute_label_, lv_color_hex(0xF0FCFF), 0);
```

### 4. 激活码弹窗
- **代码**: `main/application.cc:706` (ShowActivationCode 函数)
- **触发**: OTA 激活时，服务器返回激活码
- **实现**: 通过 `Alert()` → `Display::ShowNotification()` 显示
- **注意**: 目前只是简单的 Alert 通知，不是独立的 LVGL 页面

```cpp
// application.cc:529
ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());

// application.cc:706
void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    // 通过 Alert() 显示
}
```

### 5. 图片预览
- **代码**: `main/display/lcd_display.cc:862-865`（SetupUI 创建） + `SetPreviewImage()` 函数
- **触发**: 收到图片消息时调用 `SetPreviewImage()`
- **组件**: `preview_image_`
- **位置**: 屏幕中央，最大内容 200×200，保持比例并带青色取景框
- **行为**: 预览期间停止小狗 GIF 和全息主场景动画，关闭后恢复

```cpp
// 位置：line 862
preview_frame_ = lv_obj_create(screen);
preview_image_ = lv_image_create(preview_frame_);
lv_obj_add_flag(preview_frame_, LV_OBJ_FLAG_HIDDEN);
```

### 6. 顶部状态栏 UI
- **代码**: `main/display/lcd_display.cc:388-428`（路径A）和 `868-915`（路径B）
- **组件**: `top_bar_`, `network_label_`, `battery_label_`, `status_label_`
- **背景**: 纯黑，底部增加 1px 深青分隔线
- **图标与状态文字**: `#F0FCFF`

### 7. 底部字幕栏
- **代码**: `main/display/lcd_display.cc:460-478`（路径A）和 `948-1016`（路径B）
- **组件**: `bottom_bar_`, `chat_message_label_`
- **背景**: 70% 纯黑，顶部 1px 低亮青色分隔线，正文 `#F0FCFF`

### 8. 微信消息气泡区 (CONFIG_USE_WECHAT_MESSAGE_STYLE)
- **代码**: `main/display/lcd_display.cc:460-478`（路径A）
- **组件**: `content_` + 聊天气泡
- **注意**: 你的板子可能没开启这个宏，确认 `sdkconfig` 中 `CONFIG_USE_WECHAT_MESSAGE_STYLE` 是否开启

### 9. Emoji/字体图标回退
- **代码**: `main/display/lcd_display.cc:496-497`
- **组件**: `emoji_label_`（当没有 GIF 时显示的 FontAwesome 图标）
- **触发**: 当表情无法匹配到狗 GIF 时

```cpp
// 位置：line 496
emoji_label_ = lv_label_create(screen);
lv_obj_center(emoji_label_);
// 文字颜色跟随 theme text_color
```

### 10. Boot 启动覆盖层（已修改 ✅）
- **代码**: `main/display/lcd_display.cc:1106-1271` + `1700-1760`（关闭动画）
- **组件**: `boot_overlay_`, `boot_spinner_`, `boot_card_`, `boot_status_label_`, `boot_progress_bar_`, `boot_core_`, `boot_particles_[4]`
- **持续动画预算**: 单分段环旋转 + 核心呼吸；粒子为状态触发的一次性动画
- **状态更新**: `UpdateBootStatus()` line 1700

---

## 附加 HTML 页面

| 页面 | 路径 | 说明 |
|------|------|------|
| WiFi 配网页面 | `managed_components/78__esp-wifi-connect/assets/wifi_configuration.html` | 热点配网主页 |
| WiFi 配网完成页 | `managed_components/78__esp-wifi-connect/assets/wifi_configuration_done.html` | 配网成功提示页 |

---

## 快速定位指南

| 要找什么 | 去哪个文件 | 关键搜索词 |
|----------|-----------|-----------|
| AI对话主UI | `lcd_display.cc:815-1016` | `SetupUI()` |
| 配网终端面板 | `lcd_display.cc:1017-1100` | `terminal_panel_` |
| 启动动画 | `lcd_display.cc:1106-1271` | `boot_overlay_` |
| 低电量弹窗 | `lcd_display.cc:480-490` | `low_battery_popup_` |
| 通知弹窗 | `lcd_display.cc:442-448` | `notification_label_` |
| 图片预览 | `lcd_display.cc:862-865` | `preview_image_` |
| GIF/表情显示 | `lcd_display.cc:1378-1504` | `SetEmotion()` |
| 状态文字更新 | `lcd_display.cc` | `SetStatus()` |
| 字幕更新 | `lcd_display.cc` | `SetChatMessage()` |
| 主题切换 | `lcd_display.cc:1506-1645` | `SetTheme()` |
| 激活码 | `application.cc:706` | `ShowActivationCode()` |
| 配网HTML | `sonic_wifi_config.html` | — |
