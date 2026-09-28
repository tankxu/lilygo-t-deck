/*
 * SPDX-FileCopyrightText: 2026 Tank Xu
 *
 * SPDX-License-Identifier: MIT
 */
/* 公式排版器:LaTeX 子集 → LVGL 对象树。
 *
 * 【为什么在设备上排而不是服务端出图】小学到初中的公式结构就那几种:分数、根号、上下标、带条件的箭头,
 * 一个两三百行的递归盒子布局就够;不用等下载、不用 R2 存图,LLM 现写现显示。
 * 【为什么收 LaTeX】模型写 LaTeX 最稳,\frac{1}{2} 几乎不会写错;自造记法反而要在提示词里教。
 *
 * 布局模型:每个节点排成一个 Box{对象, 宽, 高, 基线到顶的距离},序列按基线对齐横排;
 * 分数线/根号横线/箭头轴线都放在"数学轴"上(基线往上约 0.35 行高)。 */
#include "formula.h"
#include <memory>
#include <vector>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

struct Node;
using Seq = std::vector<std::unique_ptr<Node>>;
enum class Kind { Text, Frac, Sqrt, Sup, Sub, Arrow, Equal, Newline };
struct Node {
    Kind kind = Kind::Text;
    std::string text;   // Text 用
    Seq a, b;           // Frac: a/b;Sqrt/Sup/Sub: a;Arrow/Equal: a = 箭头上方的条件
};

size_t utf8_len(unsigned char c) { return c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4; }

