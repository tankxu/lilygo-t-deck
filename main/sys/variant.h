// variant.h — 两个固件变体之间怎么来回切
//
// 双槽 OTA 原本是为了【回滚安全】(推新固件写另一个槽,推挂了退回来)。
// 这里多用了它一层:两个槽各装一个【变体】,切换 = 改 otadata + 重启,
// 不写 flash、不掉电、3 秒回来。
//
// 推固件的流程一点没变,而且天然是对的 —— 推送永远写"当前没在跑的那个槽",
// 也就是另一个变体所在的槽。所以要更新日常固件,得先切到车载模式再推,
// 反之亦然。这有点绕,但比腾出第三个槽(每个槽 6.375MB,16MB 塞不下三个)现实。
#pragma once
#include <stddef.h>

namespace tdeck {

// 当前跑的是哪个变体。取自 esp_app_desc_t.project_name:
//   "tdeck-os"     = daily
//   "tdeck-os-car" = car
const char* variant_name();
bool        variant_is_car();

// 另一个槽里装的是什么(写 project_name 进 out)。
// 那个槽是空的、或者里面不是个能认出来的镜像,返回 false。
bool variant_other_slot(char* out, size_t n);

// 切到另一个槽并重启。另一个槽不可用时返回 false 且【不重启】。
bool variant_switch_reboot();

}  // namespace tdeck
