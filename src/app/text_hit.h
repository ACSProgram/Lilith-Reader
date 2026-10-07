// text_hit.h — 文本命中测试的纯几何（可单测）
//
// 从 text_interaction.cpp 的 char_index_at 抽出的**选行**逻辑。为什么值得单独成头：
// 它是"看起来能用、实则错位"的那一类 —— 单列版面一切正常，**多列/书版（两列论文）里
// 右列几乎选不中**。根因是行选择的方向错误：
//
//   一块版面上同一 y 区间常常有**多行**（左列一行、右列一行，行带完全重叠）。旧实现
//   遍历行、**取第一个 y 命中就收**；而 MuPDF 的 stext 块按阅读顺序排，左列整列在前，
//   于是悬停右列任意一点都会先"命中"左列那一行。随后的行内字符选择找不到横向匹配，
//   x 距离必然很大 → 严格模式判"此处无文字"，右列几乎选不中（实测：两列论文）。
//
// 正确判据是两维的：**先按"纵向距行带（含行高 1/4 容差）"取近；纵向并列（都落在带内）
// 时再按"横向距行框"取近**。单列版面里每个 y 至多命中一行，行为与旧实现逐位一致。
//
// 纯浮点、无 ImGui / Win32 / document 依赖（元素类型由模板参数决定，故 lr::TextLine 与
// 测试里的同名结构都能直接实例化），因此可像 page_map.h 那样被单测直接断言
// （tests/text_hit_test.cpp）。

#pragma once

#include <cstddef>

namespace lr::app {

// 行的"文字高度"：取 bbox 高与宽的较小者。
// 竖排文字的 bbox 高是**文字长度**，直接拿它当行高会得出荒唐的容差（整页都算命中）。
template <class Line>
inline float hit_line_text_height(const Line& L) {
    const float h = L.y1 - L.y0;
    const float w = L.x1 - L.x0;
    const float m = h < w ? h : w;
    return m > 1.0f ? m : 1.0f;
}

// 选行。lines 只需支持 size() 与 lines[i].{x0,y0,x1,y1}（故对 lr::TextLine 与测试结构
// 都适用）。返回命中的行下标；-1 = 无行。
// *out_in_band（可空）= 命中点是否落在所选行的**纵向容差带**内：严格模式（悬停/按下）
// 据此判定"此处有无文字"，超出即如实返回无命中；拖拽（clamp）才退回最近行。
template <class Lines>
inline int pick_text_line(const Lines& lines, float x, float y, int* out_in_band) {
    const int count = static_cast<int>(lines.size());
    int   best = -1;
    float best_dy = 0.0f;
    float best_dx = 0.0f;
    for (int i = 0; i < count; ++i) {
        const auto& L = lines[static_cast<std::size_t>(i)];
        const float pad = hit_line_text_height(L) * 0.25f;
        // 纵向距行带（0 = 落在带内）
        float dy = 0.0f;
        if (y < L.y0 - pad)      dy = (L.y0 - pad) - y;
        else if (y > L.y1 + pad) dy = y - (L.y1 + pad);
        // 横向距行框（0 = 落在框内）—— 只在纵向并列时用于破平（多列版面）。
        float dx = 0.0f;
        if (x < L.x0)      dx = L.x0 - x;
        else if (x > L.x1) dx = x - L.x1;
        if (best < 0 || dy < best_dy || (dy == best_dy && dx < best_dx)) {
            best = i;
            best_dy = dy;
            best_dx = dx;
        }
    }
    if (out_in_band) *out_in_band = (best >= 0 && best_dy == 0.0f) ? 1 : 0;
    return best;
}

}  // namespace lr::app
