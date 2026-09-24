// xiaozhi_core.cc — 内核的启动与唤醒入口
//
// 上游是 app_main() 里 Initialize() + Run(),Run() 永不返回、独占 main task。
// 在这个 OS 里小智只是常驻后台的一员(ADR-004),所以把这两句搬进自己的任务。
// 行为一字不变:Run() 本体是 xEventGroupWaitBits(portMAX_DELAY),空闲不占 CPU。

#include "xiaozhi_core.h"
#include "application.h"
#include "xz_display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "xz"

XzDisplay* tdeck_xz_display();   // 定义在 tdeck_board.cc

namespace xz {

static bool      s_started = false;
static Callbacks s_cb;

void set_callbacks(Callbacks cb) {
    s_cb = std::move(cb);
    tdeck_xz_display()->SetCallbacks(s_cb);
}

static void XiaozhiTask(void*) {
    auto& app = Application::GetInstance();
    app.Initialize();   // 音频服务 + MCP + 网络回调,内部会起 3 个音频任务
    app.Run();          // 事件循环,永不返回
}

bool start() {
    if (s_started) return false;

    // 栈必须在内部 RAM(FreeRTOS 任务栈不能放 PSRAM),而内部 RAM 是这块板子上
    // 最紧的资源,所以开机时把余量打出来 —— 起不来的时候第一眼就能看见原因。
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // 12KB:Initialize() 里 OTA 检查会拉 HTTPS(mbedtls 握手吃栈),
    // 加上 Display 回调里会走进 LVGL,8KB(上游 main task 的值)偏紧。
    TaskHandle_t h = nullptr;
    auto ok = xTaskCreate([](void* a) { XiaozhiTask(a); }, "xiaozhi_main",
                          12 * 1024, nullptr, 5, &h);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "小智主任务建不起来,内部 RAM 只剩 %u 字节", (unsigned)before);
        return false;
    }
    s_started = true;
    ESP_LOGI(TAG, "小智内核启动,内部 RAM %u → %u 字节",
             (unsigned)before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

bool started() { return s_started; }

State state() {
    if (!s_started) return State::Unknown;
    switch (Application::GetInstance().GetDeviceState()) {
    case kDeviceStateStarting:        return State::Starting;
    case kDeviceStateWifiConfiguring: return State::WifiConfiguring;
    case kDeviceStateIdle:            return State::Idle;
    case kDeviceStateConnecting:      return State::Connecting;
    case kDeviceStateListening:       return State::Listening;
    case kDeviceStateSpeaking:        return State::Speaking;
    case kDeviceStateUpgrading:       return State::Upgrading;
    case kDeviceStateActivating:      return State::Activating;
    case kDeviceStateAudioTesting:    return State::AudioTesting;
    case kDeviceStateFatalError:      return State::FatalError;
    default:                          return State::Unknown;
    }
}

const char* state_text(State s) {
    switch (s) {
    case State::Starting:        return "启动中";
    case State::WifiConfiguring: return "配网中";
    case State::Idle:            return "待命";
    case State::Connecting:      return "连接中";
    case State::Listening:       return "聆听中";
    case State::Speaking:        return "说话中";
    case State::Upgrading:       return "升级中";
    case State::Activating:      return "激活中";
    case State::AudioTesting:    return "音频自检";
    case State::FatalError:      return "出错了";
    default:                     return "";
    }
}

// 下面四个都只是往 EventGroup 置位(上游注释明写 thread-safe),
// 所以可以直接在 LVGL 任务或输入任务里调,不会阻塞界面。
void toggle_chat() {
    if (!s_started) return;
    Application::GetInstance().ToggleChatState();
}

void start_listening() {
    if (!s_started) return;
    Application::GetInstance().StartListening();
}

void stop_listening() {
    if (!s_started) return;
    Application::GetInstance().StopListening();
}

void abort_speaking() {
    if (!s_started) return;
    Application::GetInstance().AbortSpeaking(kAbortReasonNone);
}

}  // namespace xz
