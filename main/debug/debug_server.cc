// debug_server.cc — 开发期 HTTP 调试接口
//
// 存在的理由很实际:没有它的时候,每个 UI 问题都要"我改 → 烧录 → 你看屏幕 →
// 你口述现象 → 我猜"。触摸方向那一轮来回猜了三四遍,还让人白点了两次屏幕,
// 因为我的串口抓取根本没跑起来而我不知道。有了截图和注入,这个闭环我自己就能走完。
//
//   GET /shot                                   截图,裸 RGB565(320*240*2 字节)
//   GET /tap?x=&y=                              在坐标点一下
//   GET /swipe?x1=&y1=&x2=&y2=&ms=              划一下
//   GET /key?c=q | /key?code=13                 注入键盘字符(code 用于控制字符)
//   GET /ball?d=left|right|up|down|click        注入轨迹球
//   GET /info                                   堆/运行时长等状态
//   POST /ota  (body = tdeck-os.bin)            WiFi 推固件,写另一个槽后重启
//
// 点击和滑动走【虚拟指针 indev】注册进 LVGL,和真手指完全同一条路径。
// 绕过输入层直接给对象发事件的话,滚动、手势、点击判定都不会被验证到 ——
// 那种测试测的是"我以为的实现",不是实现本身。
//
// ⚠️ 这是开发期工具,没有任何鉴权。发布前用 Kconfig 关掉。

#include "sys/volume.h"
#include "sys/variant.h"
#if TDECK_LEARNING
#include "learning/learning.h"
#endif
#include "debug_server.h"
#include "net/net.h"
#include "ble_probe.h"
#include "tdeck_bsp.h"
#include "tdeck_pins.h"

#include <esp_app_desc.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_lvgl_port.h>
#include <esp_heap_caps.h>
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

