idf_component_register(
    SRCS
        "main.cc"
        "app_framework/app_registry.cc"
        "launcher/launcher.cc"
        "launcher/avatar.cc"
        "launcher/wx_icon.cc"
        "net/net.cc"
        "sys/volume.cc"
        "debug/debug_server.cc"
        "ui/fonts.cc"
        "ui/cbin_font.c"
        "apps/palette/palette_app.cc"
        "apps/settings/settings_app.cc"
        "apps/music/music_app.cc"
        "apps/xiaozhi/xiaozhi_app.cc"
    INCLUDE_DIRS
        "." "app_framework"
    REQUIRES
        tdeck_bsp
        esp_wifi esp_netif esp_event nvs_flash
        esp_http_client esp_http_server esp-tls json esp_timer
    # ⚠️ 必须 WHOLE_ARCHIVE。
    # app 靠文件末尾的 TDECK_REGISTER_APP 静态对象自注册,没有任何代码显式引用它。
    # 组件被编成静态库,链接器默认只拉取「被引用到」的 .o —— app 的 .o 会被整个丢掉,
    # 注册表里空空如也,而且不报任何错。
    EMBED_FILES "assets/wallpaper.rgb565" "assets/font_puhui_common_20_4.bin"
    WHOLE_ARCHIVE
)
