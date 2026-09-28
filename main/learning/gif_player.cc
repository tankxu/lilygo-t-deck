#include "gif_player.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <string.h>

// gifdec 是 LVGL 内部的解码器,但 gd_open_gif_data / gd_get_frame /
// gd_render_frame 和 gd_GIF 的字段都是公开的,直接用就行。
#include "src/libs/gif/gifdec.h"

namespace tdeck {
namespace {

const char* TAG = "gifp";

struct Player {
    gd_GIF*        gif = nullptr;
    lv_image_dsc_t dsc{};
    uint8_t*       buf = nullptr;     // ARGB8888 画布,LVGL 直接画它
    lv_timer_t*    timer = nullptr;
    uint32_t       last_ms = 0;
    bool           finished = false;
    bool           first = true;
    int            total = 0;      // 总帧数,0 表示数不出来(那就只能靠上限兜底)
    int            shown = 0;
    // 上一帧的矩形。disposal 会把它擦回背景,所以下一次失效要把它算进去。
    lv_area_t      prev{};
};


// 数出总帧数 —— 判断"放完一轮"要用。
//
// ⚠️ 不能指望 gd_get_frame 返回 0:笔顺 GIF 的 NETSCAPE 块写的是
// loop_count=0(无限循环),gifdec 会一直自己绕回去,那个"最后一轮结束"
// 的信号永远不来。第一版就是这么写的,结果动画放完不收,一直转到 150 秒上限。
//
// 走一遍块结构:0x2C 是图像描述符,一个就是一帧;0x21 是扩展块;
// 两者后面都跟着"长度前缀"的子块链,按链走完就行。
int count_frames(const uint8_t* d, size_t len)
{
    if (len < 13 || memcmp(d, "GIF", 3) != 0) return 0;
    size_t i = 13;
    if (d[10] & 0x80) i += 3u << ((d[10] & 7) + 1);   // 全局调色板
    int n = 0;
    while (i + 1 < len) {
        const uint8_t b = d[i];
        if (b == 0x21) {                               // 扩展块
            i += 2;
            while (i < len && d[i]) i += d[i] + 1;     // 子块链
            i++;
        } else if (b == 0x2C) {                        // 图像描述符 = 一帧
            n++;
            if (i + 9 >= len) break;
            const uint8_t lf = d[i + 9];
            i += 10;
            if (lf & 0x80) i += 3u << ((lf & 7) + 1);  // 局部调色板
            i++;                                       // LZW 最小码长
            while (i < len && d[i]) i += d[i] + 1;
            i++;
        } else {                                       // 0x3B 结束,或者遇到脏字节
            break;
        }
    }
    return n;
}


// 把米字格和引导字提亮。
//
// 服务端画的格线是 #242424、底是 #050505(见 xiaozhi-music-mcp 的
// _stroke_frame_svg),量到屏幕上格线只比背景亮 8/255 —— 3% 的对比度,
// 在 T-Deck 这块屏上等于没有。这个取值从 otto 那会儿就没变过,
// 不是回归,是当初就偏暗。
//
// 服务端改色要重渲染 6763 个 GIF 再重传(146MB),不划算;
// 而画布在我们手里,提亮一次就够:
//   · 只动 [LO, HI] 这一档 —— 格线(~0x1D)和引导字(~0x16)都在里面
//   · 背景(0x05)在 LO 以下,不动,黑还是黑
//   · 笔画(0xF5)在 HI 以上,不动,白还是白
// 只在第一帧(整幅)做一次。后面的增量帧只写自己那一小块笔画,
// disposal 是 none(笔顺是一笔笔叠上去的),所以提亮过的格线一直留着。
constexpr uint8_t GRID_LO = 0x0A;
constexpr uint8_t GRID_HI = 0x40;
constexpr int     GRID_GAIN = 3;      // 0x1D * 3 ≈ 0x57,够看见又不抢笔画

void brighten_guides(uint8_t* argb, uint32_t px)
{
    for (uint32_t i = 0; i < px; i++) {
        uint8_t* p = argb + i * 4;
        // 灰的才动 —— 黄色当前笔(#f6c445)三通道差很大,别碰
        if (p[0] != p[1] || p[1] != p[2]) continue;
        const uint8_t v = p[1];
        if (v < GRID_LO || v > GRID_HI) continue;
        const int n = v * GRID_GAIN;
        const uint8_t out = (uint8_t)(n > 255 ? 255 : n);
        p[0] = p[1] = p[2] = out;
    }
}

void destroy_cb(lv_event_t* e)
{
    auto* p = (Player*)lv_event_get_user_data(e);
    if (!p) return;
    if (p->timer) lv_timer_delete(p->timer);
    if (p->gif)   gd_close_gif(p->gif);
    if (p->buf)   heap_caps_free(p->buf);
    delete p;
}

void tick(lv_timer_t* t)
{
    auto* obj = (lv_obj_t*)lv_timer_get_user_data(t);
    auto* p   = (Player*)lv_obj_get_user_data(obj);
    if (!p || p->finished) return;

    // gce.delay 的单位是厘秒
    const uint32_t want = (uint32_t)p->gif->gce.delay * 10;
    if (!p->first && lv_tick_elaps(p->last_ms) < want) return;
    p->last_ms = lv_tick_get();

    const int has_next = gd_get_frame(p->gif);
    // has_next==0 只在有限循环的 GIF 上才会出现;笔顺 GIF 是无限循环,
    // 所以主要靠数帧来判断"放完一轮"(见 count_frames 的说明)。
    const bool done = (has_next == 0) || (p->total > 0 && p->shown >= p->total);
    if (done) {
        p->finished = true;
        lv_timer_pause(t);
        lv_obj_send_event(obj, LV_EVENT_READY, nullptr);
        return;
    }
    p->shown++;

    // 这一帧的矩形(gifdec 只会往这块里写)
    lv_area_t cur;
    const lv_area_t* content = nullptr;
    lv_area_t coords;
    lv_obj_get_coords(obj, &coords);
    cur.x1 = coords.x1 + p->gif->fx;
    cur.y1 = coords.y1 + p->gif->fy;
    cur.x2 = cur.x1 + p->gif->fw - 1;
    cur.y2 = cur.y1 + p->gif->fh - 1;
    (void)content;

    gd_render_frame(p->gif, p->buf);
    lv_image_cache_drop(&p->dsc);

    if (p->first) {
        // 第一帧整幅都是新的,也是唯一一帧带完整米字格和引导字的
        brighten_guides(p->buf, p->gif->width * p->gif->height);
        lv_obj_invalidate(obj);
        p->first = false;
    } else {
        // ⚠️ 要 union 上一帧的矩形:disposal 会把上一帧那块擦回背景,
        // 只失效当前矩形的话,上一帧留下的残影擦不掉。
        lv_area_t a = cur;
        if (p->prev.x1 < a.x1) a.x1 = p->prev.x1;
        if (p->prev.y1 < a.y1) a.y1 = p->prev.y1;
        if (p->prev.x2 > a.x2) a.x2 = p->prev.x2;
        if (p->prev.y2 > a.y2) a.y2 = p->prev.y2;
        lv_obj_invalidate_area(obj, &a);
    }
    p->prev = cur;

    // 按这一帧自己的延时排下一次。用定时器周期而不是每次都轮询,
    // 省掉大量空跑 —— 笔顺动画大部分帧是 100ms,LVGL 刷新周期才 16ms。
    lv_timer_set_period(t, want < 10 ? 10 : want);
}

}  // namespace

lv_obj_t* gif_player_create(lv_obj_t* parent, const void* data, size_t len)
{
    auto* p = new Player();

    p->total = count_frames((const uint8_t*)data, len);
    p->gif = gd_open_gif_data(data);
    if (!p->gif) {
        ESP_LOGE(TAG, "GIF 解不开");
        delete p;
        return nullptr;
    }

    const uint32_t w = p->gif->width, h = p->gif->height;
    // ARGB8888。放 PSRAM:240x240 就是 230KB,内部 RAM 给不起
    // (而且这块只被 LVGL 读一次/帧,不需要 DMA)。
    p->buf = (uint8_t*)heap_caps_malloc(w * h * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p->buf) {
        ESP_LOGE(TAG, "画布分配失败 %ux%u", (unsigned)w, (unsigned)h);
        gd_close_gif(p->gif);
        delete p;
        return nullptr;
    }
    memset(p->buf, 0, w * h * 4);

    p->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    p->dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888;
    p->dsc.header.w      = w;
    p->dsc.header.h      = h;
    p->dsc.header.stride = w * 4;
    p->dsc.data          = p->buf;
    p->dsc.data_size     = w * h * 4;

    lv_obj_t* obj = lv_image_create(parent);
    lv_image_set_src(obj, &p->dsc);
    lv_obj_set_user_data(obj, p);
    lv_obj_add_event_cb(obj, destroy_cb, LV_EVENT_DELETE, p);

    ESP_LOGI(TAG, "%ux%u,共 %d 帧", (unsigned)w, (unsigned)h, p->total);
    p->timer = lv_timer_create(tick, 10, obj);
    tick(p->timer);            // 立刻出第一帧,别让用户先看到一块黑
    return obj;
}

bool gif_player_finished(lv_obj_t* obj)
{
    if (!obj) return true;
    auto* p = (Player*)lv_obj_get_user_data(obj);
    return !p || p->finished;
}

}  // namespace tdeck
