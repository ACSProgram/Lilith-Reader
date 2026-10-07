// view_motion.cpp — Lilith Reader 应用层：视图动效与滚动/缩放入口（ADR-047）
//
// 从 session.cpp 拆出（ADR-087）。设计：**动效只发生在 app 层写入画布之前**，
// 画布保持"状态唯一真源 + 纯函数布局"，不引入任何时间/插值概念。
// 本 TU 是全部滚动/缩放意图的唯一入口：request_scroll / request_jump_scroll /
// zoom_*_animated / fit_to_width_animated / scroll_by_rows，以及每帧推进 step_view_motion。
//
// 动效状态仍在 AppContext（scroll_pending / zoom_* / jump_repin_*，ADR-086）。

#include "app_internal.h"

namespace lr::app {

// ---------------- 视图动效（ADR-047） ----------------
//
// 设计：**动效只发生在 app 层写入画布之前**。画布保持"状态唯一真源 + 纯函数布局"，
// 不引入任何时间/插值概念，`canvas_test` 的断言与"滚动不重算布局"的 O(1) 性质都不受影响。
//
// 三条路径（统一由 [ui] Motion 开关，关掉即等价于动效前的直切行为）：
//   · 滚轮 / 方向键 / 整屏滚动 → request_scroll()：增量进 g_app.scroll_pending，逐帧一阶滞后消耗；
//   · 显式跳页（跳页框/目录/书签/缩略图/翻行）→ request_jump_scroll()：让画布先算出目标位置
//     （含对开/网格/钳制的唯一真源），再回退到起点交给动效滑行；
//   · 缩放 → zoom_*_animated()：显示倍率指数趋近目标倍率，锚点固定不动。

// 滑行到目标滚动位置：起点/终点由调用方用画布算好（本文件内部使用）。
// 末尾要重新钉游标的原因见 app_internal.h 的 g_app.jump_repin_page 注释。
namespace {
void start_scroll_glide(float from, float to, int repin_page, float repin_align) {
    if (!g_app.prefs.motion || std::fabs(to - from) < 1.0f) {
        g_app.scroll_pending = 0.0f;
        g_app.jump_repin_page = -1;
        return;
    }
    g_app.canvas.scroll_by(0.0f, from - to);  // 回退到起点（scroll_by 会把游标同步回视口顶部）
    g_app.scroll_pending = to - from;
    g_app.jump_repin_page = repin_page;
    g_app.jump_repin_align = repin_align;
}
}  // namespace

void request_scroll(float delta_px) {
    // 新的滚动意图取消未完成的跳页滑行：否则滑行会把用户刚滚到的位置又拽回目标页。
    g_app.jump_repin_page = -1;
    g_app.scroll_pending += delta_px;
}

void request_jump_scroll(int page, float align) {
    const float from = g_app.canvas.state().scroll_y;
    g_app.canvas.scroll_to_page(page, align);
    start_scroll_glide(from, g_app.canvas.state().scroll_y, page, align);
}

void zoom_to_animated(float z, float anchor_sx, float anchor_sy) {
    z = std::clamp(z, lr::kMinZoom, lr::kMaxZoom);
    g_app.zoom_anchor_x = anchor_sx;
    g_app.zoom_anchor_y = anchor_sy;
    g_app.zoom_end_fit = false;
    if (!g_app.prefs.motion) {
        g_app.zoom_anim = false;
        g_app.canvas.set_zoom(z, anchor_sx, anchor_sy);
        return;
    }
    if (std::fabs(z - view_zoom_target()) < 1e-4f) return;  // 目标未变：不重启动画
    g_app.zoom_anim = true;
    g_app.zoom_to = z;
}

void zoom_by_animated(float factor, float anchor_sx, float anchor_sy) {
    if (!(factor > 0.0f)) return;
    // 基于**目标倍率**累乘：动画进行中若基于显示值，连续滚轮的倍率会被滞后吞掉
    zoom_to_animated(view_zoom_target() * factor, anchor_sx, anchor_sy);
}

// 适合宽度：让画布先算派生倍率（fit 公式的唯一真源），再回退到当前倍率交给动效，
// 动画结束时重新置 fit（幂等；期间显示的是插值倍率）。
void fit_to_width_animated() {
    if (!g_app.prefs.motion) {
        g_app.zoom_anim = false;
        g_app.zoom_end_fit = false;
        g_app.canvas.fit_to_width();
        return;
    }
    const lr::CanvasState st = g_app.canvas.state();
    const float from = g_app.canvas.effective_zoom();
    g_app.canvas.fit_to_width();
    const float to = g_app.canvas.effective_zoom();
    if (std::fabs(to - from) < 1e-4f) return;  // 已在 fit 状态：无需动画
    g_app.canvas.set_state(st);                    // 回退（含 fit_width 标志）
    g_app.zoom_anim = true;
    g_app.zoom_to = to;
    g_app.zoom_anchor_x = g_app.canvas.viewport_w() * 0.5f;
    g_app.zoom_anchor_y = g_app.canvas.viewport_h() * 0.5f;
    g_app.zoom_end_fit = true;
}

float view_zoom_target() {
    return g_app.zoom_anim ? g_app.zoom_to : g_app.canvas.effective_zoom();
}

// 每帧推进（在画布输入处理之后、渲染请求之前调用）
void step_view_motion(float dt) {
    // 1) 待定滚动：一阶滞后消耗
    if (g_app.scroll_pending != 0.0f) {
        if (!g_app.prefs.motion) {
            g_app.canvas.scroll_by(0.0f, g_app.scroll_pending);
            g_app.scroll_pending = 0.0f;
        } else {
            const float step = g_app.scroll_pending * (1.0f - std::exp(-kScrollSmoothRate * dt));
            g_app.canvas.scroll_by(0.0f, step);
            g_app.scroll_pending -= step;
            if (std::fabs(g_app.scroll_pending) < kMotionEpsPx) g_app.scroll_pending = 0.0f;
        }
        if (g_app.scroll_pending == 0.0f && g_app.jump_repin_page >= 0) {
            g_app.canvas.scroll_to_page(g_app.jump_repin_page, g_app.jump_repin_align);
            g_app.jump_repin_page = -1;
        }
    }

    // 2) 缩放插值：锚点固定，显示倍率趋近目标倍率（收敛后吸附到目标，避免残留亚像素差）
    if (g_app.zoom_anim) {
        const float target = g_app.zoom_to;
        const bool done = !g_app.prefs.motion ||
                          std::fabs(g_app.canvas.effective_zoom() - target) <= 1e-3f * target;
        g_app.canvas.set_zoom(done ? target
                               : approach(g_app.canvas.effective_zoom(), target, kZoomSmoothRate, dt),
                          g_app.zoom_anchor_x, g_app.zoom_anchor_y);
        if (done) {
            g_app.zoom_anim = false;
            if (g_app.zoom_end_fit) { g_app.zoom_end_fit = false; g_app.canvas.fit_to_width(); }
        }
    }
}

// 翻行/翻页的几何计算全在画布层（纯函数、可单测，见 canvas_test 的 scroll_rows 用例）。
// UI 层只做转发：不再用 visible_first() 自行推算目标行 —— 那正是"视口高于一行时末页反复
// 卡住"的根因（已由阅读游标修掉，见 ADR-023 与 canvas.ixx）。
// 动效（ADR-047）：翻行同样是一次"显式跳页"，故先记下起点、让画布跳到目标，再回退滑行。
void scroll_by_rows(int dir) {
    const float from = g_app.canvas.state().scroll_y;
    g_app.canvas.scroll_rows(dir);
    start_scroll_glide(from, g_app.canvas.state().scroll_y, g_app.canvas.current_page(), 0.0f);
}

}  // namespace lr::app
