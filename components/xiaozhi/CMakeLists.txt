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

idf_component_register(
    SRCS
        "src/application.cc"
        "src/mcp_server.cc"
        "src/settings.cc"
        "src/system_info.cc"
        ${XZ_AUDIO}
        ${XZ_PROTOCOL}
    INCLUDE_DIRS
        "src" "src/led" "src/audio" "src/protocols" "src/display" "src/boards/common" "src/net" "src/assets"
    REQUIRES
        tdeck_bsp
        esp_timer esp_event nvs_flash esp_wifi esp_netif
        esp_http_client esp-tls mqtt json driver
        esp_app_format spi_flash app_update
        78__esp-ml307 espressif__esp-sr espressif__esp_audio_effects
        lvgl__lvgl espressif__esp_lvgl_port esp_lcd
        espressif__esp_audio_codec espressif__esp_codec_dev
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
)
