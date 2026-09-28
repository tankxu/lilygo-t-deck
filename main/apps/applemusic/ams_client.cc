#include "ams_client.h"
#include "debug/ble_probe.h"
#include "hid_service.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

// NimBLE 的 NVS 绑定存储。IDF 把它编进 bt 组件但没给头文件,官方例程也是这么外部声明的。
extern "C" void ble_store_config_init(void);

namespace tdeck {
namespace ams {
namespace {

const char* TAG = "ams";

// 广播要用它,而它自己又要用广播(断开后重开),所以先声明
int gap_event(ble_gap_event* ev, void* arg);

// ── UUID ──────────────────────────────────────────────────
//
// ⚠️ 128 位 UUID 在 NimBLE 里是【小端】存的,和写在纸上的顺序正好相反。
// 手工倒着敲一遍极容易出错,而且错了完全看不出来 —— 表现只是"服务发现不到",
// 一点都不像 UUID 写反了。所以这里按【书写顺序】写死,运行时再翻。
const uint8_t UU_AMS[16] = {0x89,0xD3,0x50,0x2B,0x0F,0x36,0x43,0x3A,
                            0x8E,0xF4,0xC5,0x02,0xAD,0x55,0xF8,0xDC};
const uint8_t UU_REMOTE[16] = {0x9B,0x3C,0x81,0xD8,0x57,0xB1,0x4A,0x8A,
                               0xB8,0xDF,0x0E,0x56,0xF7,0xCA,0x51,0xC2};
const uint8_t UU_ENTITY_UPDATE[16] = {0x2F,0x7C,0xAB,0xCE,0x80,0x8D,0x41,0x1F,
                                      0x9A,0x0C,0xBB,0x92,0xBA,0x96,0xC1,0x02};
const uint8_t UU_ENTITY_ATTR[16] = {0xC6,0xB2,0xF3,0x8C,0x23,0xAB,0x46,0xD8,
                                    0xA6,0xAB,0xA3,0xA8,0x70,0xBB,0xD5,0xD7};

void uuid_from_written(ble_uuid128_t* out, const uint8_t written[16])
{
    out->u.type = BLE_UUID_TYPE_128;
    for (int i = 0; i < 16; i++) out->value[i] = written[15 - i];
}

bool uuid_is(const ble_uuid_t* u, const uint8_t written[16])
{
    ble_uuid128_t want;
    uuid_from_written(&want, written);
    return ble_uuid_cmp(u, &want.u) == 0;
}

// ── AMS 的实体与属性编号(来自规范,不能自己编)──────────────
enum { ENT_PLAYER = 0, ENT_QUEUE = 1, ENT_TRACK = 2 };
enum { PLAYER_NAME = 0, PLAYER_PLAYBACK = 1, PLAYER_VOLUME = 2 };
enum { QUEUE_INDEX = 0, QUEUE_COUNT = 1, QUEUE_SHUFFLE = 2, QUEUE_REPEAT = 3 };
enum { TRACK_ARTIST = 0, TRACK_ALBUM = 1, TRACK_TITLE = 2, TRACK_DURATION = 3 };

// 要订阅的实体/属性。每个实体一次写入:[EntityID, attr, attr, ...]
const uint8_t SUB_PLAYER[] = {ENT_PLAYER, PLAYER_NAME, PLAYER_PLAYBACK};
const uint8_t SUB_QUEUE[]  = {ENT_QUEUE,  QUEUE_INDEX, QUEUE_COUNT, QUEUE_SHUFFLE, QUEUE_REPEAT};
const uint8_t SUB_TRACK[]  = {ENT_TRACK,  TRACK_ARTIST, TRACK_ALBUM, TRACK_TITLE, TRACK_DURATION};

struct SubStep { const uint8_t* data; uint16_t len; };
const SubStep SUBS[] = {
    {SUB_PLAYER, sizeof(SUB_PLAYER)},
    {SUB_QUEUE,  sizeof(SUB_QUEUE)},
    {SUB_TRACK,  sizeof(SUB_TRACK)},
};
constexpr int SUB_N = sizeof(SUBS) / sizeof(SUBS[0]);

// ── 状态 ──────────────────────────────────────────────────
SemaphoreHandle_t s_lock = nullptr;
Snapshot          s_snap;                 // 只在持锁时碰

uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
uint8_t  s_addr_type = 0;
bool     s_running = false;

uint16_t s_svc_start = 0, s_svc_end = 0;
uint16_t s_h_remote = 0, s_h_remote_cccd = 0;
uint16_t s_h_update = 0, s_h_update_cccd = 0;
uint16_t s_h_attr   = 0;
int      s_sub_i = 0;

void set_link(Link l)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap.link = l;
    xSemaphoreGive(s_lock);
}

void reset_session()
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_svc_start = s_svc_end = 0;
    s_h_remote = s_h_remote_cccd = 0;
    s_h_update = s_h_update_cccd = 0;
    s_h_attr = 0;
    s_sub_i = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    Snapshot fresh;                 // 断开就把曲目信息清掉 ——
    fresh.link = s_snap.link;       // 留着上一首在屏幕上比空着更容易误导
    s_snap = fresh;
    xSemaphoreGive(s_lock);
}

// ── 广播 ──────────────────────────────────────────────────
//
// ⚠️ 手工拼 AD 数据,不用 ble_hs_adv_fields —— 它没有 Service Solicitation
// (AD type 0x15)这一项,而那正是"我想用你身上的 AMS"的表达方式。
//
// 广播包(31 字节上限,这里正好 29):
//   02 01 06                     Flags: LE General Disc + BR/EDR 不支持
//   03 19 C0 03                  Appearance = 0x03C0 HID Generic
//   03 03 12 18                  16 位服务表: 0x1812 HID
//   11 15 <16 字节 UUID(小端)>   128 位服务征求: AMS
//
// 扫描响应:
//   07 09 "T-Deck"               完整本地名
//
// 名字为什么挪到扫描响应:加上 HID 和 appearance 之后广播包只剩 2 字节,
// 放不下名字了。扫描响应是主动扫描时才发的第二包,iOS 会去取。
//
// ⚠️ 0x1812(HID)是【让 iPhone 认得这块板子】的关键,不是为了多一种遥控。
// iOS 的蓝牙设置只列系统认识的配置文件 —— 没有 HID 的时候,
// 板子广播得再标准,设置里也是一片空白(实测过,Mac 能扫到,iPhone 设置没有)。
//
// appearance 用 HID Generic 而不是 Keyboard(0x03C1):我们只报媒体键,
// 不是键盘,报成键盘可能招来键盘设置助理。
// ✅ 实测 HID Generic 就够:T-Deck 出现在 设置→蓝牙 里,能配对,软键盘不受影响。
const char DEV_NAME[] = "T-Deck";

int start_advertising()
{
    uint8_t buf[31];
    int n = 0;

    buf[n++] = 0x02; buf[n++] = 0x01; buf[n++] = 0x06;
    buf[n++] = 0x03; buf[n++] = 0x19; buf[n++] = 0xC0; buf[n++] = 0x03;
    buf[n++] = 0x03; buf[n++] = 0x03; buf[n++] = 0x12; buf[n++] = 0x18;

    buf[n++] = 0x11; buf[n++] = 0x15;
    for (int i = 0; i < 16; i++) buf[n++] = UU_AMS[15 - i];   // 小端

    int rc = ble_gap_adv_set_data(buf, n);
    if (rc != 0) { ESP_LOGE(TAG, "设广播数据失败 rc=%d(%d 字节)", rc, n);
                   set_link(Link::NoService); return rc; }

    uint8_t rsp[31];
    int m = 0;
    const int name_len = (int)strlen(DEV_NAME);
    rsp[m++] = (uint8_t)(1 + name_len); rsp[m++] = 0x09;
    memcpy(rsp + m, DEV_NAME, name_len); m += name_len;
    rc = ble_gap_adv_rsp_set_data(rsp, m);
    if (rc != 0) ESP_LOGW(TAG, "设扫描响应失败 rc=%d(名字就显示不出来了)", rc);

    ble_gap_adv_params adv = {};
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;     // 可连接
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_addr_type, nullptr, BLE_HS_FOREVER, &adv, gap_event, nullptr);
    if (rc != 0) { ESP_LOGE(TAG, "开广播失败 rc=%d", rc); set_link(Link::NoService); return rc; }

