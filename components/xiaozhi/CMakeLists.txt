# 小智核心 —— 只搬子系统,不继承上游的构建体系
#
# 上游 main/CMakeLists.txt 有 1206 行、252 个条件分支,外加字体生成、语音资源
# 打包、200 块板子的选择逻辑。整个继承过来意味着每次跟上游同步都要重做一遍。
#
# 这里只要四块:audio(Opus + AFE)、protocols(WebSocket/MQTT)、
# application(状态机)、mcp_server。板级和显示由 launcher 的 tdeck_bsp 提供,
# 上游那套 Board/Display 抽象只保留基类,具体实现我们自己写。
file(GLOB_RECURSE XZ_AUDIO    "src/audio/*.cc")
file(GLOB_RECURSE XZ_PROTOCOL "src/protocols/*.cc")

# T-Deck 板级适配 —— 我们自己写的那一半(Board / AudioCodec / Display / 启动入口)。
# 单独一个目录、单独一个变量,是为了跟上游代码泾渭分明:
# 跟上游同步时 src/ 下的东西可以整体覆盖,boards/t-deck/ 一个字都不用动。
file(GLOB XZ_BOARD "src/boards/t-deck/*.cc")

idf_component_register(
    SRCS
        "src/application.cc"
        "src/mcp_server.cc"
        "src/settings.cc"
        "src/system_info.cc"
        "src/ota.cc"
        "src/device_state_machine.cc"
        "src/boards/common/board.cc"
        "src/display/display.cc"
        "src/backlight.cc"
        ${XZ_AUDIO}
        ${XZ_PROTOCOL}
        ${XZ_BOARD}
    INCLUDE_DIRS
        # 注意:这里【不要】写不存在的目录。src/net 曾经在这一行里,但它是个空目录,
        # git 不跟踪空目录 —— 新克隆出来 configure 直接失败:
        #   CMake Error: Include directory '.../src/net' is not a directory
        # 上游把 http/websocket/mqtt 抽象放在 esp-ml307 组件里,这里本来就不需要它。
        "src" "src/led" "src/audio" "src/protocols" "src/display" "src/boards/common" "src/assets"
        "src/boards/t-deck"
    REQUIRES
        tdeck_bsp
        esp_timer esp_event nvs_flash esp_wifi esp_netif
        esp_http_client esp-tls mqtt json driver
        esp_app_format spi_flash app_update efuse
        78__esp-ml307 espressif__esp-sr espressif__esp_audio_effects
        lvgl__lvgl espressif__esp_lvgl_port esp_lcd
        espressif__esp_audio_codec espressif__esp_codec_dev
    # 提示音。上游把它们放在 assets 分区里(和字体、唤醒词模型一起),
    # 本工程的分区表没有 assets 分区(ADR-005),而 lang_config.h 里的
    # Lang::Sounds::OGG_* 是【每个 include 它的 .cc 都会引用】的
    # _binary_*_ogg_start 符号 —— 少一个就是一堆链接错误。总共 96KB,直接进固件。
    EMBED_FILES
        "assets/sounds/0.ogg" "assets/sounds/1.ogg" "assets/sounds/2.ogg"
        "assets/sounds/3.ogg" "assets/sounds/4.ogg" "assets/sounds/5.ogg"
        "assets/sounds/6.ogg" "assets/sounds/7.ogg" "assets/sounds/8.ogg"
        "assets/sounds/9.ogg"
        "assets/sounds/activation.ogg" "assets/sounds/err_pin.ogg"
        "assets/sounds/err_reg.ogg" "assets/sounds/exclamation.ogg"
        "assets/sounds/low_battery.ogg" "assets/sounds/popup.ogg"
        "assets/sounds/success.ogg" "assets/sounds/upgrade.ogg"
        "assets/sounds/vibration.ogg" "assets/sounds/welcome.ogg"
        "assets/sounds/wificonfig.ogg"
)

# 上游这些宏是 main/CMakeLists.txt 按所选板子注入的。我们不继承那套逻辑,
# 所以在这里直接给死 —— 这个组件只服务 T-Deck 一块板。
#
# 注意必须放在 idf_component_register 之后用 target_compile_definitions:
# 组件 CMakeLists 会被 IDF 先跑一遍"只收集依赖"的模式,那个模式下
# add_compile_definitions 这类命令根本不存在,会直接报 Unknown CMake command。
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    BOARD_NAME="lilygo-t-deck"
    BOARD_TYPE="t-deck"
    # 小智官方的版本检查 / 激活服务。设备第一次连上会在这里拿到激活码和
    # WebSocket 地址,再写进 NVS —— 所以不需要在 secrets.h 里填服务器。
    # 想换自建服务端:改这里,或者往 NVS 的 wifi/ota_url 写一条。
    CONFIG_OTA_URL="https://api.tenclass.net/xiaozhi/ota/"
)
