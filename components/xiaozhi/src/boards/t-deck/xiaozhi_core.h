// xiaozhi_core.h — 小智内核对 OS 的全部接口
//
// UI 层(main/apps/xiaozhi/)只认这个头文件:启动内核、按语音键、收状态和文本。
// 界面不该知道 Opus、WebSocket、AudioCodec 的存在,内核也不该知道 LVGL 的存在 ——
// 中间这道墙就是这里。
//
// 线程模型(重要):
//   · 除 start() 外,所有函数都是【线程安全】的,内部只是往 Application 的
//     EventGroup 置位,不阻塞,可以直接在 LVGL 任务/输入任务里调。
//   · 回调【在小智主任务里】被调用,并且调用时 LVGL 锁【已经被持有】——
//     回调里可以直接操作 lv_obj,不要再自己 lvgl_port_lock(那是递归锁,
//     再锁也不会死,但没必要),更不要在回调里做耗时的事。

#pragma once

#include <functional>
#include <string>

namespace xz {

// 和小智内核的 DeviceState 一一对应,但不把内核头文件泄漏给 UI 层
enum class State {
    Unknown, Starting, WifiConfiguring, Idle, Connecting,
    Listening, Speaking, Upgrading, Activating, AudioTesting, FatalError,
};

struct Callbacks {
    std::function<void(State)>                              on_state;
    std::function<void(const std::string&)>                 on_emotion;       // "happy" / "thinking" ...
    std::function<void(const std::string& role,
                       const std::string& text)>            on_chat;          // role: user / assistant / system
    std::function<void(const std::string&)>                 on_status;        // 状态行文案(已是中文)
    std::function<void(const std::string&, int duration_ms)> on_notification; // 一闪而过的提示
};

// 必须在 start() 之前设好,否则启动早期的几条状态会丢。
void set_callbacks(Callbacks cb);

// 启动内核:建一个后台任务跑 Application::Initialize() + Run()。
// 幂等 —— 重复调用只有第一次真的启动(ADR-004:小智常驻,不跟着 app 生命周期走)。
// 返回 false 表示任务没建起来(基本只会是内部 RAM 不够)。
bool start();
bool started();

State       state();
const char* state_text(State s);   // 中文状态名,给 UI 兜底显示用

// ── 用户动作。都是非阻塞的 ──
void toggle_chat();       // 语音键:空闲→开始对话,说话中→打断
void start_listening();
void stop_listening();
void abort_speaking();

}  // namespace xz