    set_link(Link::Advertising);
    ESP_LOGI(TAG, "开始广播(%s,带 HID + AMS 征求),等 iPhone 连过来", DEV_NAME);
    return 0;
}

// ── 订阅流程 ──────────────────────────────────────────────
// GATT 过程不能并发发起,所以整条链是一步接一步在回调里推进的:
//   发现服务 → 发现特征 → 发现描述符(找 CCCD)
//   → 开 Remote Command 通知 → 开 Entity Update 通知
//   → 依次写三个实体的订阅 → Ready
int on_sub_written(uint16_t, const ble_gatt_error* err, ble_gatt_attr*, void*);

void write_next_sub()
{
    if (s_sub_i >= SUB_N) {
        set_link(Link::Ready);
        ESP_LOGI(TAG, "AMS 订阅完成,数据开始流");
        return;
    }
    const SubStep& s = SUBS[s_sub_i];
    int rc = ble_gattc_write_flat(s_conn, s_h_update, s.data, s.len, on_sub_written, nullptr);
    if (rc != 0) ESP_LOGE(TAG, "写订阅 #%d 失败 rc=%d", s_sub_i, rc);
}

int on_sub_written(uint16_t, const ble_gatt_error* err, ble_gatt_attr*, void*)
{
    if (err->status != 0) {
        // 实体不被支持时 iOS 会拒,跳过继续下一个 —— 不是致命错
        ESP_LOGW(TAG, "订阅 #%d 被拒 status=%d,跳过", s_sub_i, err->status);
    }
    s_sub_i++;
    write_next_sub();
    return 0;
}

