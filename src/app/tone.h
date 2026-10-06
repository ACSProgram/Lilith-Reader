// tone.h — 纸张方案 → chrome 色调映射（ADR-068）
//
// 为什么不是"给整块调色板乘一个暖化系数"：那种做法只改色相、不管层次，彩色底上会
// 立刻露出两个毛病 —— ①所有表面被推向同一色相后彼此更难分辨（"控件框和底糊在一起"）；
// ②冷色强调（蓝）被"污染"成脏蓝，而不是变成该方案该有的强调色。
//
// 本文件把"上色"拆成**按角色分类**的三条规则，每类各自有明确的保证：
//
//   中性族（色度 < kChromaMin）：表面、文字、边框、控件底。
//       旋转到方案色相并补饱和度，但**保持 WCAG 相对亮度** —— 于是各层之间"谁比谁亮"
//       的关系与中性方案逐字一致，层次不会因为换色而塌掉。
//       其中"中间调"（相对亮度 0.02~0.50）再乘一次 ink 压深：色度对比**不**计入明度对比，
//       彩色底的感知对比天生低于中性底，次要文字与边框需要额外压深一档来补偿。
//   强调族（有色、且色相偏冷）：整族换到方案强调色的色相/饱和度上，**明度沿用原色** ——
//       常态/悬停/按下之间的明暗关系因此原样保留，只是整族换了色相。
//       原色用蓝、深色用沙金、暖色用赭石，"页面暖、按钮冷"在结构上不再可能发生。
//   语义族（有色、且非冷色：红/橙/绿）：只把色相向方案色相拉近一个小比例，
//       保住"红=错误、橙=警告"的可辨识度 —— 全盘换色会把语义一起抹掉。
//
// 纯函数、不依赖 ImGui / Win32：platform.cpp 与 tests/tone_test.cpp 都直接包含它，
// 因此配色可以被自动化断言（见 tests/tone_test.cpp）。

#pragma once

#include <cmath>

