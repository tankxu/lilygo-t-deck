// ams_client.h — Apple Media Service 客户端
//
// AMS 是 iOS【自带】的服务,不需要伴侣 App、不需要开发者账号、不需要签名。
// 配件拿到的是:歌名/艺人/专辑/时长、播放状态和进度、shuffle/repeat、
// 当前播放 App 名,以及一套遥控命令(播放/暂停/上下一首/音量/……)。
// 拿不到的是封面、歌单、搜索 —— 那些才需要伴侣 App。
//
// ⚠️ 角色是反的,这是最容易想错的地方:
//   · GAP 上我们是 Peripheral —— 我们广播,iPhone 连过来
//   · GATT 上我们是 Client —— 服务在 iPhone 那边,我们去读它
// 所以 NimBLE 的 CONFIG_BT_NIMBLE_ROLE_CENTRAL 不能关(GATT Client 依赖它),
// 哪怕我们从不主动发起连接。
//
// ⚠️ 三个特征都要求【加密链路】才能访问,所以必须配对绑定 ——
// 板子会出现在 iPhone 的"设置→蓝牙"里。对车载配件来说这正好。
//
// 线程模型:所有回调都在 NimBLE 的 host 任务里跑,【不要在那里碰 LVGL】。
// 这里的做法是回调只写一份加锁的快照,UI 自己定时来取(snapshot())。
// 不用 lv_async_call —— LV_OS_NONE 下它不是线程安全的,这个项目为它崩过一次。

#pragma once

#include <stdbool.h>
#include <stdint.h>

namespace tdeck {
namespace ams {

// 连接状态。Ready 才代表 AMS 真的发现并订阅好了 ——
// Connected 只说明 BLE 连上了,那时候还什么数据都没有。
enum class Link {
    Off,           // 协议栈没起
    Starting,      // 协议栈起来了,还没开始广播(等控制器 sync)
    Advertising,   // 在广播,等 iPhone
    Connecting,    // 连上了,正在配对/加密
    Discovering,   // 加密好了,正在找 AMS
    Ready,         // 订阅完成,数据在流
    NoService,     // 连上也配对了,但对面没有 AMS(一般是还没解锁,或者系统不给)
};

// RemoteCommandID —— 值来自 Apple 的 AMS 规范,不能自己编
enum class Cmd : uint8_t {
    Play          = 0,
    Pause         = 1,
    TogglePlay    = 2,
    NextTrack     = 3,
    PrevTrack     = 4,
    VolumeUp      = 5,
    VolumeDown    = 6,
    AdvanceRepeat = 7,
    AdvanceShuffle= 8,
    SkipForward   = 9,
    SkipBackward  = 10,
    LikeTrack     = 11,
    DislikeTrack  = 12,
    BookmarkTrack = 13,
};

// PlaybackState 的取值(AMS 规范)
enum {
    PLAY_PAUSED  = 0,
    PLAY_PLAYING = 1,
    PLAY_REWIND  = 2,
    PLAY_FORWARD = 3,
};

struct Snapshot {
    Link  link = Link::Off;

    char  title[80]  = {};
    char  artist[80] = {};
    char  album[80]  = {};
    char  player[40] = {};      // 当前播放 App,"Music" / "Spotify" / 播客……

    float duration_s = 0;
    int   state      = PLAY_PAUSED;
    float rate       = 0;

    // 进度【不】高频同步。AMS 只在变化时推一次"那一刻的已播秒数",
    // 中间由本地按 rate 外推 —— 这也正是 AMS 自己的设计意图。
    // elapsed_now() 已经把外推做好了,UI 直接用那个。
    float   elapsed_at_report = 0;
    int64_t reported_at_us    = 0;

    int   queue_index = -1;
    int   queue_count = 0;
    int   shuffle     = -1;     // -1 未知
    int   repeat      = -1;

    // iPhone 会通知它【这一刻支持哪些命令】(比如播客没有 shuffle)。
    // 按位存,位号就是 RemoteCommandID。0 表示还没收到过。
    uint32_t supported = 0;
};

// 起协议栈 + 广播。已经在跑就直接返回 true。
bool start();
void stop();

// 取一份一致的快照(内部加锁拷贝)。UI 定时调这个,别自己存指针。
void snapshot(Snapshot* out);

// 按 rate 外推到"现在"的播放进度,秒。暂停时就是上报值本身。
float elapsed_now(const Snapshot& s);

// 发一条遥控命令。没连上、或者 iPhone 说不支持这条,返回 false。
bool send(Cmd c);

// 这条命令当前支不支持。还没收到支持列表时一律返回 true ——
// 灰掉一个其实能用的按钮,比让人以为坏了要糟。
bool supports(const Snapshot& s, Cmd c);

// 把已配对的设备忘掉(下次要重新配对)。设置里"忘记 iPhone"用。
void forget_bonds();

}  // namespace ams
}  // namespace tdeck
