#include "xz_display.h"
#include "application.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>

#define TAG "XzDisplay"

static xz::State to_xz(DeviceState s) {
    switch (s) {
    case kDeviceStateStarting:         return xz::State::Starting;
    case kDeviceStateWifiConfiguring:  return xz::State::WifiConfiguring;
    case kDeviceStateIdle:             return xz::State::Idle;
    case kDeviceStateConnecting:       return xz::State::Connecting;
    case kDeviceStateListening:        return xz::State::Listening;
    case kDeviceStateSpeaking:         return xz::State::Speaking;
    case kDeviceStateUpgrading:        return xz::State::Upgrading;
    case kDeviceStateActivating:       return xz::State::Activating;
    case kDeviceStateAudioTesting:     return xz::State::AudioTesting;
    case kDeviceStateFatalError:       return xz::State::FatalError;
    default:                           return xz::State::Unknown;
    }
}

XzDisplay::XzDisplay() {
    // 尺寸给内核看的。它只用来判断"是不是小屏"(见 Board::GetSystemInfoJson
    // 和 mcp_server 里 height() > 64 的判断),不参与任何绘制。
    width_  = 320;
    height_ = 240;
}

bool XzDisplay::Lock(int timeout_ms) {
    // 0 在 esp_lvgl_port 里是"永久等待",和 Display 基类的语义一致
    return lvgl_port_lock(timeout_ms);
}

void XzDisplay::Unlock() {
    lvgl_port_unlock();
}

void XzDisplay::EmitStateIfChanged() {
    auto s = to_xz(Application::GetInstance().GetDeviceState());
    if (s == last_state_) return;
    last_state_ = s;
    if (cb_.on_state) cb_.on_state(s);
}

void XzDisplay::SetStatus(const char* status) {
    DisplayLockGuard lock(this);
    EmitStateIfChanged();
    if (cb_.on_status && status) cb_.on_status(status);
}

void XzDisplay::ShowNotification(const char* notification, int duration_ms) {
    DisplayLockGuard lock(this);
    if (cb_.on_notification && notification) cb_.on_notification(notification, duration_ms);
}

void XzDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (cb_.on_emotion && emotion) cb_.on_emotion(emotion);
}

void XzDisplay::SetChatMessage(const char* role, const char* content) {
    DisplayLockGuard lock(this);
    if (cb_.on_chat && role && content) cb_.on_chat(role, content);
}

void XzDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (cb_.on_chat) cb_.on_chat("system", "");
}

void XzDisplay::UpdateStatusBar(bool update_all) {
    // 状态栏归 launcher 管(电量、WiFi、时间都是 OS 级的东西),
    // 小智每秒钟来敲这一下不做任何事。空实现,不要删 ——
    // 基类的默认实现也是空的,但显式写出来是为了说明"这是有意不做"。
    (void)update_all;
}
