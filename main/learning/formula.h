/*
 * SPDX-FileCopyrightText: 2026 Tank Xu
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <lvgl.h>
#include <string>

/* 把一段 LaTeX 子集排成公式,返回一个尺寸已定的容器(子对象绝对定位),调用方自己 align。
 * 支持:\frac{a}{b}  \sqrt{x}  x^{2}  H_{2}O  \xrightarrow{点燃}  \xlongequal{高温}  \\ 换行
 *      符号命令 \times \div \pm \cdot \pi \approx \neq \le \ge \rightarrow \uparrow \downarrow \degree 希腊字母等
 * 不认识的命令把名字原样打出来,总比空着强。big 管公式正文(PuHuiMath28:数字/字母/希腊/数学符号 28px),
 * text 管中文、上下标、反应条件和 big 缺字时的回落(notify::text_font() 的 20px 全字库);两者传同一个字体就是 20px 紧凑版。 */
lv_obj_t* formula_render(lv_obj_t* parent, const std::string& latex, const lv_font_t* big, const lv_font_t* text, int max_w = 300);
/* 超过 max_w 的行会在 + = → 前自动折行 */
