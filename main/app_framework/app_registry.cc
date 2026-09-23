// app_registry.cc — App 静态注册表(ADR-003)
//
// 每个 app 在自己的 .cc 末尾用 TDECK_REGISTER_APP(Cls) 注册,
// launcher 只遍历这张表,不需要 #include 任何具体 app。

#include "app.h"
#include <lvgl.h>

namespace tdeck {

// 兜底卡片:居中的图标 + 名称。
// 任何一个 app 只要自己实现了 render_card,就不会走到这里 ——
// 这个实现存在的意义是"新 app 没画卡片时也不至于是个空白框"。
void App::render_card(lv_obj_t* card)
{
    lv_obj_t* ic = lv_label_create(card);
    lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(ic, lv_color_hex(accent()), LV_PART_MAIN);
    lv_label_set_text(ic, icon());
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -10);

    lv_obj_t* nm = lv_label_create(card);
    lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(nm, lv_color_hex(0x1b2117), LV_PART_MAIN);
    lv_label_set_text(nm, name());
    lv_obj_align(nm, LV_ALIGN_CENTER, 0, 22);
}

AppRegistry& AppRegistry::instance()
{
    // 函数内 static:保证在第一个 registrar 的构造函数调用它时已经初始化好,
    // 不受全局静态对象初始化顺序的影响。
    static AppRegistry inst;
    return inst;
}

void AppRegistry::add(App* app)
{
    if (app) apps_.push_back(app);
}

}  // namespace tdeck