const char* sym(const std::string& name)
{
    static const struct { const char* n; const char* u; } tbl[] = {
        {"times", "×"}, {"div", "÷"}, {"pm", "±"}, {"mp", "±"}, {"cdot", "·"}, {"pi", "π"}, {"approx", "≈"}, {"neq", "≠"}, {"ne", "≠"},
        {"le", "≤"}, {"leq", "≤"}, {"ge", "≥"}, {"geq", "≥"}, {"rightarrow", "→"}, {"to", "→"}, {"Rightarrow", "→"}, {"longrightarrow", "→"},
        {"leftarrow", "←"}, {"uparrow", "↑"}, {"downarrow", "↓"}, {"infty", "∞"}, {"degree", "°"}, {"circ", "°"}, {"angle", "∠"},
        {"perp", "⊥"}, {"parallel", "∥"}, {"triangle", "△"}, {"sum", "∑"}, {"int", "∫"}, {"because", "∵"}, {"therefore", "∴"},
        {"sim", "∽"}, {"equiv", "≡"}, {"in", "∈"}, {"cup", "∪"}, {"cap", "∩"}, {"ldots", "…"}, {"cdots", "…"}, {"dots", "…"},
        {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"varepsilon", "ε"}, {"zeta", "ζ"}, {"eta", "η"},
        {"theta", "θ"}, {"iota", "ι"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"}, {"rho", "ρ"}, {"sigma", "σ"},
        {"tau", "τ"}, {"upsilon", "υ"}, {"phi", "φ"}, {"varphi", "φ"}, {"chi", "χ"}, {"psi", "ψ"}, {"omega", "ω"},
        {"Delta", "Δ"}, {"Omega", "Ω"}, {"Sigma", "Σ"}, {"Gamma", "Γ"}, {"Theta", "Θ"}, {"Lambda", "Λ"}, {"Pi", "Π"}, {"Phi", "Φ"}, {"Psi", "Ψ"},
        {",", " "}, {";", " "}, {":", " "}, {"!", ""}, {" ", " "}, {"%", "%"}, {"{", "{"}, {"}", "}"}, {"&", "&"}, {"_", "_"}, {"#", "#"},
        {"quad", "  "}, {"qquad", "    "},
    };
    for (auto& t : tbl) if (name == t.n) return t.u;
    return nullptr;
}

struct Parser {
    const std::string& s;
    size_t i = 0;
    explicit Parser(const std::string& src) : s(src) {}
    bool eof() const { return i >= s.size(); }
    void skip_ws() { while (!eof() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++; }

    /* 二元关系/运算符两侧统一留一个空格:LaTeX 排 a=b 会自动加中距,模型常写 \\rho=\\frac..(无空格)或 x=y,
     * 位图字体没有排版间距,不补空格就挤成一团;一元的 - 和 ± 不动。 */
    static std::string space_ops(const std::string& t)
    {
        static const char* ops[] = {"=", "+", "×", "÷", "≈", "≠", "≤", "≥", "→", "←"};
        std::string out;
        size_t i = 0;
        while (i < t.size()) {
            size_t len = utf8_len((unsigned char)t[i]);
            std::string ch = t.substr(i, len);
            bool is_op = false;
            for (auto op : ops) if (ch == op) { is_op = true; break; }
            if (is_op) {
                /* 段首/段尾的运算符也补:\\rho 和 = 是两段先后 push 进来的,左边是希腊字母、右边是分数节点,
                 * 不补就挤成 ρ=;多出来的连续空格最后并成一个 */
                if (out.empty() || out.back() != ' ') out.push_back(' ');
                out += ch;
                out.push_back(' ');
            } else {
                out += ch;
            }
            i += len;
        }
        std::string squeezed;
        for (char c : out) if (!(c == ' ' && !squeezed.empty() && squeezed.back() == ' ')) squeezed.push_back(c);
        return squeezed;
    }
    void push_text(Seq& out, const std::string& raw)
    {
        if (raw.empty()) return;
        if (!out.empty() && out.back()->kind == Kind::Text) { out.back()->text = space_ops(out.back()->text + raw); return; }   // 合并后整段重排空格
        auto n = std::make_unique<Node>();
        n->kind = Kind::Text;
        n->text = space_ops(raw);
        out.push_back(std::move(n));
    }
    /* 一个参数:{分组} 或单个字符 / 单个命令 */
    Seq arg()
    {
        skip_ws();
        Seq out;
        if (eof()) return out;
        if (s[i] == '{') { i++; parse_into(out, true); return out; }
        if (s[i] == '\\') { parse_one(out); return out; }
        size_t len = utf8_len((unsigned char)s[i]);
        push_text(out, s.substr(i, len));
        i += len;
        return out;
    }
    void skip_opt()   // [..] 可选参数:\sqrt[3]{x} 的 3、\xrightarrow[下]{上} 的下,都不画
    {
        skip_ws();
        if (!eof() && s[i] == '[') { while (!eof() && s[i] != ']') i++; if (!eof()) i++; }
    }
    void parse_into(Seq& out, bool until_brace)
    {
        while (!eof()) {
            if (s[i] == '}') { i++; if (until_brace) return; continue; }   // 顶层多出来的 } 忽略
            parse_one(out);
        }
    }
    void parse_one(Seq& out)
    {
        char c = s[i];
        if (c == '{') { i++; parse_into(out, true); return; }   // 裸分组:内容直接接上
        if (c == '^' || c == '_') {
            i++;
            auto n  = std::make_unique<Node>();
            n->kind = (c == '^') ? Kind::Sup : Kind::Sub;
            n->a    = arg();
            out.push_back(std::move(n));
            return;
        }
        if (c == '$') { i++; return; }
        if (c == '\n') { i++; auto n = std::make_unique<Node>(); n->kind = Kind::Newline; out.push_back(std::move(n)); return; }
        if (c == '\\') {
            i++;
            if (eof()) return;
            if (s[i] == '\\') { i++; auto n = std::make_unique<Node>(); n->kind = Kind::Newline; out.push_back(std::move(n)); return; }
            if (!std::isalpha((unsigned char)s[i])) {
                std::string one = s.substr(i, 1);
                i++;
                const char* u = sym(one);
                push_text(out, u ? u : one);
                return;
            }
            size_t st = i;
            while (!eof() && std::isalpha((unsigned char)s[i])) i++;
            std::string name = s.substr(st, i - st);
            skip_ws();   // LaTeX 规则:控制词后面的空格是分隔符不是内容,\Delta t 要排成 Δt
            if (name == "frac" || name == "dfrac" || name == "tfrac") {
                auto n = std::make_unique<Node>(); n->kind = Kind::Frac; n->a = arg(); n->b = arg(); out.push_back(std::move(n)); return;
            }
            if (name == "sqrt") {
                auto n = std::make_unique<Node>(); n->kind = Kind::Sqrt;
                skip_ws();
                if (!eof() && s[i] == '[') {   // \sqrt[3]{27}:根指数进 b,画在根号左上
                    i++;
                    size_t st2 = i;
                    while (!eof() && s[i] != ']') i++;
                    std::string idx = s.substr(st2, i - st2);
                    if (!eof()) i++;
                    Parser sub(idx); sub.parse_into(n->b, false);
                }
                n->a = arg(); out.push_back(std::move(n)); return;
            }
            if (name == "xrightarrow" || name == "xlongequal" || name == "overset" || name == "stackrel") {
                auto n  = std::make_unique<Node>();
                n->kind = (name == "xlongequal") ? Kind::Equal : Kind::Arrow;
                skip_opt();
                n->a = arg();
                if (name == "overset" || name == "stackrel") {   // \overset{点燃}{=} / \overset{高温}{\rightarrow}:看底下是等号还是箭头
                    Seq base = arg();
                    std::string bt;
                    for (auto& x : base) bt += x->text;
                    n->kind = (bt.find("→") != std::string::npos) ? Kind::Arrow : Kind::Equal;
                }
                out.push_back(std::move(n));
                return;
            }
            if (name == "text" || name == "mathrm" || name == "mathbf" || name == "textbf" || name == "mbox" || name == "operatorname" || name == "mathit" || name == "textit") {
                Seq g = arg();
                for (auto& x : g) out.push_back(std::move(x));
                return;
            }
            if (name == "left" || name == "right") { if (!eof() && s[i] == '.') i++; return; }   // \left( 的括号本身留给普通字符路径
            if (name == "displaystyle" || name == "big" || name == "Big" || name == "bigl" || name == "bigr" || name == "limits") return;
            if (name == "begin" || name == "end") { arg(); return; }   // \begin{aligned} 之类:环境名吞掉,内容照排
            const char* u = sym(name);
            push_text(out, u ? u : name);
            return;
        }
        if (c == '&') { i++; push_text(out, " "); return; }   // aligned 环境的对齐符当空格
        size_t st = i;
        while (!eof()) {
            char d = s[i];
            if (d == '\\' || d == '{' || d == '}' || d == '^' || d == '_' || d == '$' || d == '\n' || d == '&') break;
            i += utf8_len((unsigned char)d);
        }
        push_text(out, s.substr(st, i - st));
    }
};

/* ── 布局 ──
 * 【为什么是扁平的】第一版每个序列/分数/根号都套一层 lv_obj 容器,求根公式嵌到 7 层;显示没事,
 * 但 /screenshot 在 httpd 任务(8KB 栈)上跑 lv_snapshot,递归画到第 7 层就栈溢出崩机(2026-09-18 实测,
 * StoreProhibited + 回溯损坏)。现在所有叶子(label / 白条 / 箭头线)都直接挂在同一个容器下,
 * 布局阶段只记相对坐标,最后统一 set_pos,对象树固定两层。 */
struct Part { lv_obj_t* o; int x, y; };
struct Box { std::vector<Part> parts; int w = 0, h = 0, base = 0; };   // base: 基线离盒顶的距离
/* big:公式正文(28px,只有数字/字母/希腊/数学符号);text:全字库 20px,管中文、上下标、反应条件,
 * 也是 big 缺字时的回落(按整段 Text 判,一段里混了中文就整段用 20px,避免一行里两种字号跳来跳去)。 */
struct Ctx { lv_obj_t* root; const lv_font_t* big; const lv_font_t* text; int lh; };   // lh = 正文(big)行高

bool font_has_all(const lv_font_t* font, const std::string& t)
{
    size_t i = 0;
    while (i < t.size()) {
        unsigned char c = (unsigned char)t[i];
        uint32_t cp; int len;
        if (c < 0x80) { cp = c; len = 1; } else if (c < 0xE0) { cp = c & 0x1F; len = 2; } else if (c < 0xF0) { cp = c & 0x0F; len = 3; } else { cp = c & 0x07; len = 4; }
        if (i + (size_t)len > t.size()) return false;
        for (int k = 1; k < len; k++) cp = (cp << 6) | ((unsigned char)t[i + k] & 0x3F);
        i += len;
        if (cp < 0x20) continue;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, cp, 0)) return false;
    }
    return true;
}
const lv_font_t* pick_font(Ctx& cx, const std::string& t, bool small) { return (!small && font_has_all(cx.big, t)) ? cx.big : cx.text; }