httpd_handle_t s_srv = nullptr;   // 非空 = 服务在跑(debug_server_stop 要用)

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

    // ⚠️ lv_screen_active() 【不包含】lv_layer_top()。音量 HUD、快捷键浮层、
    // 以后小智的悬浮窗都建在 top layer 上 —— 只拍 active screen 的话它们在
    // 截图里根本不存在,而屏幕上明明看得见。这会让人把"浮层没弹出来"和
    // "浮层弹了但截图看不到"搞混,白白查半天。
    // 所以这里把 top layer 单独拍成 ARGB8888,再按 alpha 合上去。
    // top 和 sys 两层都要合。sys 层放的是 LVGL 自己的性能监视器
    // (LV_USE_PERF_MONITOR),调性能的时候全靠它 —— 只合 top 的话
    // 屏幕上明明有 FPS 读数,截图里却没有。
    lv_obj_t* overlays[2] = { lv_layer_top(), lv_layer_sys() };
    for (int oi = 0; oi < 2 && snap; oi++) {
        lv_obj_t* top = overlays[oi];
        if (lv_obj_get_child_count(top) > 0) {
            lv_draw_buf_t* ov = lv_snapshot_take(top, LV_COLOR_FORMAT_ARGB8888);
            if (ov) {
                int w = (int)snap->header.w, h = (int)snap->header.h;
                if ((int)ov->header.w == w && (int)ov->header.h == h) {
                    for (int y = 0; y < h; y++) {
                        uint16_t* d = (uint16_t*)(snap->data + y * snap->header.stride);
                        uint8_t*  s = ov->data + y * ov->header.stride;
                        for (int x = 0; x < w; x++, s += 4) {
                            uint8_t a = s[3];
                            if (!a) continue;
                            uint8_t sb = s[0], sg = s[1], sr = s[2];
                            uint16_t p = d[x];
                            uint8_t dr = ((p >> 11) & 0x1F) << 3;
                            uint8_t dg = ((p >> 5)  & 0x3F) << 2;
                            uint8_t db = ( p        & 0x1F) << 3;
                            uint8_t r = (uint8_t)((sr * a + dr * (255 - a)) / 255);
                            uint8_t g = (uint8_t)((sg * a + dg * (255 - a)) / 255);
                            uint8_t b = (uint8_t)((sb * a + db * (255 - a)) / 255);
                            d[x] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
                        }
                    }
                }
                lv_draw_buf_destroy(ov);
            }
        }
    }
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
    // ?c=<字符> 打可见字符;?code=<十进制> 打控制字符。
    // 两个都要:httpd_query_key_value 【不做 URL 解码】,所以 ?c=%0D 拿到的
    // 是字面量 '%' 而不是回车 —— 回车(13)、退格(8)、ESC(27) 这些键
    // 只能用 code 注入。踩过一次:搜索框里打出来一串 "bhey%jude%"。
    int code = qs_int(req, "code", -1);
    if (code < 0) {
        char c[8] = {};
        if (!qs_str(req, "c", c, sizeof(c))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need ?c=<char> or ?code=<n>");
            return ESP_FAIL;
        }
        code = (unsigned char)c[0];
    }
    tdeck_input_inject(TDECK_INPUT_KEYBOARD, (unsigned char)code);
    char msg[40]; snprintf(msg, sizeof(msg), "key %d\n", code);
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
    // 带上当前跑在哪个槽、以及 ELF 的 SHA:推完固件一眼就能确认是不是新的那份
    // (只看 uptime 归零分不清"推成功了"和"崩溃重启了")。
    // ⚠️ 不要用 esp_app_desc 里的 date/time —— 那是 __DATE__/__TIME__,
    // 只有全量重编时才会刷新,增量构建推上去还是旧时间戳,反而骗人。
    // ELF 的 SHA 每次构建必变,这才是可靠的"新旧"判据。
    const esp_partition_t* run = esp_ota_get_running_partition();
    char sha[17] = {};
    esp_app_get_elf_sha256(sha, sizeof(sha));
    char buf[480];
    snprintf(buf, sizeof(buf),
             "{\"uptime_s\":%lld,"
             "\"heap_internal\":%u,\"heap_psram\":%u,\"heap_min\":%u,"
             "\"battery_mv\":%d,\"volume\":%d,"
             "\"part\":\"%s\",\"sha\":\"%s\",\"variant\":\"%s\"}\n",
             esp_timer_get_time() / 1000000,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             tdeck_battery_mv(), tdeck::volume::get(),
             run ? run->label : "?", sha, variant_name());
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}
// ── OTA ───────────────────────────────────────────────────
//
// 为什么自己写而不用 esp_https_ota:那套是【设备去拉】,得先有个服务器、
// 先把固件放上去、再想办法告诉设备去拉。开发期要的是反过来 —— 我这边
// 一条命令就推过去。所以这里是设备【被推】,body 就是裸的 tdeck-os.bin。
//
// 写的是 esp_ota_get_next_update_partition() 给的【另一个槽】,正在跑的
// 那个一个字节都不动。所以传到一半断网、或者推了个根本起不来的固件,
// 最坏也只是这次白推,板子还是老固件 —— 这正是双槽存在的意义。
//
// OTA_WITH_SEQUENTIAL_WRITES:我们是顺序写到底的,告诉它这一点就能边写边擦,
// 不用一开始把 6MB 全擦一遍(那要十几秒,而且 HTTP 那头会先超时)。
esp_err_t h_ota(httpd_req_t* req)
{
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_FAIL;
    }
    int total = req->content_len;
    if (total <= 0 || (size_t)total > part->size) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "OTA 开始:%d 字节 -> %s @0x%" PRIx32, total, part->label, part->address);

    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin 失败:%s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    // 缓冲放堆上:HTTP 服务任务栈只有 8K,4K 的局部数组会顶掉大半。
    char* buf = (char*)malloc(4096);
    if (!buf) { esp_ota_abort(h); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }

    int got = 0, last_pct = -10;
    while (got < total) {
        int n = httpd_req_recv(req, buf, total - got > 4096 ? 4096 : total - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) { ESP_LOGE(TAG, "OTA 收数据中断于 %d/%d", got, total); break; }
        if (esp_ota_write(h, buf, n) != ESP_OK) { ESP_LOGE(TAG, "OTA 写失败"); got = -1; break; }
        got += n;
        int pct = got * 100 / total;
        if (pct >= last_pct + 10) { last_pct = pct; ESP_LOGI(TAG, "OTA %d%%", pct); }
    }
    free(buf);

    if (got != total) {
        esp_ota_abort(h);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "incomplete");
        return ESP_FAIL;
    }
    // esp_ota_end 会校验镜像头和 SHA256 —— 推错文件(比如 bootloader.bin)
    // 在这一步就被挡住,不会写进启动分区。
    err = esp_ota_end(h);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA 收尾失败:%s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "ok, rebooting\n");
    ESP_LOGW(TAG, "OTA 完成,1 秒后重启到 %s", part->label);
    // 延后重启,让上面这个响应先发出去 —— 直接 restart 的话客户端看到的是
    // 连接被重置,分不清是成功了还是崩了。
    xTaskCreate([](void*) { vTaskDelay(pdMS_TO_TICKS(1000)); esp_restart(); },
                "ota_reboot", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}


}  // namespace

