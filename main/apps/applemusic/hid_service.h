// hid_service.h — BLE HID(纯 Consumer Control 媒体键)
//
// 存在的理由不是"多一种遥控方式",是【让 iPhone 认得这块板子】。
//
// iOS 的「设置 → 蓝牙」只列系统认识的配置文件(HID、音频那类)。普通 BLE
// 外设不管广播多标准都不会出现在那儿 —— 实测过:Mac 扫得到 T-Deck,
// CoreBluetooth 连我们征求的 UUID 都解析成了 "Apple Media",而 iPhone 的
// 设置里一片空白。所以在加 HID 之前,配对只能靠第三方 BLE 工具(LightBlue)。
//
// 加上 HID 之后:
//   · 板子出现在 设置 → 蓝牙,可以直接配对 —— 不需要任何 App
//   · 配对之后 iOS 会【主动重连】。这才是上车真正要的 ——
//     光有绑定不够,还得有人发起连接,而 iOS 不会主动连它不认识的外设。
//   · 附带:AMS 拿不到的时候(比如系统没发布它),媒体键仍然能用
//
// ⚠️ report map 里【只有】Consumer Control,没有键盘集合。
// 这是有意的:iPhone 一旦认定连着的是键盘,就会把屏幕上的软键盘收起来 ——
// 配过 BLE 键盘的人都知道这个副作用。抑制软键盘的判据是 report map 里
// 有没有键盘集合,不是 GAP 的 appearance,所以只报媒体键应该不触发。
// ⚠️ 这一条【还没实测】,配对之后要专门验一下软键盘还在不在。

#pragma once

#include <stdint.h>

namespace tdeck {
namespace hid {

// Consumer Control 的按键位。位号就是 report 里的比特位,
// 对应的 HID usage 见 hid_service.cc 的 report map。
enum class Key : uint8_t {
    PlayPause = 0,
    Next      = 1,
    Prev      = 2,
    VolumeUp  = 3,
    VolumeDown= 4,
    Mute      = 5,
    Stop      = 6,
};

// 把 HID / 设备信息 / 电量三个服务注册进 GATT 表。
// ⚠️ 必须在 ble_gatts_start() 之前调用 —— 也就是在 configure_host() 里,
// 跟 ble_svc_gap_init() 一批。晚了服务表已经定型,加不进去。
int register_services();

// 连接建立/断开时告诉它。通知要用连接句柄。
void set_conn(uint16_t conn_handle);   // BLE_HS_CONN_HANDLE_NONE 表示断开

// 按一下再松开。没连上、或者对方没订阅 report,返回 false。
bool tap(Key k);

// 对方有没有订阅我们的 HID report —— 也就是 iOS 是不是真把我们当 HID 在用。
bool subscribed();

// GAP 的 BLE_GAP_EVENT_SUBSCRIBE 转进来。订阅状态没有查询接口,
// NimBLE 只在事件里通知一次,只能自己记。
void on_gap_subscribe(uint16_t attr_handle, bool notify_enabled);

}  // namespace hid
}  // namespace tdeck
