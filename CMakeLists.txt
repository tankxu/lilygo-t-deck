cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "components")

# ⚠️ 必须显式设版本号,而且必须是 x.y.z 的形式。
#
# 不设的话 ESP-IDF 会拿 git describe 的结果当 app version,在这个仓库里
# 就是一串 commit hash(如 "cad86d6")。小智的 OTA 检查会对它做
#   std::stoi(segment)   —— components/xiaozhi/src/ota.cc, ParseVersion()
# hash 解不成整数,抛 std::invalid_argument,没人 catch,直接 abort ——
# 于是【每次连上 WiFi 就重启一次】,而且现场在 C++ 异常机制里,
# 完全看不出跟版本号有关系。
set(PROJECT_VER "1.0.0")

# ── 构建变体 ────────────────────────────────────────────────
#
#   daily —— 平时那套:全部 app,蓝牙关掉
#   car   —— Apple Music 控制器:蓝牙开,不编 music/bilibili/小智
#
# 为什么要分变体而不是一个固件全包:CONFIG_BT_ENABLED=y 一开,控制器就
# 【无条件】占掉 54KB 内部 DRAM —— 不是 nimble_port_init() 申请的,是进
# .bss/IRAM 的,运行时关不掉。实测开着 BT 进音乐 app,内部堆最低点掉到
# 23 字节,专辑封面全部加载失败。这 54KB 不能让日常固件一起付。
#
# ⚠️ 车载变体【仍然编 WiFi,而且照常开着】。
# 一开始以为 BLE 和 WiFi 二选一(日常固件上 WiFi 开着只剩 19KB,
# nimble_port_init 直接失败),换到车载固件上实测是共存的:
# 开机 79,491,起完 BLE 还剩 30,483,WiFi 照常响应 HTTP。
# 省出来的不是蓝牙让的,是砍掉小智/B站/音乐让的。
#
# 砍掉 WiFi 还会连带砍掉调试服务和 OTA,开发闭环就断了;而且
# CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 要求新固件自己声明可用,那个声明点
# 就在 debug_server 启动处 —— 起不来 WiFi 的固件会被 bootloader 回滚掉。
#
# 切变体用 tools/build.sh,它给每个变体独立的 build 目录和 sdkconfig。
#
# ⚠️ 变体走【环境变量】,不是 -D。
# ESP-IDF 收集组件 REQUIRES 是在另一个 CMake 进程里跑的
# (component.cmake 的 execute_process + component_get_requirements.cmake),
# 只传了它自己那几个 -D —— 顶层用 -D 或 CACHE 定的变量到不了那边。
# 环境变量会被子进程继承,所以它是唯一在两遍里都可见的通道。详见 main/CMakeLists.txt。
if(DEFINED ENV{TDECK_VARIANT})
    set(TDECK_VARIANT "$ENV{TDECK_VARIANT}")
else()
    set(TDECK_VARIANT "daily")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 变体写进 project_name,而【不是】version —— version 必须保持 x.y.z(见上)。
# esp_ota_get_partition_description() 读得到另一个槽的 project_name,
# "另一个槽里装的是哪个变体" 就靠它判断。
if(TDECK_VARIANT STREQUAL "car")
    project(tdeck-os-car)
else()
    project(tdeck-os)
endif()
