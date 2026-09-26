// launcher.cc — 应用宿主 + 分页桌面
//
// 两屏,横向滑动切换,底部圆点指示 —— 手机桌面的标准形态:
//
//   第 0 屏  首页。时间、日期、天气各自独立排布,不套卡片;小智头像常驻左上。
//            首页不放应用列表 —— 一眼要看到的是"现在几点、外面什么天气",
//            而不是一排图标。
//   第 1 屏  应用。2x2 四张卡片。卡片不是纯图标:图标底衬 + 名称 + 一行说明,
//            同类应用才区分得开。
//
// 全局键位见 ADR-006。q 在 Host 层截获不下发,是逃生口。

#include "app.h"
#include <esp_heap_caps.h>
#include <string.h>
#include "avatar.h"
#include "sys/volume.h"
#include "wx_icon.h"
#include "tdeck_bsp.h"

#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <lvgl.h>
#include <math.h>
#include <string.h>

namespace tdeck {
namespace {

constexpr int SCR_W = 320, SCR_H = 240;

// 主题色取军绿 —— 外壳是透明军绿耗材打印的,UI 跟着走机器才像一件完整的东西。
// 文字色也往绿里偏一点(不是纯中性灰),整屏才是同一个色温。
// 这个绿要够亮。之前 0x4F6B3E 太深,压在右上角那块背光漏光区上,
// 深色本身反而把红光衬出来了 —— 浅色底能盖住漏光,深色图标不能。
constexpr uint32_t C_ACCENT = 0x5E9E33;   // 中绿:选中态、卡片图标、天气图标

// 状态栏单独用浅灰,不跟主题色。
// 右上角是背光漏光最重的位置,那里【颜色越深,红光越被衬出来】——
// 浅色才压得住。绿色试过了,还是偏深。这一块的目标不是好看,是"别把漏光勾出来"。
constexpr uint32_t C_STATUS = 0xAAB0AC;
constexpr uint32_t C_CARD   = 0xffffff;
constexpr uint32_t C_TEXT   = 0x1b2117;
constexpr uint32_t C_MUTE   = 0x6a7360;
constexpr uint32_t C_LINE   = 0xd6dbcd;
constexpr uint32_t C_DOT    = 0x7d8a6e;

// 屏幕相对键盘/外壳偏左,居中的内容整体往右挪,视觉上才是居中。
// 这是机械装配的偏差,只能在 UI 上补。
constexpr int CX = 18;

// 右上状态栏离屏幕右缘的留白。之前排到 288 就停了,右边空出 30 多像素,
// 看着像没排满。iOS 的状态栏是贴着安全区边缘走的,这里也贴紧。
constexpr int SB_R = 4;

constexpr int PAGES     = 3;          // 首页 + 两屏应用
// 桌面网格:每屏 2 列 x 2 行,应用占两屏。
// ⚠️ MAX_APPS 是【容量上限】,不是"当前有几个 app"。注册表按它截断,
// 装不下的 app 会【静默消失】—— 不报错也不换行。
// 之前只有一屏 4 格,加了 B站 之后第 5 个(Palette)正好被吃掉,
// 而且一声不吭。现在超了会 ESP_LOGE,而且两屏共 8 格。
constexpr int GRID_COLS = 2;
constexpr int PER_PAGE  = GRID_COLS * 2;          // 每屏 4 个
constexpr int APP_PAGES = PAGES - 1;              // 首页之外都是应用屏
constexpr int MAX_APPS  = PER_PAGE * APP_PAGES;   // 8

// 应用卡片 2x2
constexpr int CM = 12, CG = 10;                       // 外边距 / 卡间距
constexpr int CW = (SCR_W - CM * 2 - CG) / 2;         // 143
constexpr int CH = 94;
constexpr int CY0 = 16, CY1 = CY0 + CH + CG;          // 16 / 120

const char* TAG = "launcher";

// 壁纸:domain-warped fBm,tools/gen_wallpaper.py 生成,
// 经 CMake EMBED_FILES 以二进制嵌入(150KB,不走 C 数组)。
extern "C" const uint8_t wallpaper_start[] asm("_binary_wallpaper_rgb565_start");
const lv_image_dsc_t kWallpaper = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0, .w = SCR_W, .h = SCR_H, .stride = SCR_W * 2, .reserved_2 = 0 },
    .data_size = SCR_W * SCR_H * 2,
    .data      = wallpaper_start,
};

