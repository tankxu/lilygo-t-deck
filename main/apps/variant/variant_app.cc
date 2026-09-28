// variant_app.cc — 在两个固件变体之间来回切
//
// 切换的实现就是 esp_ota_set_boot_partition(另一个槽) + 重启,不写 flash。
// 但这个 app 存在的主要理由不是那两行代码,是【让人看懂现在处在哪一边】:
//   · 当前跑的是哪个变体
//   · 另一个槽里装的是什么(可能是空的,也可能装着同一个变体)
//   · 切过去会发生什么(重启,大约 3 秒)
//
// 不做二次确认弹窗 —— 切错了再按一次就回来了,代价是 3 秒,不值得为它加一层 UI。

#include "app.h"
#include "ui/fonts.h"
#include "sys/variant.h"

#include <lvgl.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr uint32_t C_BG    = 0xf6f8fa;
constexpr uint32_t C_CARD  = 0xffffff;
constexpr uint32_t C_TEXT  = 0x1f2328;
constexpr uint32_t C_MUTE  = 0x57606a;
constexpr uint32_t C_OK    = 0x1F6FEB;
constexpr uint32_t C_WARN  = 0xB35900;

// project_name → 给人看的名字
const char* pretty(const char* project)
{
    if (!project || !project[0])                return "(空)";
    if (strcmp(project, "tdeck-os-car") == 0)   return "车载模式";
    if (strcmp(project, "tdeck-os") == 0)       return "日常模式";
    return project;
}

class VariantApp : public tdeck::App {
public:
    const char* name() const override        { return "VARIANT"; }
    const char* icon() const override        { return LV_SYMBOL_SHUFFLE; }
    uint32_t    card_color() const override  { return 0x334155; }
    const char* card_title() const override
    {
        // 卡片上写的是【去哪儿】,不是【在哪儿】—— 按钮该说自己会做什么
        return tdeck::variant_is_car() ? "回日常" : "车载模式";
    }

    void on_enter(lv_obj_t* root) override
    {
        lv_obj_set_style_bg_color(root, lv_color_hex(C_BG), LV_PART_MAIN);
        lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

        // ⚠️ 字体要在 root 上设一次 —— LVGL 的文本样式会向下继承。
        // 不设的话 lv_label_create 用的是 LVGL 默认的 Montserrat(只有 ASCII),
        // 中文全变豆腐块,而且不报任何错。
        const lv_font_t* f = tdeck::font_cjk_small();
        if (f) lv_obj_set_style_text_font(root, f, LV_PART_MAIN);

        char other[32] = {};
        bool has_other = tdeck::variant_other_slot(other, sizeof(other));
        bool same      = has_other && strcmp(other, tdeck::variant_name()) == 0;
        can_switch_    = has_other && !same;

        lv_obj_t* t = lv_label_create(root);
        lv_label_set_text(t, "固件变体");
        lv_obj_set_style_text_color(t, lv_color_hex(C_TEXT), LV_PART_MAIN);
        lv_obj_set_pos(t, 16, 12);

        row(root, 44,  "正在运行", pretty(tdeck::variant_name()), C_TEXT);
        row(root, 88,  "另一个槽", pretty(other),                 has_other ? C_TEXT : C_MUTE);

        lv_obj_t* hint = lv_label_create(root);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(hint, 288);
        lv_obj_set_pos(hint, 16, 136);
        lv_obj_set_style_text_color(hint, lv_color_hex(can_switch_ ? C_MUTE : C_WARN),
                                    LV_PART_MAIN);
        if (can_switch_) {
            lv_label_set_text(hint, "按回车切过去。改的只是启动分区,不写 flash,"
                                    "重启大约 3 秒。");
        } else if (same) {
            lv_label_set_text(hint, "两个槽装的是同一个变体。先构建另一个变体再推上来:"
                                    "tools/build.sh car");
        } else {
            lv_label_set_text(hint, "另一个槽是空的。先构建另一个变体再推上来:"
                                    "tools/build.sh car");
        }

        btn_ = lv_obj_create(root);
        lv_obj_set_size(btn_, 288, 40);
        lv_obj_set_pos(btn_, 16, 190);
        lv_obj_set_style_radius(btn_, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn_, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(btn_, lv_color_hex(can_switch_ ? C_OK : C_MUTE),
                                  LV_PART_MAIN);
        lv_obj_set_style_bg_opa(btn_, can_switch_ ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
        lv_obj_remove_flag(btn_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(btn_, [](lv_event_t* e) {
            static_cast<VariantApp*>(lv_event_get_user_data(e))->go();
        }, LV_EVENT_CLICKED, this);

        lv_obj_t* bl = lv_label_create(btn_);
        lv_label_set_text_fmt(bl, "切到%s",
                              pretty(has_other ? other : "(空)"));
        lv_obj_set_style_text_color(bl, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(bl);
    }

    void on_exit() override { btn_ = nullptr; }

    bool on_input(const tdeck::InputEvent& ev) override
    {
        if (ev.key == tdeck::Key::Enter) { go(); return true; }
        return false;
    }

private:
    // panel() 这种装饰性容器必须摘掉 CLICKABLE。
    // lv_obj_create 建出来的对象【默认可点】,盖在按钮上会把点击吃掉 ——
    // 这个坑在 music/settings/bilibili 里各踩过一次。
    void row(lv_obj_t* root, int y, const char* k, const char* v, uint32_t vc)
    {
        lv_obj_t* p = lv_obj_create(root);
        lv_obj_set_size(p, 288, 36);
        lv_obj_set_pos(p, 16, y);
        lv_obj_set_style_radius(p, 8, LV_PART_MAIN);
        lv_obj_set_style_border_width(p, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_color(p, lv_color_hex(C_CARD), LV_PART_MAIN);
        lv_obj_set_style_pad_all(p, 0, LV_PART_MAIN);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* a = lv_label_create(p);
        lv_label_set_text(a, k);
        lv_obj_set_style_text_color(a, lv_color_hex(C_MUTE), LV_PART_MAIN);
        lv_obj_align(a, LV_ALIGN_LEFT_MID, 12, 0);

        lv_obj_t* b = lv_label_create(p);
        lv_label_set_text(b, v);
        lv_obj_set_style_text_color(b, lv_color_hex(vc), LV_PART_MAIN);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, -12, 0);
    }

    void go()
    {
        if (!can_switch_) return;
        // 失败了不会重启,函数正常返回 —— 日志里有原因,界面保持原样
        tdeck::variant_switch_reboot();
    }

    lv_obj_t* btn_ = nullptr;
    bool      can_switch_ = false;
};

}  // namespace

TDECK_REGISTER_APP(VariantApp)
