// page_map.h — 页面点 ↔ 屏幕点的坐标折算（纯函数）
//
// 为什么单独成头、且不依赖 ImGui / Win32：旋转折算是"文本选择 / 复制 / 超链接 /
// 搜索命中高亮"这一整套功能里最容易错的一环（90°/270° 的四角对应、归一化置换的逆），
// 而它完全由「页矩形 + 未旋转页尺寸 + 旋转角」决定 —— 无状态、无外部依赖，
// 因此可以像 tone.h 那样被单测直接断言（tests/page_map_test.cpp）。
//
// 为什么放在 app 层而不是 canvas：canvas **刻意对旋转透明**（旋转由 app 折算成
// "页尺寸宽高互换"后交给它，ADR-036）。只有 app 知道当前旋转角；若把旋转塞进 canvas，
// 就破坏了"画布只认旋转后的宽高"这条已经定稿的边界。
//
// 坐标空间（与 document.ixx 的约定一致）：
//   · 「页面 pt」= 未旋转的页面坐标，单位点，原点在页面左上、y 向下。
//     这是 fz_stext / fz_load_links 的原生空间，也是本文件两个函数里"那个 pt"的含义。
//   · 「屏幕点」= 画布内的屏幕像素坐标（**含**画布局部原点；调用方自己把 origin 加进 view）。
//
// 折算原理：旋转 90° 的整数倍只是**归一化坐标的置换**，不必做矩阵运算。
// 记未旋转页面的归一化坐标为 (nu, nv)、屏幕页矩形的归一化坐标为 (u, v)：
//   rot 0  : (u,v) = (nu, nv)              恒等
//   rot 90 : (u,v) = (1-nv, nu)            顺时针 90°：原左下角转到左上角
//   rot 180: (u,v) = (1-nu, 1-nv)          中心对称
//   rot 270: (u,v) = (nv, 1-nu)            顺时针 270°（= 逆时针 90°）
// 该置换与"页面包围盒原点是否为零"无关 —— 因为渲染出的位图恰好覆盖旋转后的包围盒，
// 归一化之后原点偏移自然消掉（这也是能只用归一化坐标的原因）。

#pragma once

namespace lr::app {

// 页面在屏幕上的矩形。宽高**已是旋转后**的值（即 canvas.page_rect 的结果 + 画布原点）。
struct PageView {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

// 未旋转的页面尺寸（点），即 document 的 PageSize。
struct PageGeom {
    float w_pt = 0.0f;
    float h_pt = 0.0f;
};

// 旋转角归一化到 {0,90,180,270}
[[nodiscard]] inline int normalize_rotation(int deg) noexcept {
    int r = deg % 360;
    if (r < 0) r += 360;
    return (r / 90) * 90;
}

// 未旋转归一化 (nu,nv) → 屏幕归一化 (u,v)
inline void norm_forward(int rot, float nu, float nv, float& u, float& v) noexcept {
    switch (normalize_rotation(rot)) {
    case 90:  u = 1.0f - nv; v = nu;        break;
    case 180: u = 1.0f - nu; v = 1.0f - nv; break;
    case 270: u = nv;        v = 1.0f - nu; break;
    default:  u = nu;        v = nv;        break;
    }
}

// 屏幕归一化 (u,v) → 未旋转归一化 (nu,nv)（norm_forward 的逆）
inline void norm_inverse(int rot, float u, float v, float& nu, float& nv) noexcept {
    switch (normalize_rotation(rot)) {
    case 90:  nu = v;        nv = 1.0f - u; break;
    case 180: nu = 1.0f - u; nv = 1.0f - v; break;
    case 270: nu = 1.0f - v; nv = u;        break;
    default:  nu = u;        nv = v;        break;
    }
}

// 页面 pt（未旋转）→ 屏幕点。页尺寸或页矩形退化时返回 false（调用方据此跳过绘制）。
[[nodiscard]] inline bool page_pt_to_screen(const PageView& view, const PageGeom& geom, int rot,
                                            float x_pt, float y_pt,
                                            float& sx, float& sy) noexcept {
    if (!(geom.w_pt > 0.0f) || !(geom.h_pt > 0.0f)) return false;
    if (!(view.w > 0.0f) || !(view.h > 0.0f)) return false;
    float u = 0.0f, v = 0.0f;
    norm_forward(rot, x_pt / geom.w_pt, y_pt / geom.h_pt, u, v);
    sx = view.x + u * view.w;
    sy = view.y + v * view.h;
    return true;
}

// 屏幕点 → 页面 pt（未旋转）。
// **刻意不做"点在页外"的判定**：拖动选择需要"取最近字符"，指针拖出页面边界时
// 仍要能换算出一个（页外的）pt 才能算出最近字符。页内/页外由调用方按归一化值判断。
[[nodiscard]] inline bool screen_to_page_pt(const PageView& view, const PageGeom& geom, int rot,
                                            float sx, float sy,
                                            float& x_pt, float& y_pt) noexcept {
    if (!(view.w > 0.0f) || !(view.h > 0.0f)) return false;
    if (!(geom.w_pt > 0.0f) || !(geom.h_pt > 0.0f)) return false;
    float nu = 0.0f, nv = 0.0f;
    norm_inverse(rot, (sx - view.x) / view.w, (sy - view.y) / view.h, nu, nv);
    x_pt = nu * geom.w_pt;
    y_pt = nv * geom.h_pt;
    return true;
}

}  // namespace lr::app
