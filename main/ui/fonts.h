#pragma once
#include <lvgl.h>
namespace tdeck {
// 中文字体。开机时从内嵌的二进制字体建一次,全局共用。
// 没建起来时返回 nullptr,调用方回落到 Montserrat(只有 ASCII)。
void        fonts_init();
const lv_font_t* font_cjk();      // 20px,约 9000 字
}
