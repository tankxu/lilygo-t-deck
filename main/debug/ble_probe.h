#pragma once
namespace tdeck {

// BLE 协议栈的起/停 —— 目前只为一件事存在:量它到底吃多少内部 DRAM。
//
// 为什么要单独量:NimBLE 的开销分两块,一块是编译期进 .bss/IRAM 的(开了
// CONFIG_BT_ENABLED 就一直占着,关不掉),一块是 nimble_port_init() 从堆上
// 申请的。只有后者能靠"用完就关"省回来,而网上的数字全是把两块混在一起报的。
bool ble_probe_up();
void ble_probe_down();
bool ble_probe_is_up();

}  // namespace tdeck