// ⚠️ 贴图源【搬到 PSRAM】,不要直接从 flash 读。
//
// EMBED_FILES 嵌进来的数据在 flash 里,靠 MMU 映射读。滑动时整屏每帧都要
// 重贴一遍壁纸和四张卡片配图,这些读会和 CPU 取指令抢同一条 flash 总线,
// cache 来回颠簸 —— 同样是 RGB565 直贴,放 flash 比放 PSRAM 慢得多。
// 开机时拷一份到 PSRAM(壁纸 150KB + 四张配图 107KB),之后只读 PSRAM。
// 朝白色插值。pct=0 原色,pct=100 纯白。
uint32_t lighten(uint32_t c, int pct)
{
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r += (255 - r) * pct / 100;
    g += (255 - g) * pct / 100;
    b += (255 - b) * pct / 100;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

const lv_image_dsc_t* to_psram(const lv_image_dsc_t* src)
{
    if (!src) return nullptr;
    auto* dsc = (lv_image_dsc_t*)heap_caps_malloc(sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM);
    void* data = heap_caps_malloc(src->data_size, MALLOC_CAP_SPIRAM);
    if (!dsc || !data) {           // PSRAM 不够就退回 flash,别让界面起不来
        free(dsc); free(data);
        return src;
    }
    memcpy(data, src->data, src->data_size);
    *dsc = *src;
    dsc->data = (const uint8_t*)data;
    return dsc;
}

const lv_image_dsc_t* wallpaper_dsc()
{
    static const lv_image_dsc_t* cached = nullptr;
    if (!cached) cached = to_psram(&kWallpaper);
    return cached;
}

lv_obj_t* mk_label(lv_obj_t* p, const lv_font_t* f, uint32_t c, const char* txt)
{
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(c), LV_PART_MAIN);
    lv_label_set_text(l, txt);
    return l;
}