int on_cccd_update(uint16_t, const ble_gatt_error* err, ble_gatt_attr*, void*)
{
    if (err->status != 0) {
        ESP_LOGE(TAG, "开 Entity Update 通知失败 status=%d", err->status);
        set_link(Link::NoService);
        return 0;
    }
    s_sub_i = 0;
    write_next_sub();
    return 0;
}

int on_cccd_remote(uint16_t, const ble_gatt_error* err, ble_gatt_attr*, void*)
{
    // Remote Command 的通知只是用来知道"这一刻支持哪些命令",
    // 拿不到也不影响遥控本身,所以失败只记一笔就往下走。
    if (err->status != 0) ESP_LOGW(TAG, "开 Remote Command 通知失败 status=%d", err->status);

    uint16_t on = 1;
    int rc = ble_gattc_write_flat(s_conn, s_h_update_cccd, &on, sizeof(on),
                                  on_cccd_update, nullptr);
    if (rc != 0) ESP_LOGE(TAG, "写 Entity Update CCCD 失败 rc=%d", rc);
    return 0;
}

// ── 找 CCCD ───────────────────────────────────────────────
//
// ⚠️ ble_gattc_disc_all_dscs 的 start_handle 必须是【某个特征的值句柄】,
// 不是服务起点;而且回调里那个 chr_val_handle 只是把你传进去的值原样回给你,
// 【不是】"这个描述符属于哪个特征"。
//
// 一开始按后者理解,传了服务起点(45),于是回调里 chr_val_handle 恒等于 45,
// 跟哪个特征都对不上,CCCD 一个都认不出来 —— 报"Entity Update 没有 CCCD",
// 而前面配对、加密、服务发现全是好的,很容易往"iOS 不给权限"的方向查。
//
// 现在对两个特征各发一趟,各取【第一个】0x2902:特征自己的 CCCD 紧跟在它
// 后面,所以先遇到的一定是它的,和服务里特征的排列顺序无关。
enum DscTarget { DSC_REMOTE, DSC_UPDATE };
void disc_dscs(DscTarget which);

int on_dsc(uint16_t, const ble_gatt_error* err, uint16_t,
           const ble_gatt_dsc* dsc, void* arg)
{
    const DscTarget which = (DscTarget)(intptr_t)arg;

    if (err->status == 0) {
        if (dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
            if (which == DSC_REMOTE && !s_h_remote_cccd) s_h_remote_cccd = dsc->handle;
            if (which == DSC_UPDATE && !s_h_update_cccd) s_h_update_cccd = dsc->handle;
        }
        return 0;
    }

    // status 非 0 = 这一趟结束(EDONE,或者这个特征根本没有描述符)
    if (which == DSC_REMOTE) { disc_dscs(DSC_UPDATE); return 0; }

    ESP_LOGI(TAG, "CCCD: remote=%u update=%u", s_h_remote_cccd, s_h_update_cccd);
    if (!s_h_update_cccd) {
        ESP_LOGE(TAG, "Entity Update 没有 CCCD,订阅不了");
        set_link(Link::NoService);
        return 0;
    }

    uint16_t on = 1;
    if (s_h_remote_cccd) {
        ble_gattc_write_flat(s_conn, s_h_remote_cccd, &on, sizeof(on),
                             on_cccd_remote, nullptr);
    } else {
        ble_gattc_write_flat(s_conn, s_h_update_cccd, &on, sizeof(on),
                             on_cccd_update, nullptr);
    }
    return 0;
}

