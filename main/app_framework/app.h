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

// app 自己的快捷键。
//
// ⚠️ app【不要】把快捷键画在自己的界面里 —— 屏幕就 320×240,
// 每个界面都留一行"esc back / y enter"是纯粹的浪费,而且各 app 各写一套
// 措辞,看起来也不像一个系统。统一的去处是按 s 弹出的那张表,
// launcher 会把这里返回的内容接在全局快捷键后面。
//
// 界面上该留的是【控件】(能点的返回按钮、播放键),不是【提示文字】。
struct Shortcut {
    const char* key;    // 例如 "esc" / "y" / "ball L/R"
    const char* desc;
};

class App {
public:
    virtual ~App() = default;

    // ── 身份 ──
    virtual const char* name() const = 0;
    virtual const char* icon() const = 0;   // LVGL symbol 或 storage 里的图标路径

    // 卡片的主题色,用于选中态描边
    virtual uint32_t    accent() const { return 0x55853A; }

    // 不出现在应用页的网格里。小智是常驻角色(ADR-004),入口是首屏头像和
    // 数字键 0,把它也塞进卡片网格会让"常驻"这件事在概念上自相矛盾。
    virtual bool hidden_from_grid() const { return false; }

    // 正在做文字输入,请把【原始按键】全都给我。
    //
    // 默认情况下 y/n/b/q/s/0 被 OS 全局占用(ADR-006),六个字母打不出来 ——
    // 搜索框里输不了 "yesterday"、"boy",WiFi 密码里也少六个字符,不能接受。
    // 声明了这个的 app 拿到全部按键,代价是【自己负责提供退路】:
    // ESC 和轨迹球中键仍然由 OS 保留,永远能退出去。
    virtual bool wants_raw_keys() const { return false; }

    // 当前界面生效的快捷键。按 s 的那一刻才调,所以可以随界面变化返回不同内容。
    // 返回条数,*out 指向一个静态数组。
    virtual int shortcuts(const Shortcut** out) const { (void)out; return 0; }

    // ── 卡片 ──
    // 桌面上的卡片是一个【内容整体】,不是"图标 + 标题 + 描述"的容器:
    // 音乐卡片就该是封面和正在播放什么,色板卡片就该是一片真实的色带,
    // 设置卡片就该是当前连着哪个 WiFi。卡片之间的区别来自内容本身,
    // 而不是来自图标的颜色 —— 后者只是把同一个模板换了个色。
    //
    // card 是一块 143x94、已经设好圆角和阴影的容器,app 往里画什么都行。
    // 默认实现是退化形式(图标 + 名称),只给还没做卡片的 app 兜底。
    // 卡片配图:143x94 的 RGB565 整幅图(tools/gen_card_art.py 生成)。
    //
    // 返回非空时 launcher 直接把它整幅贴进卡片,左下角叠一行白色标题,
    // 不再调 render_card()。这既是为了好看,也是为了快 ——
    // 实测四张卡片的矢量内容每帧要重画 75ms,而它们一动不动。
    virtual const lv_image_dsc_t* card_art() const { return nullptr; }

    // 卡片上显示的标题。name() 是给日志和身份判断用的全大写标识,
    // 不适合直接印在卡片上。
    virtual const char* card_title() const { return name(); }

    // 没有配图时的回退:自己在卡片上画。
    virtual void render_card(lv_obj_t* card);

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
void launcher_back();   // 回到桌面。app 画在界面上的返回按钮用这个

// 桌面数据注入。联网子系统拿到数据后调这两个,桌面不关心数据从哪来。
void launcher_set_time(const char* hhmm, const char* sub_line);
void launcher_set_weather(int wmo_code, float temp_c, float tmin_c, float tmax_c);
void launcher_set_online(bool online);

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
