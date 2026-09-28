// learning.h — 学习卡片 / 笔顺动画 / 公式展示
//
// 从 parent-master 的 stackchan(hal_learning.cpp + hal_formula.cpp)搬过来的。
// 整屏盖在 lv_layer_top 上,顶部一条倒计时条走完自动收,点一下也收。
//
// 三类内容,来源不同:
//
//   卡片   汉字/词语/英文单词的大图。【服务端预渲染】的 JPEG 320x240,
//          设备只负责下载和显示。为什么不在设备上画:任意汉字都可能超出
//          字库子集,而且拼音声调、IPA 音标、音节拆分在设备上排不出来。
//   笔顺   单个汉字的 GIF 动画 240x240,同样是预渲染的,交给 lv_gif。
//   公式   LaTeX 子集,【设备自己排】(formula.h)。中小学公式就分数/根号/
//          上下标/条件箭头几种结构,几百行布局够用 —— 模型现写现显示,
//          不用预渲染、不用等下载。
//
// 素材服务是 R2 桶的自有域名(CDN 直出,无鉴权):
//   GET <base>/card/<字或词>.jpg
//   GET <base>/stroke/<字>.gif
// 里面有 GB2312 全部 6763 字的卡与笔顺、6 级词库 5299 个英文词卡、
// 中文词频前 3 万的词卡。卡片 404(生僻词)时设备用自己的字体画一张简卡。
//
// 线程:show_* 由 MCP 回调线程调,只改状态;真正建控件在 LVGL 定时器里做
// (那里持着 LVGL 锁)。下载在自己的任务上跑。
// 每次请求带一个代号,下载完发现代号变了(用户又要了别的)就丢弃。

#pragma once

namespace tdeck {
namespace learning {

// 建定时器、准备好状态机。开机调一次(main.cc),要持 LVGL 锁。
void begin();

// 把 self.learning.* 挂进小智的 MCP。在 Application 启动【之前】调 ——
// McpServer 是单例,提前 AddTool 就行,不用改 xiaozhi 组件;
// 顺带占住 tools/list 的前排(那边按字节分页,排后面有被漏掉的风险)。
void register_mcp();

// meaning:中文释义,只对英文词有意义。
// 服务端渲染的卡【没有翻译】(_render_learning_card 对英文只排 IPA + 音节),
// 重渲染 5299 张英文卡再重传不划算,所以这一行由设备画在卡片的空白带上。
void show_card(const char* text, const char* guide, const char* meaning);
void show_stroke_order(const char* character);                // 单字笔顺 GIF
void show_page(const char* title, const char* body);          // 任意文字页(古诗/释义/组词)
void show_formula(const char* title, const char* latex, const char* note);

void clear();
bool visible();

}  // namespace learning
}  // namespace tdeck