void disc_dscs(DscTarget which)
{
    const uint16_t h = (which == DSC_REMOTE) ? s_h_remote : s_h_update;
    int rc = ble_gattc_disc_all_dscs(s_conn, h, s_svc_end, on_dsc,
                                     (void*)(intptr_t)which);
    if (rc != 0) {
        ESP_LOGE(TAG, "找描述符失败 rc=%d(特征句柄 %u)", rc, h);
        set_link(Link::NoService);
    }
}

int on_chr(uint16_t, const ble_gatt_error* err, const ble_gatt_chr* chr, void*)
{
    if (err->status == 0 && chr) {
        if      (uuid_is(&chr->uuid.u, UU_REMOTE))        s_h_remote = chr->val_handle;
        else if (uuid_is(&chr->uuid.u, UU_ENTITY_UPDATE)) s_h_update = chr->val_handle;
        else if (uuid_is(&chr->uuid.u, UU_ENTITY_ATTR))   s_h_attr   = chr->val_handle;
    }

    if (err->status == BLE_HS_EDONE) {
        if (!s_h_remote || !s_h_update) {
            ESP_LOGE(TAG, "AMS 特征不全(remote=%u update=%u)", s_h_remote, s_h_update);
            set_link(Link::NoService);
            return 0;
        }
        ESP_LOGI(TAG, "AMS 特征:remote=%u update=%u attr=%u",
                 s_h_remote, s_h_update, s_h_attr);
        disc_dscs(DSC_REMOTE);
    }
    return 0;
}

int on_svc(uint16_t, const ble_gatt_error* err, const ble_gatt_svc* svc, void*)
{
    if (err->status == 0 && svc) {
        s_svc_start = svc->start_handle;
        s_svc_end   = svc->end_handle;
    }
    if (err->status == BLE_HS_EDONE) {
        if (!s_svc_start) {
            // 正常情况:iPhone 锁着、或者系统这会儿没发布 AMS。
            // Apple 明说 AMS 不保证一直在,所以这不算错,等 Service Changed。
            ESP_LOGW(TAG, "对面现在没有 AMS(iPhone 可能锁着)");
            set_link(Link::NoService);
            return 0;
        }
        ESP_LOGI(TAG, "找到 AMS,句柄 %u..%u", s_svc_start, s_svc_end);
        ble_gattc_disc_all_chrs(s_conn, s_svc_start, s_svc_end, on_chr, nullptr);
    }
    return 0;
}

void begin_discovery()
{
    set_link(Link::Discovering);
    ble_uuid128_t u; uuid_from_written(&u, UU_AMS);
    int rc = ble_gattc_disc_svc_by_uuid(s_conn, &u.u, on_svc, nullptr);
    if (rc != 0) {
        ESP_LOGE(TAG, "发起服务发现失败 rc=%d", rc);
        set_link(Link::NoService);
    }
}

// ── 通知解析 ──────────────────────────────────────────────
//
// Entity Update 的推送格式:EntityID(1) | AttributeID(1) | Flags(1) | 值(UTF-8)
// Flags 的 bit0 = 被截断(值超过 MTU)。这里不去取完整值 —— 屏幕就 320 宽,
// 截断的歌名照样放不下,为它多跑一趟 Entity Attribute 不划算。
void copy_str(char* dst, size_t cap, const uint8_t* src, uint16_t len)
{
    if (len >= cap) len = (uint16_t)(cap - 1);
    memcpy(dst, src, len);
    dst[len] = 0;
}

