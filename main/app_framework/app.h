// app.h — App 插口(ADR-003 / ADR-004)
//
// 所有应用编译进同一个固件,实现这个接口,静态注册到 AppRegistry。
// launcher 遍历注册表画图标网格;切换 app 就是 on_exit/on_enter + screen 切换,
// 不重启、不动 flash(这正是不套解释器、不切分区的意义所在)。

#pragma once

#include <lvgl.h>
#include <vector>

namespace tdeck {

// 归一化后的输入事件。BSP 把键盘(I2C 0x55)、轨迹球(4 路方向 GPIO + 中键)
// 统一成这一种事件递给 app,app 不需要知道它来自哪个物理器件。
enum class Key {
    None,
    Up, Down, Left, Right,   // 轨迹球方向,或键盘方向键
    Enter,                   // 轨迹球中键短按
    Back,                    // ESC
    Char,                    // 可打印字符,取 InputEvent::ch
};

struct InputEvent {
    Key  key = Key::None;
    char ch  = 0;      // key == Key::Char 时有效
    bool long_press = false;
};

class App {
public:
    virtual ~App() = default;

    // ── 身份 ──
    virtual const char* name() const = 0;
    virtual const char* icon() const = 0;   // LVGL symbol 或 storage 里的图标路径

    // ── 生命周期 ──
    // launcher 递一块干净的 screen 过来,app 在上面建自己的控件树。
    virtual void on_enter(lv_obj_t* root) = 0;

    // 必须还干净:on_enter 里申请的任务/内存/外设,这里全部释放。
    // RAM 是所有 app 共享的,漏一次连开几个就 OOM(ADR-003)。
    virtual void on_exit() = 0;

    // ── 输入 ──
    // 返回 true 表示已消费;返回 false 让 launcher 处理(比如 Back 退回桌面)。
    virtual bool on_input(const InputEvent& ev) = 0;

    // ── 后台常驻(ADR-004)──
    // 默认 false:切走就 on_exit 彻底释放。
    // 小智返回 true:连接、音频线程、唤醒检测继续活着,只收起 UI。
    virtual bool wants_background() const { return false; }
    virtual void on_background() {}   // UI 收起,后台继续跑
    virtual void on_foreground() {}   // 重新浮出
};

// 静态注册表。app 在自己的 .cc 里用 TDECK_REGISTER_APP 挂进来,
// launcher 不需要 #include 任何一个具体 app。
class AppRegistry {
public:
    static AppRegistry& instance();
    void add(App* app);
    const std::vector<App*>& apps() const { return apps_; }

private:
    std::vector<App*> apps_;
};

// 启动桌面:订阅输入、显示应用列表。由 app_main 调用。
void launcher_begin();

}  // namespace tdeck

// 用法:文件末尾写 TDECK_REGISTER_APP(SelfTestApp);
#define TDECK_REGISTER_APP(CLS)                                   \
    namespace {                                                   \
    struct CLS##_Registrar {                                       \
        CLS##_Registrar() {                                        \
            ::tdeck::AppRegistry::instance().add(new CLS());       \
        }                                                          \
    };                                                             \
    CLS##_Registrar g_##CLS##_registrar;                           \
    }
