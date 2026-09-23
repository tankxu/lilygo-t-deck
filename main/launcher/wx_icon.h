#pragma once
#include <lvgl.h>
namespace tdeck {
// 按 WMO 天气代码在 parent 上画图标(占 34x30)。重复调用会先清空 parent。
void wx_icon_build(lv_obj_t* parent, int wmo_code, uint32_t color);
// WMO 代码 → 英文短描述
const char* wx_text(int wmo_code);
}