void handle_entity_update(const uint8_t* p, uint16_t len)
{
    if (len < 3) return;
    const uint8_t ent = p[0], attr = p[1];
    const uint8_t* val = p + 3;
    const uint16_t vlen = (uint16_t)(len - 3);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    switch (ent) {
    case ENT_TRACK:
        if      (attr == TRACK_ARTIST) copy_str(s_snap.artist, sizeof(s_snap.artist), val, vlen);
        else if (attr == TRACK_ALBUM)  copy_str(s_snap.album,  sizeof(s_snap.album),  val, vlen);
        else if (attr == TRACK_TITLE)  copy_str(s_snap.title,  sizeof(s_snap.title),  val, vlen);
        else if (attr == TRACK_DURATION) {
            char t[32]; copy_str(t, sizeof(t), val, vlen);
            s_snap.duration_s = strtof(t, nullptr);
        }
        break;

    case ENT_PLAYER:
        if (attr == PLAYER_NAME) {
            copy_str(s_snap.player, sizeof(s_snap.player), val, vlen);
        } else if (attr == PLAYER_PLAYBACK) {
            // "状态,速率,已播秒数",例如 "1,1.000,12.345"
            char t[48]; copy_str(t, sizeof(t), val, vlen);
            int st = 0; float rate = 0, el = 0;
            if (sscanf(t, "%d,%f,%f", &st, &rate, &el) >= 1) {
                s_snap.state             = st;
                s_snap.rate              = rate;
                s_snap.elapsed_at_report = el;
                s_snap.reported_at_us    = esp_timer_get_time();
            }
        }
        break;

    case ENT_QUEUE: {
        char t[32]; copy_str(t, sizeof(t), val, vlen);
        if      (attr == QUEUE_INDEX)   s_snap.queue_index = atoi(t);
        else if (attr == QUEUE_COUNT)   s_snap.queue_count = atoi(t);
        else if (attr == QUEUE_SHUFFLE) s_snap.shuffle     = atoi(t);
        else if (attr == QUEUE_REPEAT)  s_snap.repeat      = atoi(t);
        break;
    }
    default: break;
    }
    xSemaphoreGive(s_lock);
}

// Remote Command 的通知体就是"当前支持的 RemoteCommandID 列表",一个字节一条
void handle_remote_notify(const uint8_t* p, uint16_t len)
{
    uint32_t mask = 0;
    for (uint16_t i = 0; i < len; i++) if (p[i] < 32) mask |= (1u << p[i]);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_snap.supported = mask;
    xSemaphoreGive(s_lock);
}

int gap_event(ble_gap_event* ev, void* arg)
{
    (void)arg;
    switch (ev->type) {

    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status != 0) {
            ESP_LOGW(TAG, "连接失败 status=%d,重新广播", ev->connect.status);
            start_advertising();
            break;
        }
        s_conn = ev->connect.conn_handle;
        hid::set_conn(s_conn);
        set_link(Link::Connecting);
        ESP_LOGI(TAG, "iPhone 连上了(conn=%u),开始配对", s_conn);
        // AMS 的三个特征都要求加密链路,所以主动发起 ——
        // 等 iPhone 自己来发起的话,它通常要等到真去读特征被拒之后才动。
        ble_gap_security_initiate(s_conn);
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "断开(reason=%d)", ev->disconnect.reason);
        hid::set_conn(BLE_HS_CONN_HANDLE_NONE);
        reset_session();
        if (s_running) start_advertising();
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0) {
            ESP_LOGI(TAG, "链路已加密,开始找 AMS");
            begin_discovery();
        } else {
            ESP_LOGE(TAG, "加密失败 status=%d", ev->enc_change.status);
            set_link(Link::NoService);
        }
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        const uint16_t h = ev->notify_rx.attr_handle;
        uint16_t len = OS_MBUF_PKTLEN(ev->notify_rx.om);
        uint8_t buf[256];
        if (len > sizeof(buf)) len = sizeof(buf);
        if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, len, &len) != 0) break;

        if      (h == s_h_update) handle_entity_update(buf, len);
        else if (h == s_h_remote) handle_remote_notify(buf, len);
        break;
    }

    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // iPhone 重装/重置之后会拿一个新的 LTK 回来配对。
        // 不删掉旧绑定的话这里会一直失败,而且现象是"连上就断",很难想到是绑定。
        ESP_LOGW(TAG, "对方要求重新配对,删掉旧绑定");
        {
            ble_gap_conn_desc d;
            if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
                ble_store_util_delete_peer(&d.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    case BLE_GAP_EVENT_SUBSCRIBE:
        // iOS 订阅我们的 HID report = 它真把这块板子当 HID 在用。
        // 这条是唯一的信号源,NimBLE 没有"查询订阅状态"的接口。
        hid::on_gap_subscribe(ev->subscribe.attr_handle,
                              ev->subscribe.cur_notify != 0);
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU = %d", ev->mtu.value);
        break;

    default: break;
    }
    return 0;
}

