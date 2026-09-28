// ble_probe.cc — 最小 NimBLE 起停,用来量内存
//
// 这里【只】做协议栈的 init/deinit,不广播、不配对、不连 AMS。
// 目的是拿到一个干净的数字:BLE 起来要占多少内部 DRAM。
// 真正的 AMS 客户端要在这之上再加 GAP 广播 + 绑定 + GATT 发现,
// 那部分还会再吃一些,但量级由这个数决定。

#include "ble_probe.h"

#include <sdkconfig.h>

#if !CONFIG_BT_NIMBLE_ENABLED
// BT 没编进来时给三个空壳 —— 探针是可选的,不能因为它让固件编不过。
namespace tdeck {
bool ble_probe_is_up() { return false; }
bool ble_probe_up(void (*)()) { return false; }
void ble_probe_down()  {}
}  // namespace tdeck
#else

#include <esp_log.h>
#include <esp_heap_caps.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

namespace tdeck {
namespace {
const char* TAG = "ble";
bool s_up = false;

void host_task(void*)
{
    nimble_port_run();              // 一直跑到 nimble_port_stop()
    nimble_port_freertos_deinit();
}
}  // namespace

bool ble_probe_is_up() { return s_up; }

bool ble_probe_up(void (*configure)())
{
    if (s_up) return true;
    uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败:%s(内部堆当时 %u,最大块 %u)",
                 esp_err_to_name(err), (unsigned)before,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return false;
    }
    // ble_hs_cfg 要在这里设 —— 见头文件的说明
    if (configure) configure();

    nimble_port_freertos_init(host_task);
    s_up = true;

    uint32_t after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "BLE 起来了:内部堆 %u → %u(吃掉 %d)",
             (unsigned)before, (unsigned)after, (int)before - (int)after);
    return true;
}

void ble_probe_down()
{
    if (!s_up) return;
    uint32_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGE(TAG, "nimble_port_stop 失败 rc=%d,内存要不回来了", rc);
    }
    s_up = false;
    ESP_LOGW(TAG, "BLE 关掉:内部堆 %u → %u(还回来 %d)",
             (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - (int)before);
}

}  // namespace tdeck

#endif  // CONFIG_BT_NIMBLE_ENABLED
