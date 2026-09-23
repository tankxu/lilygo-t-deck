// net.cc — WiFi + NTP + 天气
//
// 桌面只认 launcher_set_time() / launcher_set_weather() 两个入口,不关心数据
// 从哪来。所以这一层可以随便换实现(换天气源、换成手机同步时间)而 UI 不动。
//
// 天气用 Open-Meteo:免费、不要 API key、JSON 直白。代价是 HTTPS,
// 靠 IDF 的证书 bundle 验证,不用自己塞根证书。

#include "net.h"
#include "app.h"
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
        ESP_LOGI(TAG, "已连上 " WIFI_SSID ",IP " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_events, BIT_CONNECTED);
        lvgl_port_lock(0); launcher_set_online(true); lvgl_port_unlock();
    }
}

void wifi_start()
{
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
    strncpy((char*)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char*)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password) - 1);
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
