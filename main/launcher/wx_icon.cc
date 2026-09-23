// wx_icon.cc — 天气图标
//
// 用 LVGL 图元(圆、圆角矩形)拼,不引图标字体也不引位图:
//   - 字体方案要跑 LVGL 的字体转换器,多一个构建期依赖,而且只为几个字形
//   - 位图方案要为每个尺寸各存一套,还得考虑抗锯齿边缘和背景的合成
// 图元拼出来的和 UI 其它部分是同一套圆角/描边语言,尺寸也能随便改。

#include "wx_icon.h"

namespace tdeck {
namespace {

// 把颜色往白里混。用它来做"淡一档"的效果,而【不是】降低不透明度 ——
// 半透明的图元一旦互相重叠,重叠区的 alpha 会叠加变深,
// 于是本该融成一朵的云,变成了几个深色的透镜形交叠区:看起来就是几个圆,
// 不是一朵云。这就是之前天气图标出现"绿色的圆"的原因。
uint32_t lighten(uint32_t c, float t)
{
    int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    r += (int)((255 - r) * t);
    g += (int)((255 - g) * t);
    b += (int)((255 - b) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

lv_obj_t* blob(lv_obj_t* p, int x, int y, int w, int h, int r, uint32_t c, lv_opa_t opa = LV_OPA_COVER)
{
    lv_obj_t* o = lv_obj_create(p);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, r, LV_PART_MAIN);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, opa, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(o, false);
    return o;
}

// 云:底部一条圆角横条 + 两个交叠的圆。三者【全不透明且同色】,才会融成一朵。
// 要淡就把颜色调淡,不要降 alpha(见 lighten 的注释)。
void cloud(lv_obj_t* p, int ox, int oy, uint32_t c)
{
    blob(p, ox + 2,  oy + 12, 28, 11, 6,  c);
    blob(p, ox + 4,  oy + 6,  13, 13, 7,  c);
    blob(p, ox + 15, oy + 2,  17, 17, 9,  c);
}

void sun(lv_obj_t* p, int ox, int oy, int d, uint32_t c)
{
    blob(p, ox, oy, d, d, d / 2, c);
}

// 雨滴 / 雪点 / 雾线,统一从云底往下排
void drops(lv_obj_t* p, int ox, int oy, int n, int h, int w, uint32_t c)
{
    for (int i = 0; i < n; i++) blob(p, ox + 5 + i * 8, oy, w, h, w / 2, c);
}

}  // namespace

const char* wx_text(int code)
{
    if (code == 0)                return "Clear";
    if (code <= 2)                return "Partly cloudy";
    if (code == 3)                return "Overcast";
    if (code == 45 || code == 48) return "Fog";
    if (code >= 51 && code <= 57) return "Drizzle";
    if (code >= 61 && code <= 67) return "Rain";
    if (code >= 71 && code <= 77) return "Snow";
    if (code >= 80 && code <= 82) return "Showers";
    if (code >= 95)               return "Thunderstorm";
    return "--";
}

void wx_icon_build(lv_obj_t* p, int code, uint32_t c)
{
    lv_obj_clean(p);
    const uint32_t DIM = lighten(c, 0.45f);   // 云用淡色,雨丝/太阳用本色,层次靠明度不靠 alpha

    if (code == 0) {                              // 晴
        sun(p, 6, 2, 24, c);
    } else if (code <= 2) {                       // 少云:太阳露一角
        sun(p, 1, 0, 16, c);
        cloud(p, 2, 7, DIM);
    } else if (code == 3) {                       // 阴
        cloud(p, 2, 5, DIM);
    } else if (code == 45 || code == 48) {        // 雾:三条横线
        for (int i = 0; i < 3; i++)
            blob(p, 3 + (i % 2) * 3, 8 + i * 7, 26, 4, 2, i == 1 ? c : DIM);
    } else if (code >= 71 && code <= 77) {        // 雪:圆点
        cloud(p, 2, 0, DIM);
        drops(p, 2, 23, 3, 4, 4, c);
    } else if (code >= 95) {                      // 雷暴:云 + 折线闪电
        cloud(p, 2, 0, DIM);
        blob(p, 16, 21, 4, 8, 1, c);
        blob(p, 13, 26, 4, 6, 1, c);
    } else if (code >= 51) {                      // 毛毛雨 / 雨 / 阵雨
        cloud(p, 2, 0, DIM);
        drops(p, 2, 23, 3, (code <= 57) ? 5 : 8, 3, c);   // 毛毛雨的雨丝短一些
    } else {
        cloud(p, 2, 5, DIM);
    }
}

}  // namespace tdeck
