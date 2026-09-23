// net.cc — WiFi + NTP + 天气
//
// 桌面只认 launcher_set_time() / launcher_set_weather() 两个入口,不关心数据
// 从哪来。所以这一层可以随便换实现(换天气源、换成手机同步时间)而 UI 不动。
//
// 天气用 Open-Meteo:免费、不要 API key、JSON 直白。代价是 HTTPS,
// 靠 IDF 的证书 bundle 验证,不用自己塞根证书。

#include "net.h"
#include "app.h"
#include "debug/debug_server.h"
#include "secrets.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <esp_netif_sntp.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>

namespace tdeck {
namespace {

const char* TAG = "net";

constexpr int BIT_CONNECTED = BIT0;
char s_ip[16] = "";

// 凭据存 NVS,secrets.h 里的值只作为【首次启动的兜底】。
// 否则改个 WiFi 就要重新编译烧录 —— 在别人家里连个网都得开电脑,不能这么设计。
constexpr const char* NVS_NS = "tdecknet";
char s_ssid[33], s_pass[65];
EventGroupHandle_t s_events;
int s_retry = 0;

void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        lvgl_port_lock(0); launcher_set_online(false); lvgl_port_unlock();
        // 一直重连。掌上机会走出路由器范围再走回来,放弃重连没有意义。
        // 退避到 5 秒封顶,避免离线时空转烧电。
        int delay_ms = (s_retry < 5) ? 1000 : 5000;
        s_retry++;
        ESP_LOGW(TAG, "WiFi 断开,%d ms 后重连(第 %d 次)", delay_ms, s_retry);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* ev = (ip_event_got_ip_t*)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG, "已连上 %s,IP %s", s_ssid, s_ip);
        s_retry = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        lvgl_port_lock(0); launcher_set_online(true); lvgl_port_unlock();
    }
}

void load_credentials()
{
    nvs_handle_t h;
    size_t n;
    bool got = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        n = sizeof(s_ssid);
        if (nvs_get_str(h, "ssid", s_ssid, &n) == ESP_OK && s_ssid[0]) {
            n = sizeof(s_pass);
            nvs_get_str(h, "pass", s_pass, &n);
            got = true;
        }
        nvs_close(h);
    }
    if (!got) {
        strncpy(s_ssid, WIFI_SSID, sizeof(s_ssid) - 1);
        strncpy(s_pass, WIFI_PASSWORD, sizeof(s_pass) - 1);
        ESP_LOGI(TAG, "NVS 里没有凭据,用 secrets.h 的兜底值");
    }
}

void wifi_start()
{
    load_credentials();
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_wifi_event, nullptr, nullptr));

    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, s_pass, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ── 天气 ──
void fetch_weather()
{
    char url[256];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code"
             "&daily=temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=1",
             (double)WX_LATITUDE, (double)WX_LONGITUDE);

    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms        = 8000;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return;

    // 循环读到底。单次 esp_http_client_read 只保证返回"已到达的"数据,
    // 不保证是整个响应体 —— 加了 daily 字段后响应变长,单次读会截断,
    // JSON 解析直接失败。这个 bug 在响应短的时候不会暴露。
    char buf[1024] = {};
    int  len = 0;
    if (esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        while (len < (int)sizeof(buf) - 1) {
            int n = esp_http_client_read(cli, buf + len, sizeof(buf) - 1 - len);
            if (n <= 0) break;
            len += n;
        }
    }
    esp_http_client_cleanup(cli);

    if (len <= 0) { ESP_LOGW(TAG, "天气请求失败"); return; }
    buf[len] = 0;

    cJSON* root = cJSON_Parse(buf);
    if (!root) { ESP_LOGW(TAG, "天气 JSON 解析失败"); return; }
    cJSON* cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON* tp = cJSON_GetObjectItem(cur, "temperature_2m");
        cJSON* cd = cJSON_GetObjectItem(cur, "weather_code");

        // 当日最高/最低。取不到就传 NAN,UI 那边自己决定怎么显示 ——
        // 比在这里塞个 0 或 -999 当哨兵值干净
        float tmax = NAN, tmin = NAN;
        cJSON* daily = cJSON_GetObjectItem(root, "daily");
        if (daily) {
            cJSON* amax = cJSON_GetObjectItem(daily, "temperature_2m_max");
            cJSON* amin = cJSON_GetObjectItem(daily, "temperature_2m_min");
            if (cJSON_IsArray(amax) && cJSON_GetArraySize(amax) > 0)
                tmax = (float)cJSON_GetArrayItem(amax, 0)->valuedouble;
            if (cJSON_IsArray(amin) && cJSON_GetArraySize(amin) > 0)
                tmin = (float)cJSON_GetArrayItem(amin, 0)->valuedouble;
        }

        if (cJSON_IsNumber(tp) && cJSON_IsNumber(cd)) {
            ESP_LOGI(TAG, "天气:WMO %d, 当前 %.1f C, 高 %.1f 低 %.1f",
                     cd->valueint, tp->valuedouble, (double)tmax, (double)tmin);
            // 原始 WMO 代码往上传,由 UI 决定画什么图标、用什么措辞 ——
            // 这一层不替 UI 拼字符串
            lvgl_port_lock(0);
            launcher_set_weather(cd->valueint, (float)tp->valuedouble, tmin, tmax);
            lvgl_port_unlock();
        }
    }
    cJSON_Delete(root);
}