namespace {

// ── 内存探针 ──────────────────────────────────────────────
//
// 要回答的问题只有一个:把 WiFi 整个拆掉能还回来多少内部 DRAM,
// 够不够装下 BLE 协议栈。光看 /info 的空闲值答不了 —— 得真拆一次再量。
//
// 为什么要一个后台任务而不是在 handler 里直接做:handler 一返回响应才发出去,
// 而拆 WiFi 会把 httpd 一起收掉。所以 handler 立刻返回,测量放到任务里,
// 结果存下来等下一次 /mem 取。
//
// ⚠️ 失败要能自己爬回来:WiFi 恢复不了的话板子就从网上消失了,只能插 USB 救。
// 所以恢复超时直接 esp_restart() —— 重启一定会回到联网状态。
struct MemProbe {
    volatile bool running = false, done = false, restored_ok = false;
    int      mode = 0;                 // 1 = 只拆 WiFi;2 = 拆 WiFi 再把 BLE 装上
    uint32_t base_free = 0, base_big = 0;   // 什么都没动
    uint32_t off_free = 0,  off_big = 0;    // WiFi 拆掉之后
    uint32_t ble_free = 0,  ble_big = 0;    // BLE 起来之后
    bool     ble_ok = false;
    uint32_t back_free = 0;                 // WiFi 恢复之后
} s_probe;

void mem_probe_task(void*)
{
    // 等 handler 的响应真的发出去
    vTaskDelay(pdMS_TO_TICKS(1200));

    s_probe.base_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_probe.base_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    net_suspend();
    vTaskDelay(pdMS_TO_TICKS(1500));   // 让释放走完
    s_probe.off_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_probe.off_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    ESP_LOGW(TAG, "探针:WiFi 关掉后内部堆 %u(原 %u),最大块 %u(原 %u)",
             (unsigned)s_probe.off_free, (unsigned)s_probe.base_free,
             (unsigned)s_probe.off_big,  (unsigned)s_probe.base_big);

    if (s_probe.mode == 2) {
        // 这才是真正要回答的问题:WiFi 让出来的地方,够不够 BLE 站进去
        s_probe.ble_ok = ble_probe_up();
        vTaskDelay(pdMS_TO_TICKS(1500));
        s_probe.ble_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        s_probe.ble_big  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
        ESP_LOGW(TAG, "探针:BLE %s,内部堆 %u,最大块 %u",
                 s_probe.ble_ok ? "起来了" : "没起来",
                 (unsigned)s_probe.ble_free, (unsigned)s_probe.ble_big);
        ble_probe_down();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    net_resume();
    for (int i = 0; i < 300 && !net_status().online; i++) vTaskDelay(pdMS_TO_TICKS(100));
    s_probe.restored_ok = net_status().online;
    s_probe.back_free   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s_probe.done    = true;
    s_probe.running = false;

    if (!s_probe.restored_ok) {
        ESP_LOGE(TAG, "WiFi 没能恢复,重启把自己救回来");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    vTaskDelete(nullptr);
}


#if TDECK_LEARNING
// ── 学习卡片的调试入口 ─────────────────────────────────────
//
// 没有它就只能对着小智说话来触发,验一个公式排版要绕一大圈。
//   /learn?text=学&guide=xué      汉字/词卡
//   /learn?text=because&guide=/bɪˈkɒz/&meaning=conj. 因为
//   /learn?stroke=学              笔顺动画
//   /learn?page=正文&title=标题    文字页
//   /learn?formula=<LaTeX>&title=&note=
//   /learn?clear=1
// 值要 URL 编码(中文和 LaTeX 里的反斜杠都必须转)。
esp_err_t h_learn(httpd_req_t* req)
{
    char q[768] = {};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    char v[512], t[256], g[256];

    auto get = [&](const char* k, char* out, size_t n) -> bool {
        if (httpd_query_key_value(q, k, out, n) != ESP_OK) return false;
        // httpd 不解百分号编码,自己来 —— 中文和 LaTeX 的反斜杠全靠它
        size_t o = 0;
        for (size_t i = 0; out[i]; i++) {
            if (out[i] == '%' && out[i+1] && out[i+2]) {
                auto hex = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
                out[o++] = (char)(hex(out[i+1]) * 16 + hex(out[i+2]));
                i += 2;
            } else if (out[i] == '+') out[o++] = ' ';
            else out[o++] = out[i];
        }
        out[o] = 0;
        return true;
    };

    t[0] = g[0] = 0;
    get("title", t, sizeof(t));
    get("guide", g, sizeof(g));

    if (get("clear", v, sizeof(v)))        tdeck::learning::clear();
    else if (get("stroke", v, sizeof(v)))  tdeck::learning::show_stroke_order(v);
    else if (get("text", v, sizeof(v))) {
        char mean[192] = {};
        get("meaning", mean, sizeof(mean));
        tdeck::learning::show_card(v, g, mean);
    }
    else if (get("page", v, sizeof(v)))    tdeck::learning::show_page(t, v);
    else if (get("formula", v, sizeof(v))) {
        char note[256] = {};
        get("note", note, sizeof(note));
        tdeck::learning::show_formula(t, v, note);
    } else {
        httpd_resp_sendstr(req, "用法: /learn?text= | stroke= | page= | formula= | clear=1\n");
        return ESP_OK;
    }
    httpd_resp_sendstr(req, "ok\n");
    return ESP_OK;
}
#endif

esp_err_t h_mem(httpd_req_t* req)
{
    char q[48] = {};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    int mode = 0;
    if (strstr(q, "test=1")) mode = 1;
    if (strstr(q, "test=2")) mode = 2;

    // BLE 单独起停 —— 不动 WiFi,用来看两个协议栈能不能同时活着
    if (strstr(q, "ble=1")) ble_probe_up();
    if (strstr(q, "ble=0")) ble_probe_down();

    if (mode && !s_probe.running) {
        s_probe = MemProbe{};
        s_probe.mode    = mode;
        s_probe.running = true;
        xTaskCreate(mem_probe_task, "memprobe", 4096, nullptr, 5, nullptr);
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"now_free\":%u,\"now_big\":%u,\"psram_free\":%u,"
             "\"wifi_suspended\":%d,\"ble_up\":%d,\"probe_running\":%d,\"probe_done\":%d,"
             "\"base_free\":%u,\"base_big\":%u,"
             "\"off_free\":%u,\"off_big\":%u,\"wifi_gain\":%d,"
             "\"ble_ok\":%d,\"ble_free\":%u,\"ble_big\":%u,\"ble_cost\":%d,"
             "\"back_free\":%u,\"restored\":%d}\n",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (int)net_suspended(), (int)ble_probe_is_up(),
             (int)s_probe.running, (int)s_probe.done,
             (unsigned)s_probe.base_free, (unsigned)s_probe.base_big,
             (unsigned)s_probe.off_free,  (unsigned)s_probe.off_big,
             (int)s_probe.off_free - (int)s_probe.base_free,
             (int)s_probe.ble_ok,
             (unsigned)s_probe.ble_free, (unsigned)s_probe.ble_big,
             (int)s_probe.off_free - (int)s_probe.ble_free,
             (unsigned)s_probe.back_free, (int)s_probe.restored_ok);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

}  // namespace

void debug_server_stop()
{
    if (!s_srv) return;
    httpd_stop(s_srv);
    s_srv = nullptr;
    ESP_LOGW(TAG, "调试服务已停");
}

void debug_server_start()
{
    // ⚠️ 幂等判断必须在【最前面】。net_task 每秒调一次这个函数(挂起恢复后
    // 靠它自己把服务拉回来),判断放在建 indev 之后的话,每秒漏一个 lv_indev
    // —— 实测 337 字节/秒,十分钟吃掉 200KB,现象是内部堆莫名其妙一直往下掉。
    if (s_srv) return;

    // 虚拟指针:和触摸并存,两个 indev 互不干扰
    lvgl_port_lock(0);
    s_vindev = lv_indev_create();
    lv_indev_set_type(s_vindev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(s_vindev, vpointer_read);
    lv_indev_set_disp(s_vindev, lv_display_get_default());
    lvgl_port_unlock();

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = 80;
    cfg.max_uri_handlers = 14;
    cfg.stack_size       = 8192;     // 截图那条要在栈上折腾

    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "调试服务启动失败");
        return;
    }
    // 用指定初始化:httpd_uri_t 在不同 IDF 版本里字段数会变,
    // 位置初始化会随升级而编译失败
    struct { const char* uri; httpd_method_t m; esp_err_t (*fn)(httpd_req_t*); } routes[] = {
        { "/shot",  HTTP_GET,  h_shot  }, { "/tap",  HTTP_GET, h_tap  },
        { "/swipe", HTTP_GET,  h_swipe }, { "/key",  HTTP_GET, h_key  },
        { "/ball",  HTTP_GET,  h_ball  }, { "/info", HTTP_GET, h_info },
        { "/ota",   HTTP_POST, h_ota   }, { "/mem",  HTTP_GET, h_mem  },
#if TDECK_LEARNING
        { "/learn", HTTP_GET, h_learn },
#endif,
    };
    for (auto& r : routes) {
        httpd_uri_t u = {};
        u.uri = r.uri; u.method = r.m; u.handler = r.fn;
        httpd_register_uri_handler(srv, &u);
    }
    s_srv = srv;

    // 走到这里说明:WiFi 通了、HTTP 服务起来了 —— 也就是【还能再推一版】。
    // 这正是"这个固件可用"的判据,所以在这里销掉回滚。
    // 在此之前如果崩溃重启,bootloader 会自动退回上一个槽。
    esp_ota_img_states_t st;
    const esp_partition_t* run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGW(TAG, "新固件已确认可用(%s),取消回滚", run->label);
    }
    ESP_LOGW(TAG, "调试接口已启动:/shot /tap /swipe /key /ball /info /ota (运行于 %s)",
             run->label);
}

}  // namespace tdeck
