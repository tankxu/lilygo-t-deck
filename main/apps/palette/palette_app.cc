// palette_app.cc — 色板
//
// 移植自 sticks3 的 palette.lua,按 T-Deck 的 320x240 重做布局。
//
// 16x16 = 256 色:每一列是一个色相,列内自上而下从「加白」过渡到「纯色」再到「加黑」,
// 所以横向扫色相、纵向调明暗,当调色板用比按 RGB 排列直观得多。
//
// 相比 sticks3 版多显示一个 RGB565 值 —— 在嵌入式里真正要往代码里填的是这个数,
// 而不是 #RRGGBB(ST7789 收的就是 RGB565,而且还是大端)。

#include "app.h"
#include <lvgl.h>
#include <math.h>
#include <stdio.h>

namespace {

// 卡片配图。tools/gen_card_art.py 生成,CMake EMBED_FILES 以二进制嵌入
// (143x94 RGB565,26884 字节),不走 C 数组。
extern "C" const uint8_t card_palette_start[] asm("_binary_card_palette_rgb565_start");
const lv_image_dsc_t kCardArt = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0, .w = 143, .h = 94, .stride = 143 * 2, .reserved_2 = 0 },
    .data_size = 143 * 94 * 2,
    .data      = card_palette_start,
};

constexpr int COLS = 16, ROWS = 16;
constexpr int CW = 19, CH = 11;            // 色块尺寸:16*19=304 宽,16*11=176 高
constexpr int GX = (320 - COLS * CW) / 2;  // = 8,左右各留 8
constexpr int GY = 54;                     // 顶部留给信息区

// 浅色主题 —— 理由见 main.cc:LCD 背光恒亮,深色不省电却会暴露四角漏光
constexpr uint32_t C_BG    = 0xf6f8fa;
constexpr uint32_t C_TEXT  = 0x1f2328;
constexpr uint32_t C_MUTE  = 0x57606a;

struct Rgb { uint8_t r, g, b; };