lv_obj_t* mk_plain(lv_obj_t* p, int x, int y, int w, int h)
{
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

class Host {
public:
    static Host& instance() { static Host h; return h; }

    void begin()
    {
        // 网格只放没被标记隐藏的 app;小智走首屏头像和数字键 0
        auto& all = AppRegistry::instance().apps();
        n_apps_ = 0;
        for (int i = 0; i < (int)all.size() && n_apps_ < MAX_APPS; i++) {
            if (all[i]->hidden_from_grid()) { 
                if (!strcmp(all[i]->name(), "XIAOZHI")) xiaozhi_idx_ = i;
                continue;
            }
            grid_idx_[n_apps_++] = i;
        }
        build();
        lv_screen_load(scr_);
        tdeck_input_subscribe(&Host::bsp_cb, this);
        status_t_ = lv_timer_create([](lv_timer_t* t) {
            static_cast<Host*>(lv_timer_get_user_data(t))->refresh_status();
        }, 10000, this);
        if ((int)all.size() > MAX_APPS)
            ESP_LOGE(TAG, "注册了 %d 个应用,桌面只有 %d 格 —— 多出来的进不了桌面!",
                     (int)all.size(), MAX_APPS);
        ESP_LOGI(TAG, "桌面就绪,%d 个应用,%d 屏", n_apps_, PAGES);
    }

    void set_time(const char* hhmm, const char* sub)
    {
        if (clock_) lv_label_set_text(clock_, hhmm);
        if (date_ && sub) lv_label_set_text(date_, sub);
    }
    void set_weather(int code, float temp_c, float tmin, float tmax)
    {
        if (!wx_) return;
        auto round_c = [](float v) { return (int)(v + (v >= 0 ? 0.5f : -0.5f)); };

        lv_label_set_text_fmt(wx_, "%d\xC2\xB0", round_c(temp_c));
        wx_icon_build(wx_icon_, code, C_ACCENT);

        // 天气描述 + 当日高低温同一行。取不到高低温就只显示描述,
        // 不要显示 "H --° L --°" 那种占位噪音
        if (isnan(tmin) || isnan(tmax)) {
            lv_label_set_text(wx_desc_, wx_text(code));
        } else {
            lv_label_set_text_fmt(wx_desc_, "%s   %d~%d\xC2\xB0",
                                  wx_text(code), round_c(tmin), round_c(tmax));
        }
    }

    void set_online(bool on)
    {
        // 断网只压透明度,颜色仍留在绿色系 —— 跳成灰会破坏整条状态栏的统一
        if (wifi_) lv_obj_set_style_text_opa(wifi_, on ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    }

    // app 画在界面上的「返回」按钮走这里。back() 本身是私有的实现细节,
    // 但屏幕上那颗按钮是公开入口,得有个正经出口。
    void request_back() { back(); }

private:
    // ── 构建 ──
    void build()
    {
        scr_ = lv_obj_create(nullptr);
        // 壁纸挂在【屏幕】上,不是挂在页面上 —— 和手机一样:背景不动,只有内容滑。
        // 之前为了能整页 snapshot 把壁纸挪到了每一页上,结果壁纸跟着一起滑了,这是错的。
        lv_obj_set_style_bg_image_src(scr_, wallpaper_dsc(), LV_PART_MAIN);
        // 这块屏是直角的,背景不要圆角(lv_obj 的默认主题会给圆角)
        lv_obj_set_style_radius(scr_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(scr_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(scr_, LV_OBJ_FLAG_SCROLLABLE);

        // 两屏并排放进一个宽 640 的容器,切屏就是把这个容器整体平移
        // 柔光色块已移除:LVGL 的径向渐变是逐像素算距离 + 插值,三个大圆叠加,
        // 每一滚动帧都要重算全屏 —— 直接把滑动拖卡了。
        // 动态背景改走预渲染循环帧,运行时只剩一次图像 blit。

        // 分页用 LVGL 原生的横向滚动 + 居中吸附,而不是自己写平移动画。
        // 好处是【手指跟随、惯性、边界回弹、拖拽与点击的区分】全都是内建行为 ——
        // 自己实现这套手感要写很多状态机,而且很难做对。
        pages_ = lv_obj_create(scr_);
        lv_obj_set_size(pages_, SCR_W, SCR_H);
        lv_obj_set_pos(pages_, 0, 0);
        lv_obj_set_style_bg_opa(pages_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(pages_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(pages_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(pages_, 0, LV_PART_MAIN);
        lv_obj_set_flex_flow(pages_, LV_FLEX_FLOW_ROW);
        lv_obj_set_scroll_dir(pages_, LV_DIR_HOR);
        lv_obj_set_scroll_snap_x(pages_, LV_SCROLL_SNAP_CENTER);
        lv_obj_set_scrollbar_mode(pages_, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_event_cb(pages_, &Host::scroll_end_cb, LV_EVENT_SCROLL_END, this);

        auto mk_page = [&]() {
            lv_obj_t* pg = lv_obj_create(pages_);
            lv_obj_set_size(pg, SCR_W, SCR_H);
            lv_obj_set_style_bg_opa(pg, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_radius(pg, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(pg, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(pg, 0, LV_PART_MAIN);
            lv_obj_remove_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
            return pg;
        };
        pg_obj_[0] = mk_page();  build_home(pg_obj_[0]);
        for (int i = 0; i < APP_PAGES; i++) {
            pg_obj_[1 + i] = mk_page();
            build_apps(pg_obj_[1 + i], i * PER_PAGE);
        }

        // 圆点指示器建在 scr_ 上而不是 pages_ 里,切屏时它不跟着走
        for (int i = 0; i < PAGES; i++) {
            lv_obj_t* d = lv_obj_create(scr_);
            lv_obj_set_size(d, 7, 7);
            lv_obj_set_pos(d, SCR_W / 2 + CX - (PAGES * 13 - 6) / 2 + i * 13, SCR_H - 16);
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_border_width(d, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(d, 0, LV_PART_MAIN);
            dots_[i] = d;
        }
        paint_dots();

        // 触摸诊断:打一条按下时的坐标。方向参数对不对只能靠真机点四个角来判,
        // 看代码看不出来。定完就删。
    }

    // 滚动停下来时(吸附完成)才更新页码和圆点 —— 拖拽过程中不抖
    static void scroll_end_cb(lv_event_t* e)
    {
        auto* self = static_cast<Host*>(lv_event_get_user_data(e));
        int p = (lv_obj_get_scroll_x(self->pages_) + SCR_W / 2) / SCR_W;
        if (p < 0) p = 0; else if (p >= PAGES) p = PAGES - 1;
        if (p != self->page_) { self->page_ = p; self->paint_dots(); self->paint_selection(); }
    }

    static void card_cb(lv_event_t* e)
    {
        auto* self = static_cast<Host*>(lv_event_get_user_data(e));
        int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
        self->sel_ = idx;
        self->paint_selection();
        self->open(self->grid_idx_[idx]);
    }

    void build_home(lv_obj_t* p)
    {
        // 小智常驻左上。深色块只能放这个角 —— 背光漏光集中在另外三个角
        // (见 docs/hardware.md),压上去会把已经解决的问题重新暴露出来。
        avatar_.create(p, 42, 40, 56);
        // 首屏头像就是小智的入口。做成一块透明的可点区域盖在头像上,
        // 而不是让 Avatar 自己处理点击 —— 那会把"画一张脸"和"当按钮"
        // 两件事耦合在一起。
        lv_obj_t* hit = lv_obj_create(p);
        lv_obj_set_size(hit, 64, 64);
        lv_obj_set_pos(hit, 10, 8);
        lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(hit, 0, LV_PART_MAIN);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, [](lv_event_t* e) {
            auto* self = static_cast<Host*>(lv_event_get_user_data(e));
            if (self->xiaozhi_idx_ >= 0) self->open(self->xiaozhi_idx_);
        }, LV_EVENT_CLICKED, this);

        // 时间/日期/天气各自独立,不套卡片。这是手机锁屏的排布:
        // 信息本身就是画面,加一圈卡片边框反而显得廉价。
        clock_ = mk_label(p, &lv_font_montserrat_48, C_TEXT, "--:--");
        lv_obj_align(clock_, LV_ALIGN_TOP_MID, CX, 62);

        date_ = mk_label(p, &lv_font_montserrat_16, C_MUTE, "connecting");
        lv_obj_align(date_, LV_ALIGN_TOP_MID, CX, 118);

        // 天气单独一块,和时间拉开距离。图标 + 温度一行,描述另起一行 ——
        // "Drizzle 25°" 挤在一起既不好认也不好看。
        wx_icon_ = mk_plain(p, 104 + CX, 146, 34, 30);
        wx_icon_build(wx_icon_, -1, C_ACCENT);

        wx_ = mk_label(p, &lv_font_montserrat_28, C_TEXT, "--");
        lv_obj_set_pos(wx_, 146 + CX, 146);

        wx_desc_ = mk_label(p, &lv_font_montserrat_14, C_MUTE, "");
        lv_obj_set_width(wx_desc_, SCR_W);
        lv_obj_set_style_text_align(wx_desc_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(wx_desc_, CX, 184);

        build_status_bar(p);
    }

    // iOS 那套状态栏:图标 + 百分比,不把原始电压糊在屏幕上 ——
    // 4.12V 对用户没有意义,87% 才有。
    void build_status_bar(lv_obj_t* p)
    {
        // 整条状态栏放进一个【右对齐的 flex 行】,而不是各自写绝对坐标。
        //
        // 绝对坐标的毛病在元素显隐时就暴露了:USB 供电时百分比是空的,
        // 最右那 28px 成了空洞,可见的最右元素离边缘 34px,看着完全没贴边。
        // flex 布局里隐藏的子元素自动退出排版,整组永远贴着右缘。
        lv_obj_t* sb = lv_obj_create(p);
        lv_obj_set_size(sb, LV_SIZE_CONTENT, 20);
        lv_obj_align(sb, LV_ALIGN_TOP_RIGHT, -SB_R, 11);
        lv_obj_set_style_bg_opa(sb, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(sb, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(sb, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_column(sb, 5, LV_PART_MAIN);
        lv_obj_set_flex_flow(sb, LV_FLEX_FLOW_ROW);
        // ⚠️ 主轴必须 START,【不能】用 END。
        // END 要靠一个已知宽度把内容往右推,而这里宽度是 LV_SIZE_CONTENT ——
        // 从内容反推出来的。两者凑一起的结果是:内容宽只算到最后一个子元素
        // (实测 sb w=15,只够装闪电),前面的 wifi 和电池拿到负坐标
        // (x=-52 / x=-29),落在父对象外面被裁掉,屏幕上只剩一个闪电。
        // "贴着右缘"这件事由 sb 自己的 TOP_RIGHT 对齐负责,不该让 flex 管;
        // 隐藏的子元素自动退出排版这一点不受影响。
        lv_obj_set_flex_align(sb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(sb, LV_OBJ_FLAG_SCROLLABLE);

        wifi_ = mk_label(sb, &lv_font_montserrat_14, C_STATUS, LV_SYMBOL_WIFI);
        lv_obj_set_style_text_opa(wifi_, LV_OPA_COVER, LV_PART_MAIN);

        // 电池外壳:框 + 帽,作为一个整体参与 flex 排版
        lv_obj_t* batw = lv_obj_create(sb);
        lv_obj_set_size(batw, 30, 14);
        lv_obj_set_style_bg_opa(batw, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(batw, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(batw, 0, LV_PART_MAIN);
        lv_obj_remove_flag(batw, LV_OBJ_FLAG_SCROLLABLE);

        bat_body_ = lv_obj_create(batw);
        lv_obj_set_size(bat_body_, 26, 13);
        lv_obj_set_pos(bat_body_, 0, 0);
        lv_obj_set_style_radius(bat_body_, 4, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bat_body_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(bat_body_, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(bat_body_, lv_color_hex(C_STATUS), LV_PART_MAIN);
        lv_obj_set_style_pad_all(bat_body_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(bat_body_, LV_OBJ_FLAG_SCROLLABLE);

        bat_fill_ = lv_obj_create(bat_body_);
        lv_obj_set_pos(bat_fill_, 2, 2);
        lv_obj_set_size(bat_fill_, 20, 7);
        lv_obj_set_style_radius(bat_fill_, 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bat_fill_, lv_color_hex(C_STATUS), LV_PART_MAIN);
        lv_obj_set_style_border_width(bat_fill_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bat_fill_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(bat_fill_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* cap = lv_obj_create(batw);
        lv_obj_set_size(cap, 3, 6);
        lv_obj_set_pos(cap, 27, 4);
        lv_obj_set_style_radius(cap, 1, LV_PART_MAIN);
        lv_obj_set_style_bg_color(cap, lv_color_hex(C_STATUS), LV_PART_MAIN);
        lv_obj_set_style_border_width(cap, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(cap, 0, LV_PART_MAIN);

        // 闪电紧跟电池,靠 flex 的 5px 间距,不会被推到天边
        bat_bolt_ = mk_label(sb, &lv_font_montserrat_14, C_STATUS, LV_SYMBOL_CHARGE);
        lv_obj_set_style_text_opa(bat_bolt_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_add_flag(bat_bolt_, LV_OBJ_FLAG_HIDDEN);

        bat_ = mk_label(sb, &lv_font_montserrat_14, C_STATUS, "--");
    }

    // base 是这一屏的第一个应用下标。卡片索引 cards_[] 是【全局】的,
    // 跨屏连续 —— 选中态、键盘导航都按全局下标走,不用再做屏内/屏间转换。
    void build_apps(lv_obj_t* p, int base)
    {
        auto& apps = AppRegistry::instance().apps();
        for (int k = 0; k < PER_PAGE; k++) {
            int i = base + k;
            int x = CM + (k % GRID_COLS) * (CW + CG);
            int y = (k / GRID_COLS == 0) ? CY0 : CY1;

            lv_obj_t* c = lv_obj_create(p);
            lv_obj_set_size(c, CW, CH);
            lv_obj_set_pos(c, x, y);
            // 这块屏是直角的,卡片也跟着做直角:圆角遮罩会强制逐像素混合而不是
            // 整行 memcpy,实测滑动时要多花 13ms/帧(55ms → 41ms)。
            lv_obj_set_style_radius(c, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
            lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
            cards_[i] = c;

            if (i < n_apps_) {
                App* a = apps[grid_idx_[i]];
                const uint32_t bg = a->card_color();
                lv_obj_set_style_bg_color(c, lv_color_hex(bg), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_MAIN);
                // 描边交给 paint_selection 管(未选中是 0,选中是白色粗环)。
                // ⚠️ 这里设了也没用,paint_selection 每次都会重设一遍。
                // ⚠️ 卡片【不加阴影】。LVGL 的阴影是软件高斯模糊,
                // 滑动时整屏每帧重画,实测宽度 6 就要吃掉约 40ms/帧
                // (89ms → 44ms)。卡片本身是浓郁的彩色配图,不靠阴影也跳得出来。
                lv_obj_set_style_shadow_width(c, 0, LV_PART_MAIN);
                // ── 卡片内容:右侧大图标 + 左下角标题 ──────────────
                //
                // 滑动性能这一路是量出来的(LV_USE_PERF_MONITOR):
                //   每张卡自己画矢量内容      152ms/帧   5 FPS
                //   关阴影                    111ms      8 FPS
                //   不画卡片内容               36ms     23 FPS
                // 现在这两个元素都几乎白送:纯色底是整行 memcpy,
                // 图标是一个字形 —— 和"不画内容"基本同价。
                //
                // 图标颜色不是另挑的,是把底色朝白色提亮 55% 算出来的。
                // 这样每张卡的图标都天然和自己的底色同一个色相,
                // 四张卡并排看是一套;手挑四个"亮色"必然会挑歪。
                // ⚠️ 图标要【在固定栏位里居中】,不能直接右对齐。
                // 右对齐对齐的是文本框,而每个 FontAwesome 字形的右侧留白
                // 各不相同 —— 实测四张卡的右边距是 22/12/17/12,
                // 打勾那张明显往里缩。给一个定宽栏位再居中,
                // 字形位置就一致了。
                lv_obj_t* ic = mk_label(c, &lv_font_montserrat_48,
                                        lighten(bg, 55), a->icon());
                lv_obj_set_width(ic, 56);
                lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
                lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -6, -6);

                lv_obj_t* nm = mk_label(c, &lv_font_montserrat_16, 0xFFFFFF,
                                        a->card_title());
                lv_obj_set_pos(nm, 10, CH - 26);

                lv_obj_set_user_data(c, (void*)(intptr_t)i);
                lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);   // 滑动手势要能穿过去
                lv_obj_add_event_cb(c, &Host::card_cb, LV_EVENT_CLICKED, this);
            } else {
                // 空位:极淡的虚位卡片。留着才看得出这是 2x2 的格子,
                // 只画一张孤零零的卡片反而像没做完。
                lv_obj_set_style_bg_color(c, lv_color_hex(0xffffff), LV_PART_MAIN);
                lv_obj_set_style_bg_opa(c, LV_OPA_30, LV_PART_MAIN);
                lv_obj_set_style_border_width(c, 1, LV_PART_MAIN);
                lv_obj_set_style_border_color(c, lv_color_hex(C_LINE), LV_PART_MAIN);
                lv_obj_set_style_border_opa(c, LV_OPA_50, LV_PART_MAIN);
                lv_obj_set_style_shadow_width(c, 0, LV_PART_MAIN);
            }
        }
        paint_selection();
    }

    void paint_selection()
    {
        for (int i = 0; i < MAX_APPS; i++) {
            if (!cards_[i] || i >= n_apps_) continue;
            bool on = (i == sel_ && page_ == 1 + i / PER_PAGE);
            auto* a = AppRegistry::instance().apps()[grid_idx_[i]];

            // 选中 = 白色粗环 + 底色提亮,两个信号一起给。
            //
            // 原来是"2px accent 描边 vs 1px 灰描边"。那是给浅色卡片设计的 ——
            // 卡片换成饱和纯色之后,accent 绿画在紫色/赭色块上跟没画一样,
            // 而且 2px 和 1px 的差别在 143x94 上根本读不出来。
            //
            // 白色是唯一在这四个色相上都跳得出来的颜色(它们都是中深调),
            // 所以环用白不用 accent。底色再提亮一档是第二道保险:
            // 光看环的话,余光扫过去还是容易漏。
            lv_obj_set_style_border_width(cards_[i], on ? 3 : 0, LV_PART_MAIN);
            lv_obj_set_style_border_color(cards_[i], lv_color_white(), LV_PART_MAIN);
            lv_obj_set_style_bg_color(cards_[i],
                lv_color_hex(on ? lighten(a->card_color(), 18) : a->card_color()),
                LV_PART_MAIN);
        }
    }

    void paint_dots()
    {
        for (int i = 0; i < PAGES; i++) {
            bool on = (i == page_);
            lv_obj_set_style_bg_color(dots_[i], lv_color_hex(on ? C_TEXT : C_DOT), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(dots_[i], on ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
        }
    }

    void refresh_status()
    {
        if (!bat_) return;
        if (tdeck_on_external_power()) {
            lv_obj_remove_flag(bat_bolt_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(bat_, LV_OBJ_FLAG_HIDDEN);   // 空串仍占位,必须真隐藏
            lv_obj_set_width(bat_fill_, 20);
            lv_obj_set_style_bg_color(bat_fill_, lv_color_hex(C_STATUS), LV_PART_MAIN);
            return;
        }
        lv_obj_add_flag(bat_bolt_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bat_, LV_OBJ_FLAG_HIDDEN);
        int pct = tdeck_battery_percent();
        if (pct < 0) { lv_label_set_text(bat_, "--"); return; }
        lv_label_set_text_fmt(bat_, "%d", pct);
        lv_obj_set_width(bat_fill_, 2 + 20 * pct / 100);
        // 低电量转红,这是唯一允许跳出军绿色系的地方 —— 警示色就该扎眼
        lv_obj_set_style_bg_color(bat_fill_,
            lv_color_hex(pct <= 15 ? 0xd1443a : C_STATUS), LV_PART_MAIN);
    }

    // ── 翻页 ──
    // 轨迹球/键盘翻页。
    //
    // ⚠️ 必须节流。轨迹球滚一下会产生【一连串】电平翻转(滚一圈十几二十个脉冲),
    // 每个脉冲都调一次这里的话,滚动动画会被反复打断重启 —— 表现就是卡顿和跳动,
    // 很容易误判成渲染性能问题。一次翻页后锁住 320ms,正好盖住动画时长。
    static constexpr uint32_t PAGE_COOLDOWN_MS = 320;

    void goto_page(int p)
    {
        uint32_t now = lv_tick_get();
        if (now - last_page_ms_ < PAGE_COOLDOWN_MS) return;
        if (p < 0 || p >= PAGES || p == page_) return;
        last_page_ms_ = now;
        page_ = p;
        // 翻到应用屏时把选中拉到这一屏第一个,否则光标还留在上一屏,
        // 按回车会打开一个看不见的应用。
        if (p >= 1) {
            int first = (p - 1) * PER_PAGE;
            if (sel_ < first || sel_ >= first + PER_PAGE) sel_ = first;
            if (sel_ >= n_apps_) sel_ = n_apps_ > 0 ? n_apps_ - 1 : 0;
        }
        lv_obj_scroll_to_x(pages_, p * SCR_W, LV_ANIM_ON);
        paint_dots();
        paint_selection();
    }

    // ── 快捷键浮层 ──
    void toggle_shortcuts()
    {
        if (sc_) { lv_obj_delete(sc_); sc_ = nullptr; return; }

        // 全局的永远在,app 自己的接在后面。
        // app 不该把快捷键画在自己界面里(见 app.h 的 Shortcut 注释),
        // 这张表是唯一的去处,所以它必须知道"现在在哪个 app 里"。
        struct Row { const char* k; const char* d; };
        static const Row global_home[] = {
            { "ball L/R",   "switch screen" },
            { "ball U/D",   "move selection" },
            { "y / click",  "open" },
            { "o / i",      "volume + / -" },
            { "0",          "talk to XIAOZHI" },
            { "s",          "this list" },
        };
        static const Row global_app[] = {
            { "n / b",      "back" },
            { "q",          "quit to home" },
            { "o / i",      "volume + / -" },
            { "0",          "talk to XIAOZHI" },
            { "s",          "this list" },
        };
        const Row* base  = current_ ? global_app : global_home;
        int        nbase = current_ ? (int)(sizeof(global_app) / sizeof(global_app[0]))
                                    : (int)(sizeof(global_home) / sizeof(global_home[0]));

        const Shortcut* own = nullptr;
        int nown = current_ ? current_->shortcuts(&own) : 0;
        if (nown > 8) nown = 8;

        const int ROW = 20;
        const int rows = nbase + (nown ? nown + 1 : 0);      // +1 是 app 名那一行
        const int box_h = 46 + rows * ROW + 12;
        const int box_w = 268;

        sc_ = lv_obj_create(lv_layer_top());
        lv_obj_set_size(sc_, SCR_W, SCR_H);
        lv_obj_set_pos(sc_, 0, 0);
        lv_obj_set_style_bg_color(sc_, lv_color_hex(0x0b0f14), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(sc_, LV_OPA_60, LV_PART_MAIN);
        lv_obj_set_style_border_width(sc_, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(sc_, 0, LV_PART_MAIN);
        lv_obj_remove_flag(sc_, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* box = lv_obj_create(sc_);
        lv_obj_set_size(box, box_w, box_h > SCR_H - 8 ? SCR_H - 8 : box_h);
        lv_obj_center(box);
        lv_obj_set_style_radius(box, 18, LV_PART_MAIN);
        lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(box, 28, LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(box, LV_OPA_40, LV_PART_MAIN);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
        lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* h = mk_label(box, &lv_font_montserrat_20, C_TEXT, "Shortcuts");
        lv_obj_set_pos(h, 18, 14);

        int y = 46;
        for (int i = 0; i < nbase; i++) {
            lv_obj_t* k = mk_label(box, &lv_font_montserrat_14, C_TEXT, base[i].k);
            lv_obj_set_pos(k, 18, y);
            lv_obj_t* d = mk_label(box, &lv_font_montserrat_14, C_MUTE, base[i].d);
            lv_obj_set_pos(d, 130, y);
            y += ROW;
        }
        if (nown && own) {
            lv_obj_t* nm = mk_label(box, &lv_font_montserrat_14, C_ACCENT, current_->name());
            lv_obj_set_pos(nm, 18, y);
            y += ROW;
            for (int i = 0; i < nown; i++) {
                lv_obj_t* k = mk_label(box, &lv_font_montserrat_14, C_TEXT, own[i].key);
                lv_obj_set_pos(k, 18, y);
                lv_obj_t* d = mk_label(box, &lv_font_montserrat_14, C_MUTE, own[i].desc);
                lv_obj_set_pos(d, 130, y);
                y += ROW;
            }
        }
    }

    // ── 生命周期 ──
    void open(int i)
    {
        auto& apps = AppRegistry::instance().apps();
        if (i < 0 || i >= (int)apps.size()) return;
        current_ = apps[i];
        app_scr_ = lv_obj_create(nullptr);
        current_->on_enter(app_scr_);
        lv_screen_load_anim(app_scr_, LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);
        ESP_LOGI(TAG, "进入 %s", current_->name());
    }

    void back()
    {
        if (!current_) return;
        ESP_LOGI(TAG, "退出 %s", current_->name());
        lv_screen_load_anim(scr_, LV_SCR_LOAD_ANIM_FADE_IN, 180, 0, false);
        App* c = current_; current_ = nullptr;
        c->on_exit();
        // screen 交给 LVGL 在切换动画结束后自己删,这里提前删会闪
        if (app_scr_) { lv_obj_delete_delayed(app_scr_, 300); app_scr_ = nullptr; }
    }

    // ── 输入 ──
    static void bsp_cb(const tdeck_input_event_t* ev, void* user)
    { static_cast<Host*>(user)->handle(*ev); }

    void handle(const tdeck_input_event_t& ev)
    {
        lvgl_port_lock(0);

        InputEvent ie{};
        if (ev.source == TDECK_INPUT_TRACKBALL) {
            switch (ev.code) {
            case TDECK_TB_UP:    ie.key = Key::Up;    break;
            case TDECK_TB_DOWN:  ie.key = Key::Down;  break;
            case TDECK_TB_LEFT:  ie.key = Key::Left;  break;
            case TDECK_TB_RIGHT: ie.key = Key::Right; break;
            case TDECK_TB_CLICK: ie.key = Key::Enter; break;
            default: break;
            }
        } else if (ev.source == TDECK_INPUT_KEYBOARD) {
            // app 正在做文字输入时,字母全部原样下发,只保留 ESC 作为退路。
            // 轨迹球不受影响(它不是键盘),所以中键和长按永远能用 ——
            // 这就是"声明了 raw keys 也不会把人困住"的保证。
            if (current_ && current_->wants_raw_keys()) {
                if (ev.code == 27) ie.key = Key::Back;
                else { ie.key = Key::Char; ie.ch = (char)ev.code; }
                if (ie.key == Key::Char && (ev.code == 13 || ev.code == 10))
                    ie.key = Key::Enter;
                current_->on_input(ie);
                lvgl_port_unlock();
                return;
            }
            switch (ev.code) {   // OS 级键位约定,见 ADR-006
            case '0':   // 全局:任何界面下按 0 都进小智
                if (xiaozhi_idx_ >= 0 && (!current_ ||
                        strcmp(current_->name(), "XIAOZHI"))) {
                    if (current_) back();
                    open(xiaozhi_idx_);
                }
                lvgl_port_unlock(); return;
            // 音量。BBQ10 键盘上 I / O 的副标就印着 - 和 +,所以这两个键
            // 归系统管:不管当前在哪个 app 都能调,和手机侧边音量键一个意思。
            // 正在打字的 app(wants_raw_keys)在上面已经 return 了,不受影响。
            case 'o': case 'O': tdeck::volume::step(+1); lvgl_port_unlock(); return;
            case 'i': case 'I': tdeck::volume::step(-1); lvgl_port_unlock(); return;
            case 's': case 'S': toggle_shortcuts(); lvgl_port_unlock(); return;
            case 'q': case 'Q':
                if (sc_)      { toggle_shortcuts(); lvgl_port_unlock(); return; }
                if (current_) { back();             lvgl_port_unlock(); return; }
                goto_page(0); lvgl_port_unlock(); return;
            case 'y': case 'Y': case 13: case 10: ie.key = Key::Enter; break;
            case 'n': case 'N': case 'b': case 'B': case 27: ie.key = Key::Back; break;
            default: ie.key = Key::Char; ie.ch = (char)ev.code; break;
            }
        }

        if (sc_) {   // 浮层开着就吞掉一切,只认关闭
            if (ie.key == Key::Back || ie.key == Key::Enter) toggle_shortcuts();
            lvgl_port_unlock();
            return;
        }

        if (current_) {
            if (!current_->on_input(ie) && ie.key == Key::Back) back();
            lvgl_port_unlock();
            return;
        }

        // 桌面导航。第 1 屏上左右先走选择,走到最左一列再往左才翻回首页 ——
        // 和手机上"滑到头才换屏"的手感一致。
        // 翻页方向按 macOS 的"自然滚动"来:内容跟着输入走。
        // 轨迹球往左推 = 把画面往左推 = 露出右边那一屏 → page + 1。
        // 这和触摸左滑翻到下一屏是同一个心智模型,两种输入方式才不会打架。
        // (选择移动是光标语义,左就是左,不做翻转。)
        // 选择移动也要节流,理由同上:一次拨动会来好几个脉冲,
        // 不拦的话光标一下窜过好几格,根本停不到想要的那个。
        bool move_ok = (lv_tick_get() - last_move_ms_) >= MOVE_COOLDOWN_MS;
        // 选中跨屏时,画面要跟着翻过去 —— 否则光标跑到看不见的那一屏上。
        auto moved = [&]() {
            last_move_ms_ = lv_tick_get();
            int want = 1 + sel_ / PER_PAGE;
            if (want != page_) goto_page(want);
            else paint_selection();
        };
        const bool on_apps = (page_ >= 1);

        switch (ie.key) {
        case Key::Left:
            if (on_apps && (sel_ % GRID_COLS) != 0) { if (move_ok) { sel_--; moved(); } }
            else goto_page(page_ + 1);
            break;
        case Key::Right:
            if (on_apps && (sel_ % GRID_COLS) != GRID_COLS - 1 && sel_ + 1 < n_apps_) { if (move_ok) { sel_++; moved(); } }
            else goto_page(page_ - 1);
            break;
        case Key::Up:
            if (on_apps && sel_ >= GRID_COLS && move_ok) { sel_ -= GRID_COLS; moved(); }
            break;
        case Key::Down:
            if (on_apps && sel_ + GRID_COLS < n_apps_ && move_ok) { sel_ += GRID_COLS; moved(); }
            break;
        case Key::Enter:
            if (on_apps) open(grid_idx_[sel_]); else goto_page(1);
            break;
        default: break;
        }
        lvgl_port_unlock();
    }

    App*        current_ = nullptr;
    lv_obj_t*   app_scr_ = nullptr;
    lv_obj_t*   scr_ = nullptr, *pages_ = nullptr, *sc_ = nullptr;
    lv_obj_t*   cards_[MAX_APPS] = {};
    lv_draw_buf_t* card_snap_[MAX_APPS] = {};   // 卡片内容烘出来的位图
    lv_obj_t*      pg_obj_[PAGES] = {};
    lv_obj_t*      snap_img_[PAGES] = {};
    lv_draw_buf_t* snap_buf_[PAGES] = {};
    bool           scrolling_ = false;
    lv_obj_t*   dots_[PAGES] = {};
    lv_obj_t*   clock_ = nullptr, *date_ = nullptr, *wx_ = nullptr, *wx_desc_ = nullptr;
    lv_obj_t*   wx_icon_ = nullptr, *bat_ = nullptr, *bat_body_ = nullptr;
    lv_obj_t*   bat_fill_ = nullptr, *wifi_ = nullptr, *bat_bolt_ = nullptr;

    lv_timer_t* status_t_ = nullptr;
    Avatar      avatar_;
    static constexpr uint32_t MOVE_COOLDOWN_MS = 120;
    uint32_t    last_page_ms_ = 0, last_move_ms_ = 0;
    int         grid_idx_[MAX_APPS] = {};   // 网格位置 → 注册表下标
    int         xiaozhi_idx_ = -1;
    int         page_ = 0, sel_ = 0, n_apps_ = 0;
};

}  // namespace

void launcher_begin()                                { Host::instance().begin(); }
// app 自己画返回按钮时要的退路。键盘 n/b 和轨迹球有 Host 统一兜底,
// 但屏幕上那颗「返回」是 app 画的,得有个正经出口。
void launcher_back()                                 { Host::instance().request_back(); }
void launcher_set_time(const char* a, const char* b) { Host::instance().set_time(a, b); }
void launcher_set_weather(int code, float t, float lo, float hi) { Host::instance().set_weather(code, t, lo, hi); }
void launcher_set_online(bool on)                    { Host::instance().set_online(on); }

}  // namespace tdeck