void net_task(void*)
{
    xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, portMAX_DELAY);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) != ESP_OK) {
        ESP_LOGW(TAG, "NTP 对时超时");
    }
    setenv("TZ", TZ_STRING, 1);
    tzset();

    debug_server_start();   // 开发期调试接口,要等拿到 IP

    fetch_weather();
    int64_t last_wx = esp_timer_get_time();

    char hhmm[8], sub[64], last_hhmm[8] = {};
    while (true) {
        time_t now = time(nullptr);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        strftime(hhmm, sizeof(hhmm), "%H:%M", &tm_now);
        // 分钟没变就不碰 UI —— 每秒重设一次 label 会触发无谓的重绘
        if (strcmp(hhmm, last_hhmm) != 0) {
            strcpy(last_hhmm, hhmm);
            char date[32];
            strftime(date, sizeof(date), "%a %d %b", &tm_now);
            // 分隔符用 • (U+2022) 不是 · (U+00B7):LVGL 内建 Montserrat 的字符集是
            // ASCII + ° + • + 图标符号,没有中点,写 · 会渲染成一个缺字形方框。
            snprintf(sub, sizeof(sub), "%s  \xE2\x80\xA2  " WX_CITY, date);
            lvgl_port_lock(0);
            launcher_set_time(hhmm, sub);
            lvgl_port_unlock();
        }

        // 天气 15 分钟一次:Open-Meteo 本身也就这个更新粒度,再频繁没意义
        if (esp_timer_get_time() - last_wx > 15LL * 60 * 1000000) {
            last_wx = esp_timer_get_time();
            fetch_weather();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

NetStatus net_status()
{
    NetStatus st{};
    st.online = s_events && (xEventGroupGetBits(s_events) & BIT_CONNECTED);
    strncpy(st.ssid, s_ssid, sizeof(st.ssid) - 1);
    strncpy(st.ip, st.online ? s_ip : "", sizeof(st.ip) - 1);
    wifi_ap_record_t ap;
    st.rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    return st;
}

int net_scan(ApInfo* out, int max_n)
{
    // 同步扫描。调用方是设置界面里的一次用户操作,阻塞一两秒可以接受,
    // 换成异步反而要给 UI 加一套回调和状态机。
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    if (n > 40) n = 40;

    auto* recs = (wifi_ap_record_t*)calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    esp_wifi_scan_get_ap_records(&n, recs);

    // esp_wifi 已按信号降序给,这里只做同名去重(mesh / 多频段会重复出现)
    int cnt = 0;
    for (int i = 0; i < n && cnt < max_n; i++) {
        if (!recs[i].ssid[0]) continue;
        bool dup = false;
        for (int j = 0; j < cnt; j++)
            if (!strcmp(out[j].ssid, (char*)recs[i].ssid)) { dup = true; break; }
        if (dup) continue;
        strncpy(out[cnt].ssid, (char*)recs[i].ssid, sizeof(out[cnt].ssid) - 1);
        out[cnt].rssi   = recs[i].rssi;
        out[cnt].secure = recs[i].authmode != WIFI_AUTH_OPEN;
        cnt++;
    }
    free(recs);
    return cnt;
}

void net_set_credentials(const char* ssid, const char* password)
{
    if (ssid && ssid[0]) {
        strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
        strncpy(s_pass, password ? password : "", sizeof(s_pass) - 1);
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_str(h, "ssid", s_ssid);
            nvs_set_str(h, "pass", s_pass);
            nvs_commit(h);
            nvs_close(h);
        }
        ESP_LOGI(TAG, "凭据已更新:%s", s_ssid);
    }
    wifi_config_t wc = {};
    strncpy((char*)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, s_pass, sizeof(wc.sta.password) - 1);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    esp_wifi_connect();
}

void net_start()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    wifi_start();
    xTaskCreate(net_task, "net", 6144, nullptr, 4, nullptr);
}

}  // namespace tdeck
