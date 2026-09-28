// learning_mcp.cc — 把学习卡片挂成小智的 MCP 工具
//
// 工具描述就是给模型看的说明书,写法直接决定它会不会在对的时候调、
// 传对的参数。这几段描述是从 parent-master 搬来的,那边已经按真实误用
// 反复改过 —— 比如不写清楚的话,模型问"这个字怎么写"也会去调静态汉字卡,
// 而不是笔顺动画。照搬,别自己重写。
//
// 注册时机:在 Application 启动【之前】。McpServer 是单例,提前 AddTool 就行,
// 不用改 xiaozhi 组件。顺带还占了 tools/list 的前排 —— 那边按字节分页,
// 描述长的工具排后面有落到第二页、被服务端漏掉的风险。

#include "learning.h"
#include "mcp_server.h"

#include <esp_log.h>
#include <string>

namespace tdeck {
namespace learning {

void register_mcp()
{
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool(
        "self.learning.show_text",
        "在屏幕上最大化显示正在学习的汉字、中文词语或英文单词。用户问汉字怎么读、英文词怎么拼、"
        "\"把这个字/单词放大给我看\"或正在教小朋友认字认单词时调用。"
        "不要用于\"某某汉字怎么写\"或笔顺请求;这类请求必须优先调用 self.learning.show_stroke_order,"
        "不能用静态汉字卡代替。"
        "text 必须是用户要学习的原始字或单词,不要翻译、改写或附加解释。guide 对中文填写简短拼音;"
        "对英文必须填写标准 IPA 音标并带斜杠,例如 /bɪˈkɒz/。"
        "meaning 只在 text 是英文单词时填:一句话的中文释义,越短越好,带词性更好"
        "(比如 because 填 \"conj. 因为\")。中文的字词【不要】填 —— 卡片上已经有拼音了,"
        "再加一行反而乱。"
        "调用工具后仍要用语音清楚读出这个字,或先读英文单词再逐字母拼读。",
        PropertyList({Property("text", kPropertyTypeString),
                      Property("guide", kPropertyTypeString, std::string("")),
                      Property("meaning", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& p) -> ReturnValue {
            const std::string text    = p["text"].value<std::string>();
            const std::string guide   = p["guide"].value<std::string>();
            const std::string meaning = p["meaning"].value<std::string>();
            if (text.empty()) return std::string("错误:要显示的字或单词不能为空");
            if (text.size() > 144 || guide.size() > 288) return std::string("错误:要显示的内容太长");
            show_card(text.c_str(), guide.c_str(), meaning.c_str());
            return std::string("已在屏幕上显示,约 20 秒后自动收起;想再看就再调一次");
        });

    mcp.AddTool(
        "self.learning.show_stroke_order",
        "在屏幕上播放一个汉字的笔顺动画(一笔一笔写出来)。"
        "用户问\"某某字怎么写\"、\"笔顺是什么\"、\"先写哪一笔\",或正在教小朋友写字时必须调用这个,"
        "不能改用 self.learning.show_text 显示静态汉字。character 必须只包含一个汉字。"
        "播放动画后仍要用语音按顺序讲解笔画。",
        PropertyList({Property("character", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            const std::string ch = p["character"].value<std::string>();
            if (ch.empty() || ch.size() > 4) return std::string("错误:请提供一个汉字");
            show_stroke_order(ch.c_str());
            return std::string("已播放汉字笔顺动画,约 30 秒后自动收起;孩子想再看一遍就再调一次");
        });

    mcp.AddTool(
        "self.learning.show_formula",
        "在屏幕上把公式排版出来给孩子看:数学公式和算式(分数、根号、上下标、π、×÷±≈≠≤≥)、"
        "物理公式和单位换算、化学方程式(反应条件写在箭头上)。"
        "凡是嘴上要念式子的场合都应该先调它 —— 念\"二分之一\"远不如把分数画出来清楚。"
        "formula 用 LaTeX 子集:\\frac{a}{b} \\sqrt{x} x^{2} H_{2}O \\xrightarrow{点燃} \\xlongequal{高温} "
        "\\times \\div \\pm \\pi,换行用 \\\\。title 填公式的名字(如\"圆的面积\"),note 填变量说明。"
        "整段文字用 self.learning.show_page,单个字或单词用 self.learning.show_text。"
        "调用后仍要用语音把式子讲一遍。",
        PropertyList({Property("title", kPropertyTypeString, std::string("")),
                      Property("formula", kPropertyTypeString),
                      Property("note", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& p) -> ReturnValue {
            const std::string title = p["title"].value<std::string>();
            const std::string f     = p["formula"].value<std::string>();
            const std::string note  = p["note"].value<std::string>();
            if (f.empty()) return std::string("错误:formula 不能为空");
            if (f.size() > 400 || title.size() > 60 || note.size() > 120)
                return std::string("错误:内容太长,请精简或分几次显示");
            show_formula(title.c_str(), f.c_str(), note.c_str());
            return std::string("已在屏幕上显示公式,约 30 秒后自动收起(多行更久);孩子还想看就再调一次");
        });

    mcp.AddTool(
        "self.learning.show_page",
        "在屏幕上显示一整段文字:古诗、课文句子、组词、词语释义、英文例句。"
        "内容较长时会自动折行并可滚动。单个字或单词用 self.learning.show_text,"
        "公式算式用 self.learning.show_formula。调用后仍要用语音念一遍。",
        PropertyList({Property("title", kPropertyTypeString, std::string("")),
                      Property("body", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            const std::string title = p["title"].value<std::string>();
            const std::string body  = p["body"].value<std::string>();
            if (body.empty()) return std::string("错误:body 不能为空");
            if (body.size() > 900 || title.size() > 60)
                return std::string("错误:内容太长,请分几次显示");
            show_page(title.c_str(), body.c_str());
            return std::string("已在屏幕上显示,看字数决定停留多久(20~90 秒),之后自动收起");
        });

    mcp.AddTool(
        "self.learning.clear",
        "立刻收起屏幕上的学习内容(卡片/笔顺/公式/文字页),回到对话画面。"
        "用户说\"关掉\"、\"不看了\"时调用。平时不用调 —— 内容会自己到时间收起。",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            clear();
            return std::string("已收起");
        });

    ESP_LOGI("learning", "self.learning.* 已注册(5 个工具)");
}

}  // namespace learning
}  // namespace tdeck
