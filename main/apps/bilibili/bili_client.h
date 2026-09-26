/*
 * bili_client —— 数据层:分区、列表、详情、封面、视频流。
 *
 * 从 stackchan-bili 的 sim/bili_client.h 搬过来。那边的接口注释写着
 * 「将来搬进固件时只换实现」—— 这次就是那个"将来",所以对外形状基本没动:
 * 仍然是「发起请求 / 轮询结果」的非阻塞形态,LVGL 线程绝不阻塞。
 *
 * 换掉的是实现和容器:
 *   模拟器: libcurl + std::thread + std::vector/std::string + ArduinoJson
 *   设备:   esp_http_client + FreeRTOS task + 定长结构 + cJSON
 *
 * 帧缓冲也换了思路。模拟器那边每帧 new 一个 vector;设备上改成【定长槽位轮转】,
 * 开机分配一次就不再动堆 —— 12fps 下每秒 12 次分配/释放,在 ESP32 上是
 * 内存碎片的可靠来源。
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

namespace bili {

struct Region { int rid; char name[24]; };

struct Video {
    char    bvid[20];
    char    title[96];
    char    up[40];
    char    pic[200];     // 服务端已按请求的尺寸裁好
    int64_t view;
    int     dur;
};

struct Detail {
    char    bvid[20], title[96], up[40], pic[200], desc[320];
    int64_t view, like, danmaku;
    int     dur;
};

struct Meta {
    int  w, h, fps, rate, ch, dur;
    char src[48];
    bool valid;
};

constexpr int LIST_MAX   = 20;   // 一页几条
constexpr int REGION_MAX = 12;

void        set_base(const char* url);
const char* base();

// 分区。阻塞,在后台任务里调。服务没起来时返回内置兜底表 ——
// 进不了界面比看到空列表更糟。
int  get_regions(Region* out, int max);

// ── 列表(非阻塞)──
void request_list(int rid, int pn, int ps);
bool list_pending();
// 有结果返回 true。out 至少 LIST_MAX 条。
bool poll_list(Video* out, int* n, int* rid, int* pn, bool* more);
bool list_failed(const char** err);

// ── 详情(非阻塞)──
void request_detail(const char* bvid);
bool poll_detail(Detail* out);
bool detail_failed(const char** err);

// ── 封面 ──
// 同步抓一张 JPEG,返回 PSRAM 缓冲(调用方 free),*len 是长度。
// url 是服务端给的绝对地址;走 /cover 代理转一道,设备只跟服务端一个 IP 说话。
uint8_t* fetch_cover(const char* url, int* len);

// ── 视频流 ──
void stream_start(const char* bvid, int rate);
void stream_stop();
bool stream_running();
Meta stream_meta();

// 取【该显示的那一帧】。
// 模拟器那边是 UI 循环里 while(next_frame) 自己挑,设备上收进来做:
// 把 pts 已经追平音频时钟的帧全部出队,只保留最后一个 —— 落后的直接丢,
// 宁可跳帧也不能让画面把音频拖住。
// 返回的指针在【下一次调用本函数之前】有效(槽位被占住)。
bool stream_take_due(uint32_t pos_ms, const uint8_t** jpeg, int* len, uint32_t* pts);

void     stream_set_paused(bool paused);
bool     stream_paused();
// 已经【写进功放】的音频字节数换算出的播放位置。视频靠它对齐。
uint32_t stream_audio_pos_ms();
void     stream_stats(int* frames_in, int* dropped, int* pcm_buffered);
bool     stream_error(const char** err);

}  // namespace bili
