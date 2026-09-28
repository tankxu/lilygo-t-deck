# 两个构建变体共用这一份(见顶层 CMakeLists 的说明)。
#
#   daily —— 全部 app,蓝牙关
#   car   —— Apple Music 控制器,蓝牙开,不编 music / bilibili / 小智
#
# 车载变体砍掉那三个 app 不是因为它们没用,是因为 BT 占掉 54KB 内部 DRAM 之后,
# 它们的内存预算就不成立了(实测音乐 app 会把内部堆压到 23 字节)。
#
# ⚠️ 变体只能从【环境变量】读,不能用 -D,也不能用顶层的 CMake 变量。
#
# ESP-IDF 处理组件是两遍:第一遍只为了收集 REQUIRES,而它是在【另一个 CMake
# 进程】里跑的(tools/cmake/component.cmake 的 execute_process ->
# component_get_requirements.cmake),那个进程只收到它自己的几个 -D。
# 所以第二遍(进程内,算 SRCS)看得见的变量,第一遍看不见。
#
# 踩过的现象:用 -D TDECK_VARIANT=car 时,applemusic_app.cc 确实编进去了
# (第二遍对了),但 bt 没进 REQUIRES(第一遍走了 else),结果是
# nimble/nimble_port.h 找不到 —— 两遍看到的是两个世界,错得很不直观。
# 顺带,CONFIG_* 在第一遍里也不可见,拿它当判据同样失败。
if("$ENV{TDECK_VARIANT}" STREQUAL "car")
    set(TDECK_CAR 1)
else()
    set(TDECK_CAR 0)
endif()

# 和 sdkconfig 对一下。CONFIG_* 只在第二遍有值,所以这个校验只在第二遍生效 ——
# 够用了:它要防的是"带着蓝牙却自称 tdeck-os"这种组合,那样切槽逻辑
# 就认不出自己是哪个变体了。
if(DEFINED CONFIG_BT_NIMBLE_ENABLED OR DEFINED CONFIG_IDF_TARGET)
    if(TDECK_CAR AND NOT CONFIG_BT_NIMBLE_ENABLED)
        message(FATAL_ERROR "变体是 car 但 sdkconfig 没开 NimBLE —— 请用 tools/build.sh car")
    elseif(NOT TDECK_CAR AND CONFIG_BT_NIMBLE_ENABLED)
        message(FATAL_ERROR "sdkconfig 开了 NimBLE 但变体不是 car —— 请用 tools/build.sh car")
    endif()
endif()

set(TDECK_SRCS
    "main.cc"
    "app_framework/app_registry.cc"
    "launcher/launcher.cc"
    "launcher/avatar.cc"
    "launcher/wx_icon.cc"
    "net/net.cc"
    "sys/volume.cc"
    "sys/variant.cc"
    "debug/debug_server.cc"
    "debug/ble_probe.cc"
    "ui/fonts.cc"
    "ui/cbin_font.c"
    "apps/palette/palette_app.cc"
    "apps/settings/settings_app.cc"
    "apps/selftest/selftest_app.cc"
    "apps/variant/variant_app.cc"    # 两个变体里都要有,否则切过去就回不来了
)

set(TDECK_REQUIRES
    tdeck_bsp
    esp_wifi esp_netif esp_event nvs_flash app_update
    esp_http_client esp_http_server esp-tls json esp_timer
)

if(TDECK_CAR)
    list(APPEND TDECK_SRCS
        "apps/applemusic/applemusic_app.cc"
        "apps/applemusic/ams_client.cc"
        "apps/applemusic/hid_service.cc"
    )
    list(APPEND TDECK_REQUIRES bt)
else()
    list(APPEND TDECK_SRCS
        "apps/music/music_app.cc"
        "apps/xiaozhi/xiaozhi_app.cc"
        "apps/bilibili/bili_app.cc"
        "apps/bilibili/bili_client.cc"
        # 学习卡片/笔顺/公式(从 parent-master 的 stackchan 搬)。
        # 只在 daily:它靠小智的 MCP 驱动,车载变体没有小智。
        "learning/learning.cc"
        "learning/learning_mcp.cc"
        "learning/formula.cc"
        "assets/fonts/PuHuiMath28.c"          # 公式正文 28px
        "assets/fonts/IpaSans20.c"            # 英文简卡的 IPA 音标
        "assets/fonts/MontserratSemiBold26.c" # 英文简卡的单词
    )
    # 小智内核(apps/xiaozhi 通过 xiaozhi_core.h 对接)。
    # 它会连带拉进 esp-sr / opus / 音频编解码,车载变体不要。
    list(APPEND TDECK_REQUIRES xiaozhi)
endif()

idf_component_register(
    SRCS ${TDECK_SRCS}
    INCLUDE_DIRS
        "." "app_framework" "learning"
    REQUIRES ${TDECK_REQUIRES}
    # ⚠️ 必须 WHOLE_ARCHIVE。
    # app 靠文件末尾的 TDECK_REGISTER_APP 静态对象自注册,没有任何代码显式引用它。
    # 组件被编成静态库,链接器默认只拉取「被引用到」的 .o —— app 的 .o 会被整个丢掉,
    # 注册表里空空如也,而且不报任何错。
    EMBED_FILES "assets/wallpaper.rgb565" "assets/font_puhui_common_20_4.bin"
               "assets/font_puhui_common_16_4.bin"
               "assets/font_noto_qwen_14_1.bin"
    WHOLE_ARCHIVE
)

# 学习卡片只在 daily 变体里存在(靠小智的 MCP 驱动),
# main.cc 和 debug_server 是两个变体共用的,靠这个宏取舍。
if(NOT TDECK_CAR)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC TDECK_LEARNING=1)
endif()