namespace lr::app {

struct Rgb {
    float r = 0.0f, g = 0.0f, b = 0.0f;
};

// 判"有没有颜色"用**色度**（max−min）而不是 HSV 饱和度：近黑正文 (0.13,0.15,0.18) 的
// 相对饱和度高达 0.28（按饱和度判会被误当成冷色强调），色度却只有 0.05 —— 肉眼就是中性。
inline constexpr float kChromaMin = 0.15f;
inline constexpr float kColdHueMin = 180.0f;   // 冷色相区间（含青、蓝、紫）
inline constexpr float kColdHueMax = 305.0f;

struct Tone {
    bool  active   = false;   // false ⇒ 原色方案：一律原样返回，一个字节都不动
    float hue      = 0.0f;    // 中性族的目标色相（度）
    float sat      = 0.0f;    // 中性族的目标饱和度上限（实际按明度分配，见 sat_ramp）
    float level    = 1.0f;    // 中性族明度倍率（<1 整体压深）
    float ink      = 1.0f;    // 中间调压深倍率（<1 压深）
    float semantic = 0.0f;    // 语义色向方案色相融入的比例
    Rgb   accent{};           // 强调色的**色相/饱和度样本**（明度另由原色决定，见 tone_apply）
};

inline bool tone_same(const Tone& a, const Tone& b) {
    return a.active == b.active && a.hue == b.hue && a.sat == b.sat &&
           a.level == b.level && a.ink == b.ink && a.semantic == b.semantic &&
           a.accent.r == b.accent.r && a.accent.g == b.accent.g && a.accent.b == b.accent.b;
}

inline float srgb_to_linear(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
inline float linear_to_srgb(float v) {
    v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
}

// WCAG 相对亮度（线性域，非 0.299/0.587/0.114 的感知加权）
inline float tone_luminance(Rgb c) {
    return 0.2126f * srgb_to_linear(c.r) + 0.7152f * srgb_to_linear(c.g) +
           0.0722f * srgb_to_linear(c.b);
}
inline float tone_contrast(Rgb a, Rgb b) {
    const float la = tone_luminance(a), lb = tone_luminance(b);
    const float hi = la > lb ? la : lb, lo = la > lb ? lb : la;
    return (hi + 0.05f) / (lo + 0.05f);
}

inline void rgb_to_hsv(Rgb c, float& h, float& s, float& v) {
    const float mx = std::fmax(c.r, std::fmax(c.g, c.b));
    const float mn = std::fmin(c.r, std::fmin(c.g, c.b));
    v = mx;
    const float d = mx - mn;
    s = (mx > 0.0f) ? d / mx : 0.0f;
    if (d <= 0.0f) { h = 0.0f; return; }
    if (mx == c.r)      h = 60.0f * std::fmod((c.g - c.b) / d, 6.0f);
    else if (mx == c.g) h = 60.0f * ((c.b - c.r) / d + 2.0f);
    else                h = 60.0f * ((c.r - c.g) / d + 4.0f);
    if (h < 0.0f) h += 360.0f;
}

inline Rgb hsv_to_rgb(float h, float s, float v) {
    h = std::fmod(h, 360.0f);
    if (h < 0.0f) h += 360.0f;
    s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    Rgb o;
    if (h < 60.0f)       o = { c, x, 0.0f };
    else if (h < 120.0f) o = { x, c, 0.0f };
    else if (h < 180.0f) o = { 0.0f, c, x };
    else if (h < 240.0f) o = { 0.0f, x, c };
    else if (h < 300.0f) o = { x, 0.0f, c };
    else                 o = { c, 0.0f, x };
    o.r += m; o.g += m; o.b += m;
    return o;
}

// 把颜色缩放到指定的相对亮度。相对亮度是线性 RGB 的线性组合，故**线性域等比缩放**
// 能精确保住亮度，同时保住色相；超出色域时以最亮通道封顶（宁可略暗也不失真）。
inline Rgb fit_luminance(Rgb c, float target_y) {
    const float lin[3] = { srgb_to_linear(c.r), srgb_to_linear(c.g), srgb_to_linear(c.b) };
    const float y = 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2];
    float k = (y > 1e-7f) ? (target_y / y) : 0.0f;
    const float mx = std::fmax(lin[0], std::fmax(lin[1], lin[2]));
    if (mx * k > 1.0f) k = (mx > 1e-7f) ? (1.0f / mx) : 0.0f;
    return { linear_to_srgb(lin[0] * k), linear_to_srgb(lin[1] * k), linear_to_srgb(lin[2] * k) };
}

// 饱和度按明度分配：中低明度最饱和，亮面（近白）与极暗面都收敛 ——
// 亮面不收会得到"比纸还黄的对话框"，暗面不收会得到一团看不清层次的泥。
inline float sat_ramp(float y) {
    const float r = 1.25f - 1.5f * std::fabs(y - 0.45f);
    return r < 0.15f ? 0.15f : (r > 1.0f ? 1.0f : r);
}

inline float blend_hue(float h, float target, float k) {
    const float d = std::fmod(target - h + 540.0f, 360.0f) - 180.0f;
    return h + d * k;
}

// 用给定色相/饱和度生成一个"亮度为 y"的颜色。
//
// 先按给定饱和度上色；若该色的亮度已经高过目标，正常缩放即可。反之说明**目标亮度超出
// 这个色相在色域内能达到的上限**（越接近白，能承载的饱和度越低）—— 此时必须**降饱和**
// 而不是硬钳：硬钳会让近白的中性色（暖白文字、亮面板）凭空变暗，"保持亮度"这条保证就破了。
//
// 对 v=1 的同色相颜色，**亮度随饱和度单调下降**，故在 [0, sat] 上二分找"亮度恰好等于 y"
// 的那个饱和度：亮度 ≥ y 说明还可以更饱和（lo 上移），否则 hi 下移。收敛后取 hi，
// 即"≤ y 的最大饱和度"，宁可差一丝也不会偏亮。
inline Rgb tone_tint(float hue, float sat, float y) {
    Rgb o = hsv_to_rgb(hue, sat, 1.0f);
    if (tone_luminance(o) >= y) return fit_luminance(o, y);
    float lo = 0.0f, hi = sat;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (tone_luminance(hsv_to_rgb(hue, mid, 1.0f)) >= y) lo = mid;
        else hi = mid;
    }
    return hsv_to_rgb(hue, hi, 1.0f);   // 饱和归零即纯白，亮度 1 可达任意 y ≤ 1
}

// 核心：把中性方案里的一个颜色映射到当前纸张方案的色调。
inline Rgb tone_apply(Rgb c, const Tone& t) {
    if (!t.active) return c;

    const float chroma = std::fmax(c.r, std::fmax(c.g, c.b)) -
                         std::fmin(c.r, std::fmin(c.g, c.b));
    float h = 0.0f, s = 0.0f, v = 0.0f;
    rgb_to_hsv(c, h, s, v);

    if (chroma >= kChromaMin) {                       // 有色
        if (h >= kColdHueMin && h <= kColdHueMax) {   // 强调族：换色相/饱和度，明度沿用原色
            float ah = 0.0f, as = 0.0f, av = 0.0f;
            rgb_to_hsv(t.accent, ah, as, av);
            return tone_tint(ah, as, tone_luminance(c) * t.level);
        }
        h = blend_hue(h, t.hue, t.semantic);          // 语义族：只拉近色相
        return tone_tint(h, s, tone_luminance(c) * t.level);
    }
    // 中性族：旋转色相、按明度补饱和、**保持相对亮度**；
    // 中间调（次要文字、边框、控件底）再压深一档，补偿彩色底偏低的感知对比。
    float y = tone_luminance(c) * t.level;
    if (y > 0.02f && y < 0.50f) y *= t.ink;
    return tone_tint(t.hue, t.sat * sat_ramp(y), y);
}

}  // namespace lr::app
