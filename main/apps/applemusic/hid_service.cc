#include "hid_service.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

namespace tdeck {
namespace hid {
namespace {

const char* TAG = "hid";

uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
uint16_t s_h_report = 0;          // Report 特征的值句柄,发通知要用
uint8_t  s_battery  = 100;
bool     s_subscribed = false;

// ── HID Report Map ────────────────────────────────────────
//
// 只有一个 Consumer Control 集合,8 个按键各占 1 位。
// 【故意不放键盘集合】—— iPhone 认定对面是键盘就会收起软键盘。
// 实测这样做之后:设置里能看到 T-Deck,软键盘照常弹。别往这里加键盘。
//
// Report ID 固定 1,和下面 Report Reference 描述符里的值必须一致,
// 对不上的话 iOS 会认为这个 report 不存在,按键全部静默丢掉。
const uint8_t REPORT_MAP[] = {
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x09, 0xCD,        //   Usage (Play/Pause)      bit0
    0x09, 0xB5,        //   Usage (Scan Next)       bit1
    0x09, 0xB6,        //   Usage (Scan Previous)   bit2
    0x09, 0xE9,        //   Usage (Volume Up)       bit3
    0x09, 0xEA,        //   Usage (Volume Down)     bit4
    0x09, 0xE2,        //   Usage (Mute)            bit5
    0x09, 0xB7,        //   Usage (Stop)            bit6
    0x09, 0x00,        //   Usage (Unassigned)      bit7 —— 占位,凑满 8 位
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0xC0               // End Collection
};

// HID Information:bcdHID 1.11,国家码 0,flags = RemoteWake | NormallyConnectable
const uint8_t HID_INFO[] = {0x11, 0x01, 0x00, 0x03};

// PnP ID:厂商来源 USB(0x02) + VID + PID + 版本。
// iOS 会拿它做设备识别,给个稳定的值就行 —— 用 Espressif 的 USB VID 0x303A。
const uint8_t PNP_ID[] = {0x02, 0x3A, 0x30, 0x01, 0x00, 0x00, 0x01};

// Report Reference 描述符:{Report ID, 类型}。类型 1 = Input。
// ⚠️ Report ID 必须和 REPORT_MAP 里的 0x85,0x01 一致。
const uint8_t REPORT_REF[] = {0x01, 0x01};

const char MANUF[] = "LilyGO";

int access_ro(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void* arg)
{
    // 只读的静态数据都走这里,arg 指向一个 {指针,长度} 对
    struct Blob { const void* p; uint16_t n; };
    const Blob* b = (const Blob*)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR &&
        ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, b->p, b->n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

const struct { const void* p; uint16_t n; }
    BLOB_MAP   = {REPORT_MAP, sizeof(REPORT_MAP)},
    BLOB_INFO  = {HID_INFO,   sizeof(HID_INFO)},
    BLOB_PNP   = {PNP_ID,     sizeof(PNP_ID)},
    BLOB_REF   = {REPORT_REF, sizeof(REPORT_REF)},
    BLOB_MANUF = {MANUF,      sizeof(MANUF) - 1};

int access_battery(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, &s_battery, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// Report 特征本身:iOS 会读一次初值,之后都靠通知
int access_report(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    const uint8_t zero = 0;
    return os_mbuf_append(ctxt->om, &zero, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// HID Control Point:主机写 0x00(挂起)/0x01(退出挂起)。收下就行,不用理。
int access_ctrl(uint16_t, uint16_t, ble_gatt_access_ctxt*, void*) { return 0; }

// Protocol Mode:固定报告协议(1)。iOS 会读,少了它有些栈会不高兴。
const uint8_t PROTO_REPORT = 0x01;
int access_proto(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;      // 只接受,不切
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    return os_mbuf_append(ctxt->om, &PROTO_REPORT, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

// ⚠️ 指定初始化在 C++ 里必须【按声明顺序】写,而且不能跳过后面又回头补。
// ble_gatt_chr_def 的顺序是:uuid, access_cb, arg, descriptors, flags,
// min_key_size, val_handle, cpfd。
// 这个宏在 C++ 里【用不了】:BLE_UUID16_DECLARE 展开成
// ((ble_uuid_t*)&(ble_uuid16_t){...}) —— 复合字面量在 C 里有块作用域的
// 存储期,取地址合法;C++ 里那是个右值,直接 "taking address of rvalue"。
// 而且报错位置指向 NimBLE 的头文件、不指向自己的代码,第一眼很像 IDF 的问题。
// 所以把用到的 16 位 UUID 都做成具名常量,取它们的地址。
constexpr ble_uuid16_t mk16(uint16_t v) { return ble_uuid16_t{{BLE_UUID_TYPE_16}, v}; }

const ble_uuid16_t UU_DIS        = mk16(0x180A);
const ble_uuid16_t UU_BAS        = mk16(0x180F);
const ble_uuid16_t UU_HID        = mk16(0x1812);
const ble_uuid16_t UU_PNP        = mk16(0x2A50);
const ble_uuid16_t UU_MANUF      = mk16(0x2A29);
const ble_uuid16_t UU_BATT_LVL   = mk16(0x2A19);
const ble_uuid16_t UU_HID_INFO   = mk16(0x2A4A);
const ble_uuid16_t UU_REPORT_MAP = mk16(0x2A4B);
const ble_uuid16_t UU_HID_CTRL   = mk16(0x2A4C);
const ble_uuid16_t UU_REPORT     = mk16(0x2A4D);
const ble_uuid16_t UU_PROTO_MODE = mk16(0x2A4E);
const ble_uuid16_t UU_REPORT_REF = mk16(0x2908);

const ble_gatt_dsc_def REPORT_DSCS[] = {
    {
        .uuid = &UU_REPORT_REF.u,          // Report Reference
        .att_flags = BLE_ATT_F_READ,
        .min_key_size = 0,
        .access_cb = access_ro,
        .arg = (void*)&BLOB_REF,
    },
    {0},
};

const ble_gatt_chr_def HID_CHRS[] = {
    {
        .uuid = &UU_HID_INFO.u,          // HID Information
        .access_cb = access_ro,
        .arg = (void*)&BLOB_INFO,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &UU_REPORT_MAP.u,          // Report Map
        .access_cb = access_ro,
        .arg = (void*)&BLOB_MAP,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
    },
    {
        .uuid = &UU_HID_CTRL.u,          // HID Control Point
        .access_cb = access_ctrl,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &UU_PROTO_MODE.u,          // Protocol Mode
        .access_cb = access_proto,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &UU_REPORT.u,          // Report (input)
        .access_cb = access_report,
        .arg = nullptr,
        .descriptors = (ble_gatt_dsc_def*)REPORT_DSCS,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_h_report,
    },
    {0},
};

const ble_gatt_chr_def DIS_CHRS[] = {
    {
        .uuid = &UU_PNP.u,          // PnP ID
        .access_cb = access_ro,
        .arg = (void*)&BLOB_PNP,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {
        .uuid = &UU_MANUF.u,          // Manufacturer Name
        .access_cb = access_ro,
        .arg = (void*)&BLOB_MANUF,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ,
    },
    {0},
};

const ble_gatt_chr_def BAS_CHRS[] = {
    {
        .uuid = &UU_BATT_LVL.u,          // Battery Level
        .access_cb = access_battery,
        .arg = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
    },
    {0},
};

const ble_gatt_svc_def SVCS[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UU_DIS.u,          // Device Information
        .includes = nullptr,
        .characteristics = DIS_CHRS,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UU_BAS.u,          // Battery
        .includes = nullptr,
        .characteristics = BAS_CHRS,
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UU_HID.u,          // HID
        .includes = nullptr,
        .characteristics = HID_CHRS,
    },
    {0},
};

}  // namespace

int register_services()
{
    int rc = ble_gatts_count_cfg(SVCS);
    if (rc != 0) { ESP_LOGE(TAG, "count_cfg 失败 rc=%d", rc); return rc; }
    rc = ble_gatts_add_svcs(SVCS);
    if (rc != 0) { ESP_LOGE(TAG, "add_svcs 失败 rc=%d", rc); return rc; }
    ESP_LOGI(TAG, "HID / 设备信息 / 电量 服务已注册");
    return 0;
}

void set_conn(uint16_t conn_handle)
{
    s_conn = conn_handle;
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) s_subscribed = false;
}

bool subscribed()
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_h_report && s_subscribed;
}

void on_gap_subscribe(uint16_t attr_handle, bool notify_enabled)
{
    // 订阅状态没有"查询"接口 —— NimBLE 只在 BLE_GAP_EVENT_SUBSCRIBE 里通知一次,
    // 所以只能自己记。记不准的后果是 tap() 白发一通,不会崩。
    if (attr_handle == s_h_report) {
        s_subscribed = notify_enabled;
        ESP_LOGI(TAG, "iOS %s HID report —— 它%s把我们当 HID 在用",
                 notify_enabled ? "订阅了" : "取消订阅了",
                 notify_enabled ? "确实" : "不再");
    }
}

bool tap(Key k)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_h_report) return false;

    // 按下:对应位置 1;松开:全 0。
    // HID 的按键是【电平】语义,不发松开的话主机会当成一直按着 ——
    // 音量键会一路加到顶,播放键会被当成长按。
    uint8_t down = (uint8_t)(1u << (uint8_t)k);
    uint8_t up   = 0;

    os_mbuf* om = ble_hs_mbuf_from_flat(&down, 1);
    if (!om) return false;
    int rc = ble_gatts_notify_custom(s_conn, s_h_report, om);
    if (rc != 0) { ESP_LOGW(TAG, "发按下失败 rc=%d", rc); return false; }

    // 20ms 足够主机认出一次按键,又短到不会被当成长按
    vTaskDelay(pdMS_TO_TICKS(20));

    om = ble_hs_mbuf_from_flat(&up, 1);
    if (!om) return false;
    rc = ble_gatts_notify_custom(s_conn, s_h_report, om);
    if (rc != 0) ESP_LOGW(TAG, "发松开失败 rc=%d", rc);
    return rc == 0;
}

}  // namespace hid
}  // namespace tdeck
