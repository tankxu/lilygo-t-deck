// assets_stub.cc — Assets 的空实现
//
// 上游的 assets.cc 用一个 spiffs "assets" 分区存中文字体、表情图、提示音和
// esp-sr 唤醒词模型,并且依赖 lcd_display.h / emote_display.h / expression_emote.h
// ——那三套 UI 实现都不在本工程里(我们自己画界面),所以那个文件编不过。
//
// 而分区表(ADR-005)里也没有 assets 分区:字体由 OS 的 main/ui/fonts.cc 管,
// 提示音直接 EMBED 进固件,表情是自己画的 avatar。也就是说这套资源机制
// 在这里【整个不需要】。
//
// 内核里只有两处用到它,而且都先问 partition_valid():
//   application.cc:351  CheckAssetsVersion()
//   mcp_server.cc:289   上报资源版本
// 所以给一个"永远没有资源分区"的实现就够了,两处都会安静地跳过。
// 将来真要上唤醒词模型,再把上游的 assets.cc 按需要搬回来。

#include "assets.h"

Assets::Assets() {
    partition_valid_ = false;
}

Assets::~Assets() = default;

bool Assets::Download(std::string url, std::function<void(int, size_t)> progress_callback) {
    (void)url; (void)progress_callback;
    return false;
}

bool Assets::Apply(bool refresh_display_theme) {
    (void)refresh_display_theme;
    return false;
}

bool Assets::GetAssetData(const std::string& name, void*& ptr, size_t& size) {
    (void)name; ptr = nullptr; size = 0;
    return false;
}
