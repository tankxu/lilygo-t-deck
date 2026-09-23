// debug_server.cc — 开发期 HTTP 调试接口
//
// 存在的理由很实际:没有它的时候,每个 UI 问题都要"我改 → 烧录 → 你看屏幕 →
// 你口述现象 → 我猜"。触摸方向那一轮来回猜了三四遍,还让人白点了两次屏幕,
// 因为我的串口抓取根本没跑起来而我不知道。有了截图和注入,这个闭环我自己就能走完。
//
//   GET /shot                                   截图,裸 RGB565(320*240*2 字节)
//   GET /tap?x=&y=                              在坐标点一下
//   GET /swipe?x1=&y1=&x2=&y2=&ms=              划一下
//   GET /key?c=q                                注入键盘字符
//   GET /ball?d=left|right|up|down|click        注入轨迹球
//   GET /info                                   堆/运行时长等状态
//
// 点击和滑动走【虚拟指针 indev】注册进 LVGL,和真手指完全同一条路径。
// 绕过输入层直接给对象发事件的话,滚动、手势、点击判定都不会被验证到 ——
// 那种测试测的是"我以为的实现",不是实现本身。
//
// ⚠️ 这是开发期工具,没有任何鉴权。发布前用 Kconfig 关掉。

#include "debug_server.h"
#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <esp_http_server.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include <stdlib.h>
#include <string.h>

namespace tdeck {
namespace {

const char* TAG = "debug";

// ── 虚拟指针 ──────────────────────────────────────────────
volatile int32_t s_vx = 0, s_vy = 0;
volatile bool    s_vpressed = false;
lv_indev_t*      s_vindev = nullptr;

void vpointer_read(lv_indev_t*, lv_indev_data_t* d)
{
    d->point.x = s_vx;
    d->point.y = s_vy;
    d->state   = s_vpressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

// 查询参数取整数
int qs_int(httpd_req_t* req, const char* key, int def)
{
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) return def;
    char val[16];
    if (httpd_query_key_value(buf, key, val, sizeof(val)) != ESP_OK) return def;
    return atoi(val);
}

bool qs_str(httpd_req_t* req, const char* key, char* out, size_t n)
{
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) != ESP_OK) return false;
    return httpd_query_key_value(buf, key, out, n) == ESP_OK;
}

// ── /shot ────────────────────────────────────────────────
// 不在设备上编码 PNG:那要引 zlib,而且编码一张 320x240 要几百毫秒。
// 直接吐裸 RGB565,主机侧转换几乎不要钱(tools/rgb565_to_png.py 已经有了)。
esp_err_t h_shot(httpd_req_t* req)
{
    lvgl_port_lock(0);
    lv_draw_buf_t* snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    lvgl_port_unlock();

    if (!snap) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Width",  "320");
    httpd_resp_set_hdr(req, "X-Height", "240");
    httpd_resp_send(req, (const char*)snap->data, snap->data_size);
    lv_draw_buf_destroy(snap);
    return ESP_OK;
}

// ── /tap ─────────────────────────────────────────────────
esp_err_t h_tap(httpd_req_t* req)
{
    int x = qs_int(req, "x", 160), y = qs_int(req, "y", 120);
    s_vx = x; s_vy = y;
    s_vpressed = true;
    vTaskDelay(pdMS_TO_TICKS(90));    // 比 LVGL 的点击判定阈值宽裕
    s_vpressed = false;
    vTaskDelay(pdMS_TO_TICKS(60));

    char msg[64];
    snprintf(msg, sizeof(msg), "tap %d,%d\n", x, y);
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "tap %d,%d", x, y);
    return ESP_OK;
}

