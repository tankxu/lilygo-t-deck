#include "variant.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <string.h>

namespace tdeck {
namespace {
const char* TAG = "variant";

const esp_partition_t* other_slot()
{
    // 「下一个可写的 OTA 槽」就是「当前没在跑的那个」—— 正是另一个变体所在处。
    return esp_ota_get_next_update_partition(nullptr);
}
}  // namespace

const char* variant_name()
{
    const esp_app_desc_t* d = esp_app_get_description();
    return d ? d->project_name : "?";
}

bool variant_is_car()
{
    return strcmp(variant_name(), "tdeck-os-car") == 0;
}

bool variant_other_slot(char* out, size_t n)
{
    if (!out || n == 0) return false;
    out[0] = 0;
    const esp_partition_t* p = other_slot();
    if (!p) return false;

    esp_app_desc_t d = {};
    // 槽是空的(全 0xFF)时这里会失败 —— 这正是我们要的判据,
    // 不能只看分区表里有没有这个分区。
    if (esp_ota_get_partition_description(p, &d) != ESP_OK) return false;

    strncpy(out, d.project_name, n - 1);
    out[n - 1] = 0;
    return out[0] != 0;
}

bool variant_switch_reboot()
{
    const esp_partition_t* p = other_slot();
    esp_app_desc_t d = {};
    if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) {
        ESP_LOGE(TAG, "另一个槽里没有可用固件,不切");
        return false;
    }

    // 那个槽如果是「推上去但从没成功启动过」或者「被回滚掉的」,切过去等于自找重启循环。
    // ⚠️ PENDING_VERIFY 不拦:它只是"还没自证可用",镜像本身是好的,
    //    而且真起不来 bootloader 会自己退回来 —— 拦掉反而把回滚机制架空了。
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(p, &st) == ESP_OK &&
        (st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED)) {
        ESP_LOGE(TAG, "%s 槽的状态是 %d(坏的),不切", p->label, (int)st);
        return false;
    }

    esp_err_t err = esp_ota_set_boot_partition(p);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切启动分区失败:%s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGW(TAG, "%s → %s(%s),重启", variant_name(), d.project_name, p->label);
    esp_restart();
    return true;   // 走不到
}

}  // namespace tdeck
