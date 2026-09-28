#pragma once
namespace tdeck {

// BLE 协议栈的起/停 —— 目前只为一件事存在:量它到底吃多少内部 DRAM。
//
// 为什么要单独量:NimBLE 的开销分两块,一块是编译期进 .bss/IRAM 的(开了
// CONFIG_BT_ENABLED 就一直占着,关不掉),一块是 nimble_port_init() 从堆上
// 申请的。只有后者能靠"用完就关"省回来,而网上的数字全是把两块混在一起报的。
// configure 在 nimble_port_init() 之后、host 任务启动【之前】被调用。
// ble_hs_cfg 只能在这个窗口里设:早了会被 ble_hs_init() 冲掉,
// 晚了可能赶不上 sync 事件(控制器同步只要几毫秒,是个真实的竞争)。
bool ble_probe_up(void (*configure)() = nullptr);
void ble_probe_down();
bool ble_probe_is_up();

}  // namespace tdeck
