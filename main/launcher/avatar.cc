// avatar.cc — 小智头像的眨眼与视线
//
// 两条各自独立的定时线:
//   眨眼  每 2.6~5.4 秒一次,眼高 22 → 2 → 22,来回共 260ms
//   视线  每 1.8~4.2 秒挪一次,水平 ±5px,用 ease_out 缓动
//
// 两条线周期互质且带随机抖动,所以不会同步成机械的节拍 —— 这正是"活"和
// "循环播放的动画"的区别所在。人对规律的重复极其敏感,一旦对齐就立刻显假。

#include "avatar.h"

#include <esp_random.h>
#include <initializer_list>

namespace tdeck {
namespace {

constexpr uint32_t C_HEAD_TOP = 0x30363d;   // 头部渐变:上浅下深,做出球面感
constexpr uint32_t C_HEAD_BOT = 0x14171a;
// 亮绿。军绿外壳 + 深色头像的组合下,这个色相既跳得出来又不脱离色系 ——
// 原来的青色是从蓝色主题带过来的,和橄榄绿放一起发冷、打架。
constexpr uint32_t C_EYE      = 0xA8E063;   // 亮黄绿
constexpr uint32_t C_RING     = 0x7E9A5B;   // 苔藓绿描边

int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

}  // namespace

void Avatar::create(lv_obj_t* parent, int cx, int cy, int d)
{
    eye_w_  = d * 11 / 72;
    eye_h_  = d * 22 / 72;
    eye_dx_ = d * 13 / 72;
    eye_cy_ = d / 2 - d * 2 / 72;   // 略高于几何中心,视觉上才是居中

    head_ = lv_obj_create(parent);
    lv_obj_set_size(head_, d, d);
    lv_obj_set_pos(head_, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(head_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(head_, lv_color_hex(C_HEAD_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(head_, lv_color_hex(C_HEAD_BOT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(head_, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(head_, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(head_, lv_color_hex(C_RING), LV_PART_MAIN);
    lv_obj_set_style_border_opa(head_, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(head_, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(head_, 3, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(head_, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(head_, 0, LV_PART_MAIN);
    lv_obj_set_scrollable(head_, false);

    auto mk_eye = [&](int dx) {
        lv_obj_t* e = lv_obj_create(head_);
        lv_obj_set_size(e, eye_w_, eye_h_);
        lv_obj_set_pos(e, d / 2 + dx - eye_w_ / 2, eye_cy_ - eye_h_ / 2);
        lv_obj_set_style_radius(e, eye_w_ / 2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(e, lv_color_hex(C_EYE), LV_PART_MAIN);
        lv_obj_set_style_border_width(e, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(e, lv_color_hex(C_EYE), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(e, 8, LV_PART_MAIN);   // 轻微辉光
        lv_obj_set_style_shadow_opa(e, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_pad_all(e, 0, LV_PART_MAIN);
        lv_obj_set_scrollable(e, false);
        return e;
    };
    eye_l_ = mk_eye(-eye_dx_);
    eye_r_ = mk_eye(+eye_dx_);

    schedule_blink();
    schedule_gaze();
}

void Avatar::destroy()
{
    if (blink_t_) { lv_timer_delete(blink_t_); blink_t_ = nullptr; }
    if (gaze_t_)  { lv_timer_delete(gaze_t_);  gaze_t_  = nullptr; }
    if (head_)    { lv_obj_delete(head_);      head_ = eye_l_ = eye_r_ = nullptr; }
}

// 眼高动画。缩放时同步下移一半差值,眼睛才是"闭合"而不是"从上边缘缩上去"。
void Avatar::eye_height_cb(void* var, int32_t v)
{
    auto* self = static_cast<Avatar*>(var);
    if (!self->eye_l_) return;
    int y = self->eye_cy_ - (int)v / 2;
    lv_obj_set_height(self->eye_l_, v);
    lv_obj_set_height(self->eye_r_, v);
    lv_obj_set_y(self->eye_l_, y);
    lv_obj_set_y(self->eye_r_, y);
}

void Avatar::eye_shift_cb(void* var, int32_t v)
{
    auto* self = static_cast<Avatar*>(var);
    if (!self->eye_l_) return;
    lv_obj_t* head = self->head_;
    int d = lv_obj_get_width(head);
    lv_obj_set_x(self->eye_l_, d / 2 - self->eye_dx_ - self->eye_w_ / 2 + v);
    lv_obj_set_x(self->eye_r_, d / 2 + self->eye_dx_ - self->eye_w_ / 2 + v);
}

void Avatar::on_blink_timer(lv_timer_t* t)
{
    auto* self = static_cast<Avatar*>(lv_timer_get_user_data(t));

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, self);
    lv_anim_set_exec_cb(&a, Avatar::eye_height_cb);
    lv_anim_set_values(&a, self->eye_h_, 2);
    lv_anim_set_duration(&a, 90);
    lv_anim_set_reverse_duration(&a, 170);   // 睁开比闭上慢,才像真的眨眼
    lv_anim_start(&a);

    // 下一次的间隔重新随机 —— 固定周期一眼就假
    lv_timer_set_period(t, rnd(2600, 5400));
}

void Avatar::on_gaze_timer(lv_timer_t* t)
{
    auto* self = static_cast<Avatar*>(lv_timer_get_user_data(t));

    int target = rnd(-5, 5);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, self);
    lv_anim_set_exec_cb(&a, Avatar::eye_shift_cb);
    lv_anim_set_values(&a, self->gaze_, target);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    self->gaze_ = target;

    lv_timer_set_period(t, rnd(1800, 4200));
}

void Avatar::schedule_blink()
{
    blink_t_ = lv_timer_create(&Avatar::on_blink_timer, rnd(2600, 5400), this);
}

void Avatar::schedule_gaze()
{
    gaze_t_ = lv_timer_create(&Avatar::on_gaze_timer, rnd(1800, 4200), this);
}

void Avatar::set_state(State s)
{
    state_ = s;
    if (!eye_l_) return;
    // 状态切换目前只改眼睛颜色和辉光强度;唤醒词、说话口型等留到小智接进来再做
    uint32_t col = (s == State::Listening) ? 0xffffff     // 听:瞳孔发白,像"睁大眼"
                 : (s == State::Speaking)  ? 0xC6F08A     // 说:更亮的绿
                                           : C_EYE;
    lv_opa_t glow = (s == State::Idle) ? LV_OPA_50 : LV_OPA_COVER;
    for (lv_obj_t* e : { eye_l_, eye_r_ }) {
        lv_obj_set_style_bg_color(e, lv_color_hex(col), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(e, glow, LV_PART_MAIN);
    }
}

}  // namespace tdeck