// ── /swipe ───────────────────────────────────────────────
// 分步移动而不是瞬移:LVGL 靠连续的位置变化算速度和惯性,
// 一步到位的话滚动容器根本不认为你在拖拽。
esp_err_t h_swipe(httpd_req_t* req)
{
    int x1 = qs_int(req, "x1", 280), y1 = qs_int(req, "y1", 120);
    int x2 = qs_int(req, "x2", 40),  y2 = qs_int(req, "y2", 120);
    int ms = qs_int(req, "ms", 300);
    const int STEPS = 20;

    s_vx = x1; s_vy = y1;
    s_vpressed = true;
    vTaskDelay(pdMS_TO_TICKS(30));
    for (int i = 1; i <= STEPS; i++) {
        s_vx = x1 + (x2 - x1) * i / STEPS;
        s_vy = y1 + (y2 - y1) * i / STEPS;
        vTaskDelay(pdMS_TO_TICKS(ms / STEPS));
    }
    s_vpressed = false;
    vTaskDelay(pdMS_TO_TICKS(120));   // 等惯性和吸附动画走完

    char msg[96];
    snprintf(msg, sizeof(msg), "swipe %d,%d -> %d,%d in %dms\n", x1, y1, x2, y2, ms);
    httpd_resp_sendstr(req, msg);
    ESP_LOGI(TAG, "swipe %d,%d -> %d,%d", x1, y1, x2, y2);
    return ESP_OK;
}

// ── /key ─────────────────────────────────────────────────
esp_err_t h_key(httpd_req_t* req)
{
    char c[8] = {};
    if (!qs_str(req, "c", c, sizeof(c))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need ?c=<char>");
        return ESP_FAIL;
    }
    tdeck_input_inject(TDECK_INPUT_KEYBOARD, (unsigned char)c[0]);
    char msg[32]; snprintf(msg, sizeof(msg), "key '%c'\n", c[0]);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

// ── /ball ────────────────────────────────────────────────
esp_err_t h_ball(httpd_req_t* req)
{
    char d[16] = {};
    if (!qs_str(req, "d", d, sizeof(d))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need ?d=left|right|up|down|click");
        return ESP_FAIL;
    }
    int code = -1;
    if      (!strcmp(d, "up"))    code = TDECK_TB_UP;
    else if (!strcmp(d, "down"))  code = TDECK_TB_DOWN;
    else if (!strcmp(d, "left"))  code = TDECK_TB_LEFT;
    else if (!strcmp(d, "right")) code = TDECK_TB_RIGHT;
    else if (!strcmp(d, "click")) code = TDECK_TB_CLICK;
    if (code < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad direction");
        return ESP_FAIL;
    }
    tdeck_input_inject(TDECK_INPUT_TRACKBALL, code);
    char msg[32]; snprintf(msg, sizeof(msg), "ball %s\n", d);
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

// ── /info ────────────────────────────────────────────────
esp_err_t h_info(httpd_req_t* req)
{
    char buf[320];
    snprintf(buf, sizeof(buf),
             "{\"uptime_s\":%lld,"
             "\"heap_internal\":%u,\"heap_psram\":%u,\"heap_min\":%u,"
             "\"battery_mv\":%d}\n",
             esp_timer_get_time() / 1000000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             tdeck_battery_mv());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

}  // namespace

void debug_server_start()
{
    // 虚拟指针:和触摸并存,两个 indev 互不干扰
    lvgl_port_lock(0);
    s_vindev = lv_indev_create();
    lv_indev_set_type(s_vindev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_vindev, vpointer_read);
    lv_indev_set_disp(s_vindev, lv_display_get_default());
    lvgl_port_unlock();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 12;
    cfg.stack_size       = 8192;     // 截图那条要在栈上折腾

    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "调试服务启动失败");
        return;
    }
    // 用指定初始化:httpd_uri_t 在不同 IDF 版本里字段数会变,
    // 位置初始化会随升级而编译失败
    struct { const char* uri; esp_err_t (*fn)(httpd_req_t*); } routes[] = {
        { "/shot",  h_shot  }, { "/tap",  h_tap  }, { "/swipe", h_swipe },
        { "/key",   h_key   }, { "/ball", h_ball }, { "/info",  h_info  },
    };
    for (auto& r : routes) {
        httpd_uri_t u = {};
        u.uri = r.uri; u.method = HTTP_GET; u.handler = r.fn;
        httpd_register_uri_handler(srv, &u);
    }
    ESP_LOGW(TAG, "调试接口已启动:/shot /tap /swipe /key /ball /info");
}

}  // namespace tdeck