void on_sync()
{
    // 随机地址还是公有地址交给协议栈判断(没烧 public 地址的模组要用随机)
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "定不下本机地址类型");
        set_link(Link::NoService);
        return;
    }
    start_advertising();
}

void on_reset(int reason)
{
    ESP_LOGE(TAG, "协议栈复位,reason=%d", reason);
    reset_session();
}

void configure_host()
{
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // Just Works 配对:板子上没有安全的输入输出通道可言,
    // 假装有反而会让 iPhone 弹一个我们根本显示不出来的配对码。
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;                       // LE Secure Connections
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEV_NAME);
    ble_svc_gap_device_appearance_set(0x03C0);   // HID Generic,见 start_advertising

    // ⚠️ 必须在 ble_gatts_start() 之前 —— 服务表一旦定型就加不进去了,
    // 而 NimBLE 是在 sync 之后自动 start 的,所以这里是最后的窗口。
    hid::register_services();

    // 歌名/专辑名很容易超过默认的 23 字节 MTU,超了就会被 AMS 截断。
    // 要大一点,但别贪 —— 每条连接的缓冲都按这个尺寸算,内部 RAM 很紧。
    ble_att_set_preferred_mtu(256);

    ble_store_config_init();                    // 绑定存 NVS,下次上车不用重配
}

}  // namespace

// ── 对外接口 ──────────────────────────────────────────────

bool start()
{
    if (s_running) return true;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return false;
    }
    reset_session();

    // ⚠️ Starting 必须在起协议栈【之前】设,不能在之后。
    // nimble_port_freertos_init() 一返回,host 任务就可能已经跑完 sync、
    // 在 on_sync() 里把状态设成了 Advertising —— 这时候再 set_link(Starting)
    // 就把它盖回去了,而且【永远】不会再变,因为 sync 只发生一次。
    // 现象是界面一直停在"蓝牙启动中…",看起来像协议栈没起来,
    // 实际上广播早就开了。这是个真踩过的竞态。
    s_running = true;
    set_link(Link::Starting);

    if (!ble_probe_up(configure_host)) {
        ESP_LOGE(TAG, "BLE 协议栈起不来");
        s_running = false;
        set_link(Link::Off);
        return false;
    }
    return true;
}

void stop()
{
    if (!s_running) return;
    s_running = false;
    ble_gap_adv_stop();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE)
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_probe_down();
    reset_session();
    set_link(Link::Off);
}

void snapshot(Snapshot* out)
{
    if (!out) return;
    if (!s_lock) { *out = Snapshot(); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_snap;
    xSemaphoreGive(s_lock);
}

float elapsed_now(const Snapshot& s)
{
    if (s.reported_at_us == 0) return 0;
    if (s.state != PLAY_PLAYING && s.state != PLAY_FORWARD && s.state != PLAY_REWIND)
        return s.elapsed_at_report;

    float dt = (float)(esp_timer_get_time() - s.reported_at_us) / 1000000.0f;
    float e  = s.elapsed_at_report + dt * (s.rate != 0 ? s.rate : 1.0f);
    if (s.duration_s > 0 && e > s.duration_s) e = s.duration_s;
    return e < 0 ? 0 : e;
}

bool supports(const Snapshot& s, Cmd c)
{
    if (s.supported == 0) return true;          // 还没收到列表,别乱灰
    return (s.supported & (1u << (uint8_t)c)) != 0;
}

bool send(Cmd c)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_h_remote) return false;
    uint8_t id = (uint8_t)c;
    int rc = ble_gattc_write_flat(s_conn, s_h_remote, &id, 1, nullptr, nullptr);
    if (rc != 0) ESP_LOGW(TAG, "发命令 %u 失败 rc=%d", id, rc);
    return rc == 0;
}

void forget_bonds()
{
    ble_store_clear();
    ESP_LOGW(TAG, "已清空配对记录");
}

}  // namespace ams
}  // namespace tdeck