const lv_color_t kFg = lv_color_hex(0xF5F5F5);


void shift(Box& b, int dx, int dy) { for (auto& p : b.parts) { p.x += dx; p.y += dy; } }
void merge(Box& into, Box& from, int dx, int dy) { shift(from, dx, dy); for (auto& p : from.parts) into.parts.push_back(p); }

lv_obj_t* make_rule(Ctx& cx, int w, int h)   // 实心白条:分数线、根号横线、箭头杆、等号
{
    lv_obj_t* o = lv_obj_create(cx.root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, std::max(w, 1), std::max(h, 1));
    lv_obj_remove_flag(o, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_set_style_bg_color(o, kFg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

/* 折线(根号、箭头头):点数组 new 出来挂在对象上,LV_EVENT_DELETE 时一起放;坐标相对对象自身 */
lv_obj_t* make_polyline(Ctx& cx, lv_point_precise_t* pts, int n, int w, int h)
{
    lv_obj_t* o = lv_line_create(cx.root);
    lv_line_set_points(o, pts, n);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_line_width(o, 2, 0);
    lv_obj_set_style_line_color(o, kFg, 0);
    lv_obj_set_style_line_rounded(o, true, 0);
    lv_obj_set_user_data(o, pts);
    lv_obj_add_event_cb(o, [](lv_event_t* e) { delete[] static_cast<lv_point_precise_t*>(lv_obj_get_user_data((lv_obj_t*)lv_event_get_target(e))); }, LV_EVENT_DELETE, nullptr);
    return o;
}
#define PP(x, y) lv_point_precise_t{(lv_value_precise_t)(x), (lv_value_precise_t)(y)}

Box text_box(Ctx& cx, const std::string& t, const lv_font_t* f)
{
    lv_point_t sz;
    lv_text_get_size(&sz, t.c_str(), f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    Box b;
    b.w    = sz.x;
    b.h    = lv_font_get_line_height(f);
    b.base = b.h - f->base_line;
    lv_obj_t* l = lv_label_create(cx.root);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, kFg, 0);
    lv_label_set_text(l, t.c_str());
    lv_obj_set_size(l, std::max(b.w, 1), b.h);
    b.parts.push_back({l, 0, 0});
    return b;
}

Box layout_seq(const Seq& seq, Ctx& cx, bool small);

Box layout_node(const Node& n, Ctx& cx, bool small)
{
    const lv_font_t* f = pick_font(cx, n.text, small);
    const int lh       = small ? lv_font_get_line_height(cx.text) : cx.lh;
    const int axis     = (int)(lh * 0.35);   // 数学轴在基线上方多少

    switch (n.kind) {
    case Kind::Text: return text_box(cx, n.text, f);

    case Kind::Frac: {
        Box num = layout_seq(n.a, cx, small);
        Box den = layout_seq(n.b, cx, small);
        int w  = std::max(num.w, den.w) + 6;
        int ly = num.h + 2;
        Box b;
        merge(b, num, (w - num.w) / 2, 0);
        b.parts.push_back({make_rule(cx, w, 2), 0, ly});
        merge(b, den, (w - den.w) / 2, ly + 4);
        b.w = w; b.h = ly + 4 + den.h; b.base = ly + 1 + axis;
        return b;
    }
    case Kind::Sqrt: {
        /* 根号不用字形(20px 的 √ 罩不住带上标/分数的内容),按内容高度画三段折线 + 顶上横线,多高都罩得住 */
        Box body = layout_seq(n.a, cx, small);
        const int H  = body.h + 4;                       // 横线到内容底
        const int Wr = std::clamp(H * 2 / 5, 9, 16);     // 根号勾的宽
        auto* pts = new lv_point_precise_t[3]{PP(1, H * 0.62), PP(Wr * 0.4, H - 1), PP(Wr, 1)};
        Box b;
        int x0 = 0;   // 有根指数时整体右移,指数压在勾的左上
        if (!n.b.empty()) {
            Box idx = layout_seq(n.b, cx, true);
            x0 = std::max(0, idx.w - Wr / 3);
            merge(b, idx, 0, 0);
        }
        b.parts.push_back({make_polyline(cx, pts, 3, Wr + 2, H + 1), x0, 0});
        b.parts.push_back({make_rule(cx, body.w + 6, 2), x0 + Wr, 0});
        merge(b, body, x0 + Wr + 3, 4);
        b.w = x0 + Wr + body.w + 6; b.h = H + 1; b.base = 4 + body.base;
        return b;
    }
    case Kind::Arrow:
    case Kind::Equal: {
        Box cond = layout_seq(n.a, cx, true);
        int w  = std::max(cond.w + 14, 56);
        int ly = cond.h + 5;   // 轴线 y
        Box b;
        merge(b, cond, (w - cond.w) / 2, 0);
        if (n.kind == Kind::Arrow) {
            b.parts.push_back({make_rule(cx, w - 7, 2), 0, ly - 1});
            /* 箭头:两条斜杆用 lv_line(坐标相对 line 对象自身),点数组挂在对象上、删对象时一起放 */
            auto* pts = new lv_point_precise_t[3]{PP(0, 0), PP(8, 6), PP(0, 12)};
            b.parts.push_back({make_polyline(cx, pts, 3, 10, 14), w - 10, ly - 7});
        } else {
            b.parts.push_back({make_rule(cx, w, 2), 0, ly - 3});
            b.parts.push_back({make_rule(cx, w, 2), 0, ly + 1});
        }
        b.w = w; b.h = ly + 8; b.base = ly + axis;
        return b;
    }
    default: return Box{};
    }
}

/* 横排一串:按基线对齐,上下标挂在前一个盒子上 */
Box layout_seq(const Seq& seq, Ctx& cx, bool small)
{
    struct Item { Box b; int x; int base_off; };   // base_off:相对序列基线的偏移(上标负、下标正)
    std::vector<Item> items;
    int x        = 0;
    const int lh = small ? lv_font_get_line_height(cx.text) : cx.lh;
    int ref_h = 0, ref_base = 0;   // 上下标参照的前一个盒子
    for (auto& n : seq) {
        if (n->kind == Kind::Newline) { x += lh / 2; continue; }   // 嵌套里的换行当空格
        if (n->kind == Kind::Sup || n->kind == Kind::Sub) {
            Box inner = layout_seq(n->a, cx, true);
            int off;
            if (n->kind == Kind::Sup) {
                int up = (ref_base > lh) ? ref_base - (int)(lh * 0.55) : (int)(lh * 0.40);   // 挂在分数上就抬到分数顶附近
                off = -up;
            } else {
                int down = (ref_h - ref_base > lh / 2) ? (ref_h - ref_base) - (int)(lh * 0.15) : (int)(lh * 0.22);
                off = down;
            }
            items.push_back({std::move(inner), x, off});
            x += items.back().b.w + 1;
            continue;   // 参照不变:H_2^+ 这种上下标都挂同一个字母
        }
        Box b = layout_node(*n, cx, small);
        if (b.parts.empty()) continue;
        ref_h = b.h; ref_base = b.base;
        int bw = b.w;
        items.push_back({std::move(b), x, 0});
        x += bw + 1;
    }
    Box out;
    if (items.empty()) { out.w = 1; out.h = lh; out.base = lh; return out; }
    int B = 0, H = 0;
    for (auto& it : items) B = std::max(B, it.b.base - it.base_off);
    for (auto& it : items) {
        int top = B + it.base_off - it.b.base;
        merge(out, it.b, it.x, top);
        H = std::max(H, top + it.b.h);
    }
    out.w = x; out.h = H; out.base = B;
    return out;
}

/* ── 太宽的行自动折行 ──
 * 屏幕只有 320 宽,一个化学方程式常常排不下。在 + = − × → 前面断开(运算符跟后半段走),
 * 断点选累计宽度最接近一半的那个;折出来的两段各自居中,还不够再折。 */
int est_w(const Node& n, Ctx& cx, bool small);
int est_w(const Seq& q, Ctx& cx, bool small) { int w = 0; for (auto& n : q) w += est_w(*n, cx, small) + 1; return w; }
int est_w(const Node& n, Ctx& cx, bool small)
{
    lv_point_t sz;
    switch (n.kind) {
    case Kind::Text: lv_text_get_size(&sz, n.text.c_str(), pick_font(cx, n.text, small), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE); return sz.x;
    case Kind::Sup: case Kind::Sub: return est_w(n.a, cx, true);
    case Kind::Frac: return std::max(est_w(n.a, cx, small), est_w(n.b, cx, small)) + 6;
    case Kind::Sqrt: return est_w(n.a, cx, small) + 20;
    case Kind::Arrow: case Kind::Equal: return std::max(56, est_w(n.a, cx, true) + 14);
    default: return 0;
    }
}
bool starts_with_op(const std::string& t)
{
    static const char* ops[] = {"+", "=", "-", "×", "÷", "→", "≈", "≠", "≤", "≥", "±"};
    size_t k = 0;
    while (k < t.size() && t[k] == ' ') k++;
    for (auto op : ops) if (t.compare(k, strlen(op), op) == 0) return true;
    return false;
}
/* 把顶层 Text 里的 " + " 之类拆成独立节点,运算符打头,这样断点都落在节点边界 */
void split_breakables(Seq& line)
{
    Seq out;
    for (auto& n : line) {
        if (n->kind != Kind::Text) { out.push_back(std::move(n)); continue; }
        const std::string& t = n->text;
        size_t start = 0;
        for (size_t k = 1; k + 1 < t.size(); ) {
            std::string rest = t.substr(k);
            if (t[k - 1] == ' ' && starts_with_op(rest) && k > start) {
                auto piece = std::make_unique<Node>(); piece->kind = Kind::Text; piece->text = t.substr(start, k - start);
                out.push_back(std::move(piece));
                start = k;
            }
            k += utf8_len((unsigned char)t[k]);
        }
        auto tail = std::make_unique<Node>(); tail->kind = Kind::Text; tail->text = t.substr(start);
        out.push_back(std::move(tail));
    }
    line = std::move(out);
}
bool is_break_start(const Node& n) { return n.kind == Kind::Arrow || n.kind == Kind::Equal || (n.kind == Kind::Text && starts_with_op(n.text)); }

void layout_line_wrapped(Seq line, Ctx& cx, int max_w, std::vector<Box>& out)
{
    Box b = layout_seq(line, cx, false);
    if (b.w <= max_w || line.size() < 2) { out.push_back(std::move(b)); return; }
    std::vector<int> cum(line.size() + 1, 0);
    for (size_t k = 0; k < line.size(); k++) cum[k + 1] = cum[k] + est_w(*line[k], cx, false) + 1;
    int best = -1, best_d = 1 << 30;
    for (size_t k = 1; k < line.size(); k++) {
        if (!is_break_start(*line[k])) continue;
        int d = std::abs(cum[k] - cum.back() / 2);
        if (d < best_d) { best_d = d; best = (int)k; }
    }
    if (best < 0) { out.push_back(std::move(b)); return; }   // 没有可断处:只能裁
    for (auto& pt : b.parts) lv_obj_delete(pt.o);
    Seq left, right;
    for (size_t k = 0; k < line.size(); k++) (k < (size_t)best ? left : right).push_back(std::move(line[k]));
    layout_line_wrapped(std::move(left), cx, max_w, out);
    layout_line_wrapped(std::move(right), cx, max_w, out);
}

}  // namespace

lv_obj_t* formula_render(lv_obj_t* parent, const std::string& latex, const lv_font_t* big, const lv_font_t* text, int max_w)
{
    Seq top;
    Parser p(latex);
    p.parse_into(top, false);

    /* 顶层按换行切成多行,每行居中,竖着叠 */
    std::vector<Seq> lines(1);
    for (auto& n : top) {
        if (n->kind == Kind::Newline) { lines.emplace_back(); continue; }
        lines.back().push_back(std::move(n));
    }
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_remove_flag(root, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    Ctx cx{root, big, text, lv_font_get_line_height(big)};
    std::vector<Box> boxes;
    for (auto& ln : lines) {
        if (ln.empty()) continue;
        split_breakables(ln);
        layout_line_wrapped(std::move(ln), cx, max_w, boxes);
    }
    int W = 0;
    for (auto& b : boxes) W = std::max(W, b.w);
    Box all;
    int H = 0;
    for (size_t k = 0; k < boxes.size(); k++) {
        int bh = boxes[k].h;
        merge(all, boxes[k], (W - boxes[k].w) / 2, H);
        H += bh + (k + 1 < boxes.size() ? 10 : 0);
    }
    for (auto& pt : all.parts) lv_obj_set_pos(pt.o, pt.x, pt.y);
    lv_obj_set_size(root, std::max(W, 1), std::max(H, 1));
    return root;
}
