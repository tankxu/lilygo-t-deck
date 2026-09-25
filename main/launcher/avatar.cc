// avatar.cc — 眨眼、视线、眯眼
//
// 三条各自独立的定时线,周期取互质数且带随机抖动:
//
//   眨眼  2.6~5.4 秒   眼高 → 2 → 复原     闭 90ms / 睁 170ms
//   视线  1.8~4.2 秒   水平 ±(直径的 8%)   ease_out
//   眯眼  6~13 秒      变成竖长条,停 1.2~2.4 秒再复原
//
// 三条不同步是刻意的。人对规律重复极其敏感,一旦对齐就立刻从"活的"
// 塌成"循环播放的动画"。
//
// 睁开比闭上慢(90ms vs 170ms)也是同样的道理 —— 真实的眨眼就是不对称的,
// 做成对称的一眼假。

#include "avatar.h"

#include <esp_random.h>
#include <initializer_list>

namespace tdeck {
namespace {

constexpr uint32_t C_HEAD_TOP = 0x2E3A22;   // 头部渐变:上浅下深,做出球面感
constexpr uint32_t C_HEAD_BOT = 0x101509;
constexpr uint32_t C_EYE      = 0xA8E063;   // 亮黄绿,和军绿外壳同色族
constexpr uint32_t C_RING     = 0x7E9A5B;

int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

}  // namespace

void Avatar::create(lv_obj_t* parent, int cx, int cy, int d)
{
    d_      = d;
    base_w_ = d * 15 / 100;
    base_h_ = d * 30 / 100;
    bar_w_  = d * 7  / 100;      // 竖条:更窄更高
    bar_h_  = d * 40 / 100;
    eye_w_  = base_w_;
    eye_h_  = base_h_;
    eye_dx_ = d * 18 / 100;
    eye_cy_ = d / 2 - d / 40;    // 略高于几何中心,视觉上才居中

    head_ = lv_obj_create(parent);
    lv_obj_set_size(head_, d, d);
    lv_obj_set_pos(head_, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(head_, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(head_, lv_color_hex(C_HEAD_TOP), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_color(head_, lv_color_hex(C_HEAD_BOT), LV_PART_MAIN);
    lv_obj_set_style_bg_grad_dir(head_, LV_GRAD_DIR_VER, LV_PART_MAIN);
    lv_obj_set_style_border_width(head_, d > 100 ? 3 : 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(head_, lv_color_hex(C_RING), LV_PART_MAIN);
    lv_obj_set_style_border_opa(head_, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(head_, d / 5, LV_PART_MAIN);
    lv_obj_set_style_shadow_offset_y(head_, d / 20, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(head_, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_pad_all(head_, 0, LV_PART_MAIN);
    lv_obj_remove_flag(head_, LV_OBJ_FLAG_SCROLLABLE);

    // ⚠️ 这张脸有两个主人(首屏的小头像、小智应用里的大脸),而且小智在
    // Full / Overlay 之间切形态时会整个重建。对象随时可能被【别人】删掉:
    // 父容器被 lv_obj_clean、所在 screen 被 lv_obj_delete_delayed……
    // 那时 Avatar 自己一无所知,eye_l_ 还指着已经释放的对象,而三个定时器
    // 和动画还在跑,下一拍就往野指针上 lv_obj_set_size —— 崩在 LVGL 深处,
    // 回溯里看不出跟 Avatar 有关系。
    // 所以挂一个 DELETE 事件:谁删的都行,删了就地自清。
    lv_obj_add_event_cb(head_, [](lv_event_t* e) {
        auto* self = static_cast<Avatar*>(lv_event_get_user_data(e));
        lv_anim_delete(self, nullptr);
        for (auto** tm : { &self->t_blink_, &self->t_gaze_, &self->t_squint_ }) {
            if (*tm) { lv_timer_delete(*tm); *tm = nullptr; }
        }
        self->head_ = self->eye_l_ = self->eye_r_ = nullptr;
    }, LV_EVENT_DELETE, this);

    auto mk_eye = [&](int dx) {
        lv_obj_t* e = lv_obj_create(head_);
        lv_obj_set_style_bg_color(e, lv_color_hex(C_EYE), LV_PART_MAIN);
        lv_obj_set_style_border_width(e, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_color(e, lv_color_hex(C_EYE), LV_PART_MAIN);
        lv_obj_set_style_shadow_width(e, d / 8, LV_PART_MAIN);   // 轻微辉光
        lv_obj_set_style_shadow_opa(e, LV_OPA_50, LV_PART_MAIN);
        lv_obj_set_style_pad_all(e, 0, LV_PART_MAIN);
        lv_obj_remove_flag(e, LV_OBJ_FLAG_SCROLLABLE);
        (void)dx;
        return e;
    };
    eye_l_ = mk_eye(-eye_dx_);
    eye_r_ = mk_eye(+eye_dx_);
    apply_eye_geom();

    schedule_all();
}

// 眼睛的位置由尺寸反算,不单独存 —— 否则宽高一动位置就要跟着改三处
void Avatar::apply_eye_geom()
{
    if (!eye_l_) return;
    int r = eye_w_ / 2;
    for (auto e : { eye_l_, eye_r_ }) {
        lv_obj_set_size(e, eye_w_, eye_h_);
        lv_obj_set_style_radius(e, r, LV_PART_MAIN);
    }
    lv_obj_set_pos(eye_l_, d_ / 2 - eye_dx_ - eye_w_ / 2 + gaze_, eye_cy_ - eye_h_ / 2);
    lv_obj_set_pos(eye_r_, d_ / 2 + eye_dx_ - eye_w_ / 2 + gaze_, eye_cy_ - eye_h_ / 2);
}

void Avatar::destroy()
{
    // 动画要【先】删。三个 eye_*_cb 的 var 都是 this,定时器停了动画也还在跑,
    // 照样会回调进来改 eye_w_/eye_h_ 再去画已经删掉的对象。
    // lv_anim_delete(this, nullptr) 按 var 匹配,一次把这张脸的动画全清掉。
    lv_anim_delete(this, nullptr);
    for (auto** t : { &t_blink_, &t_gaze_, &t_squint_ }) {
        if (*t) { lv_timer_delete(*t); *t = nullptr; }
    }
    // head_ 上挂了 LV_EVENT_DELETE 回调,会把三个指针清掉,这里不用再清
    if (head_) lv_obj_delete(head_);
    head_ = eye_l_ = eye_r_ = nullptr;
}

void Avatar::eye_h_cb(void* var, int32_t v)
{
    auto* s = static_cast<Avatar*>(var);
    s->eye_h_ = v; s->apply_eye_geom();
}
void Avatar::eye_w_cb(void* var, int32_t v)
{
    auto* s = static_cast<Avatar*>(var);
    s->eye_w_ = v; s->apply_eye_geom();
}
void Avatar::eye_shift_cb(void* var, int32_t v)
{
    auto* s = static_cast<Avatar*>(var);
    s->gaze_ = v; s->apply_eye_geom();
}

void Avatar::on_blink(lv_timer_t* t)
{
    auto* s = static_cast<Avatar*>(lv_timer_get_user_data(t));
    if (!s->head_) return;   // 脸已经被删了,别再往野指针上起动画
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s);
    lv_anim_set_exec_cb(&a, Avatar::eye_h_cb);
    lv_anim_set_values(&a, s->eye_h_, 2);
    lv_anim_set_duration(&a, 90);
    lv_anim_set_playback_duration(&a, 170);
    lv_anim_start(&a);
    lv_timer_set_period(t, rnd(2600, 5400));
}

void Avatar::on_gaze(lv_timer_t* t)
{
    auto* s = static_cast<Avatar*>(lv_timer_get_user_data(t));
    if (!s->head_) return;   // 脸已经被删了,别再往野指针上起动画
    int amp = s->d_ * 8 / 100;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s);
    lv_anim_set_exec_cb(&a, Avatar::eye_shift_cb);
    lv_anim_set_values(&a, s->gaze_, rnd(-amp, amp));
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_timer_set_period(t, rnd(1800, 4200));
}

// 眯眼:两只眼同时收窄拉长,变成竖直的细条,停一会儿再复原。
// 这是整张脸最有"性格"的动作 —— 眨眼所有机器人都有,竖条是 Grok 那种
// 略带审视的神情,一眼就能认出来。
void Avatar::on_squint(lv_timer_t* t)
{
    auto* s = static_cast<Avatar*>(lv_timer_get_user_data(t));
    if (!s->head_) return;   // 脸已经被删了,别再往野指针上起动画
    s->in_bar_ = !s->in_bar_;

    int tw = s->in_bar_ ? s->bar_w_ : s->base_w_;
    int th = s->in_bar_ ? s->bar_h_ : s->base_h_;

    lv_anim_t aw, ah;
    lv_anim_init(&aw);
    lv_anim_set_var(&aw, s);
    lv_anim_set_exec_cb(&aw, Avatar::eye_w_cb);
    lv_anim_set_values(&aw, s->eye_w_, tw);
    lv_anim_set_duration(&aw, 200);
    lv_anim_set_path_cb(&aw, lv_anim_path_ease_in_out);
    lv_anim_start(&aw);

    lv_anim_init(&ah);
    lv_anim_set_var(&ah, s);
    lv_anim_set_exec_cb(&ah, Avatar::eye_h_cb);
    lv_anim_set_values(&ah, s->eye_h_, th);
    lv_anim_set_duration(&ah, 200);
    lv_anim_set_path_cb(&ah, lv_anim_path_ease_in_out);
    lv_anim_start(&ah);

    // 竖条只保持一两秒就复原;常态还是圆眼,否则"眯眼"就不是个动作了
    lv_timer_set_period(t, s->in_bar_ ? rnd(1200, 2400) : rnd(6000, 13000));
}

void Avatar::schedule_all()
{
    t_blink_  = lv_timer_create(&Avatar::on_blink,  rnd(2600, 5400),  this);
    t_gaze_   = lv_timer_create(&Avatar::on_gaze,   rnd(1800, 4200),  this);
    t_squint_ = lv_timer_create(&Avatar::on_squint, rnd(6000, 13000), this);
}

void Avatar::set_state(State s)
{
    state_ = s;
    if (!eye_l_) return;
    uint32_t col = (s == State::Listening) ? 0xFFFFFF     // 听:瞳孔发白,像睁大眼
                 : (s == State::Speaking)  ? 0xC6F08A     // 说:更亮的绿
                                           : C_EYE;
    lv_opa_t glow = (s == State::Idle) ? LV_OPA_50 : LV_OPA_COVER;
    for (lv_obj_t* e : { eye_l_, eye_r_ }) {
        lv_obj_set_style_bg_color(e, lv_color_hex(col), LV_PART_MAIN);
        lv_obj_set_style_shadow_opa(e, glow, LV_PART_MAIN);
    }
    // 听的时候不眯眼 —— 那是走神的表情,和"正在认真听"矛盾
    if (t_squint_) lv_timer_set_period(t_squint_, s == State::Idle ? rnd(6000, 13000) : 30000);
}

}  // namespace tdeck