// HSV → RGB。h 取 0..360,s/v 取 0..1
Rgb hsv(float h, float s, float v)
{
    float c = v * s;
    float hp = h / 60.0f;
    float x = c * (1.0f - fabsf(fmodf(hp, 2.0f) - 1.0f));
    float m = v - c;
    float r = 0, g = 0, b = 0;
    if      (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    return { (uint8_t)((r + m) * 255 + 0.5f),
             (uint8_t)((g + m) * 255 + 0.5f),
             (uint8_t)((b + m) * 255 + 0.5f) };
}

// RGB888 → RGB565:红 5 位、绿 6 位(人眼对绿最敏感)、蓝 5 位
inline uint16_t to565(Rgb c)
{
    return (uint16_t)(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}

class PaletteApp : public tdeck::App {
public:
    const char* name() const override   { return "Palette"; }
    const lv_image_dsc_t* card_art() const override  { return &kCardArt; }
    const char* card_title() const override           { return "Palette"; }
    const char* icon() const override   { return LV_SYMBOL_IMAGE; }
    uint32_t    accent() const override { return 0x55853A; }

    // 卡片就是一片真实的色带 —— 不是"一个调色板图标 + Palette 字样"。
    // 看一眼就知道这个 app 是干什么的,不需要读文字。
    void render_card(lv_obj_t* card) override
    {
        build_colors();
        const int W = 143, H = 94, BARS = 16;

        // 每列一个色相,取该列中段的饱和色(rows 6~9 是最鲜艳的一段)
        for (int i = 0; i < BARS; i++) {
            lv_obj_t* bar = lv_obj_create(card);
            lv_obj_set_size(bar, (W + BARS - 1) / BARS, H);
            lv_obj_set_pos(bar, i * W / BARS, 0);
            lv_obj_set_style_bg_color(bar, lv_color_hex(rgb_[7 * COLS + i]), LV_PART_MAIN);
            lv_obj_set_style_radius(bar, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        }

        // 底部压一条半透明深色带,白字才在任何色相上都读得出来 ——
        // 直接把文字放在彩色条上,遇到黄色那几列就看不见了
        lv_obj_t* scrim = lv_obj_create(card);
        lv_obj_set_size(scrim, W, 30);
        lv_obj_set_pos(scrim, 0, H - 30);
        lv_obj_set_style_bg_color(scrim, lv_color_hex(0x101410), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scrim, LV_OPA_70, LV_PART_MAIN);
        lv_obj_set_style_radius(scrim, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(scrim, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(scrim, 0, LV_PART_MAIN);
        lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(scrim, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* nm = lv_label_create(scrim);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, LV_PART_MAIN);
        lv_obj_set_style_text_color(nm, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(nm, "Palette");
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 10, 0);
    }

    void on_enter(lv_obj_t* root) override
    {
        build_colors();

        lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        // ── 信息区:hex(大)+ RGB565(小)+ 色块预览 ──
        hex_ = lv_label_create(root);
        lv_obj_set_style_text_font(hex_, &lv_font_montserrat_28, LV_PART_MAIN);
        lv_obj_set_style_text_color(hex_, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_pos(hex_, 8, 4);

        r565_ = lv_label_create(root);
        lv_obj_set_style_text_color(r565_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_pos(r565_, 10, 34);

        preview_ = lv_obj_create(root);
        lv_obj_set_size(preview_, 44, 44);
        lv_obj_set_pos(preview_, 320 - 44 - 8, 4);
        lv_obj_set_style_radius(preview_, 6, LV_PART_MAIN);
        lv_obj_set_style_border_width(preview_, 1, LV_PART_MAIN);
        lv_obj_set_style_border_color(preview_, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_set_style_pad_all(preview_, 0, LV_PART_MAIN);

        // ── 256 个色块 ──
        for (int i = 0; i < COLS * ROWS; i++) {
            lv_obj_t* sw = lv_obj_create(root);
            lv_obj_set_size(sw, CW, CH);
            lv_obj_set_pos(sw, GX + (i % COLS) * CW, GY + (i / COLS) * CH);
            lv_obj_set_style_bg_color(sw, lv_color_hex(rgb_[i]), LV_PART_MAIN);
            lv_obj_set_style_radius(sw, 0, LV_PART_MAIN);
            lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
            lv_obj_set_style_pad_all(sw, 0, LV_PART_MAIN);
            lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE);
        }

        // ── 光标 ──
        // 白边 + 黑描边:单一颜色的框在 256 色上总会撞色,两层一起就能保证任何底色上都看得见
        cursor_ = lv_obj_create(root);
        lv_obj_set_size(cursor_, CW + 2, CH + 2);
        lv_obj_set_style_bg_opa(cursor_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_radius(cursor_, 0, LV_PART_MAIN);
        lv_obj_set_style_border_width(cursor_, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(cursor_, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_outline_width(cursor_, 1, LV_PART_MAIN);
        lv_obj_set_style_outline_color(cursor_, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(cursor_, 0, LV_PART_MAIN);

        select(0);
    }

    void on_exit() override
    {
        // 控件树由 launcher 连同 screen 一起销毁,这里只需要清掉指针
        hex_ = r565_ = preview_ = cursor_ = nullptr;
    }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        // 轨迹球:左右 ±1 走色相,上下 ±16 换一行明暗
        switch (ev.key) {
        case tdeck::Key::Right: select(idx_ + 1);    return true;
        case tdeck::Key::Left:  select(idx_ - 1);    return true;
        case tdeck::Key::Down:  select(idx_ + COLS); return true;
        case tdeck::Key::Up:    select(idx_ - COLS); return true;
        default: return false;   // Back / Enter 交给 Host 处理
        }
    }

private:
    void build_colors()
    {
        for (int col = 0; col < COLS; col++) {
            float h = col * (360.0f / COLS);
            for (int row = 0; row < ROWS; row++) {
                float s, v;
                if (row < 8) { s = 0.15f + row * 0.12f; v = 1.0f; }        // 加白
                else         { s = 1.0f; v = 0.95f - (row - 8) * 0.11f; }  // 加黑
                Rgb c = hsv(h, s, v);
                rgb_[row * COLS + col] = ((uint32_t)c.r << 16) | ((uint32_t)c.g << 8) | c.b;
            }
        }
    }

    void select(int i)
    {
        idx_ = ((i % 256) + 256) % 256;
        uint32_t v = rgb_[idx_];
        Rgb c = { (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };

        lv_label_set_text_fmt(hex_, "#%06X", (unsigned)v);
        lv_label_set_text_fmt(r565_, "RGB565  0x%04X", to565(c));
        lv_obj_set_style_bg_color(preview_, lv_color_hex(v), LV_PART_MAIN);
        lv_obj_set_pos(cursor_, GX + (idx_ % COLS) * CW - 1, GY + (idx_ / COLS) * CH - 1);
    }

    uint32_t   rgb_[COLS * ROWS] = {};
    int        idx_ = 0;
    lv_obj_t*  hex_ = nullptr;
    lv_obj_t*  r565_ = nullptr;
    lv_obj_t*  preview_ = nullptr;
    lv_obj_t*  cursor_ = nullptr;
};

}  // namespace

TDECK_REGISTER_APP(PaletteApp)
