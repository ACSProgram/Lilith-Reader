// imgui_stacks.h — ImGui 内部栈的"平衡自检"（Phase 7 稳定性加固）
//
// 目的：**让错误可追溯**。
//   漏掉一次 Pop 或 End 不会当场出事 —— 它会在若干帧之后表现为莫名其妙的界面错乱，
//   等到那时已经查不出是哪一处漏的。这里把"帧首快照 → 帧尾比对"做成常规动作：
//   任何一处漏配对，都在**当帧**被点名（哪个栈、差几层），并写进日志。
//
// 为什么单开一个头而不是放进 imgui_raii.h：这里需要 imgui_internal.h（内部栈字段），
// 而 imgui_raii.h 面向每帧路径、只依赖公开头。把它隔开，就不会让内部头泄进所有 TU。
//
// 用法（Debug 构建）：
//     const auto base = ig::capture_stacks();   // new_frame 之后
//     ...
//     char diff[256] = {};
//     ig::format_stack_diff(base, diff, sizeof diff);
//     if (diff[0] != '\0') log::error("ui", diff);
//
// 说明：**不纳入** ImDrawList 的裁剪栈 —— 它是每个 draw list 各自的，而"当前可达的
// draw list"在帧首与帧尾不是同一个对象，比对没有意义。裁剪栈由 ig::ClipRect 保证配对，
// 且 ImGui 自身在 PopClipRect 下溢时会立刻断言。

#pragma once

#include "imgui.h"
#include "imgui_internal.h"

#include <cstddef>
#include <cstdio>

namespace lr::app::ig {

// 帧内会跨帧累积的栈（一旦失衡就会持续污染后续帧）。
struct StackDepths {
    int style_var = 0;
    int style_color = 0;
    int id = 0;
    int window = 0;
    int popup = 0;
    int tree = 0;
    int font = 0;
    int disabled = 0;
};

inline StackDepths capture_stacks() noexcept {
    StackDepths d;
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (g == nullptr) return d;
    d.style_var   = g->StyleVarStack.Size;
    d.style_color = g->ColorStack.Size;
    // IDStack 属于**当前窗口**（每个窗口一份，End() 会断言它已回到基线），不是 context 级。
    // **必须用 Read 版**：公开的 ImGui::GetCurrentWindow() 会顺手把当前窗口标记为
    // WriteAccessed=true。本函数在 NewFrame 之后、第一个 Begin 之前被调用，此时"当前窗口"
    // 是 ImGui 的隐式回退窗口（Debug##Default）—— 标了 WriteAccessed，EndFrame 就不再把它
    // 当作"未使用"隐藏，于是屏幕上多出一个 400×400 的空 "Debug" 窗口（实测踩过）。
    if (const ImGuiWindow* w = ImGui::GetCurrentWindowRead()) d.id = w->IDStack.Size;
    d.window      = g->CurrentWindowStack.Size;
    d.popup       = g->BeginPopupStack.Size;
    d.tree        = g->TreeNodeStack.Size;
    d.font        = g->FontStack.Size;
    d.disabled    = static_cast<int>(g->DisabledStackSize);
    return d;
}

// 与基准快照比对，把差异写进 out（空串 = 完全平衡）。
// 刻意只用 snprintf：不分配内存，因而真的 noexcept —— 它会在帧边界被调用，
// 不能因为"诊断代码自己抛异常"而把进程带走。
inline void format_stack_diff(const StackDepths& base, char* out, std::size_t cap) noexcept {
    if (out == nullptr || cap == 0) return;
    out[0] = '\0';
    const StackDepths now = capture_stacks();

    std::size_t used = 0;
    auto add = [&](const char* name, int before, int after) noexcept {
        if (before == after || used + 32 >= cap) return;
        const int n = std::snprintf(out + used, cap - used, "%s%s=%+d",
                                    used == 0 ? "" : " ", name, after - before);
        if (n > 0) used += static_cast<std::size_t>(n);
    };
    add("stylevar", base.style_var, now.style_var);
    add("stylecolor", base.style_color, now.style_color);
    add("id", base.id, now.id);
    add("window", base.window, now.window);
    add("popup", base.popup, now.popup);
    add("tree", base.tree, now.tree);
    add("font", base.font, now.font);
    add("disabled", base.disabled, now.disabled);

    if (used > 0) {
        // 追一句可读的结论，便于直接在日志里 grep "IMGUI STACK"。
        std::snprintf(out + used, cap - used, "  (IMGUI STACK LEAK)");
    }
}

// 两个快照是否完全一致（帧首基线 ↔ 帧尾实际）。
//
// **不要**用"各层是否为 0"当作收口判据：ImGui 在 NewFrame 里就会自动开一个隐式窗口
// （"Debug##Default"）并压入默认字体，于是帧首的 id / window / font 三层天然非 0。
// 真正的判据只能是"帧尾 == 帧首"，这也是本函数存在的理由。
inline bool stacks_match(const StackDepths& a, const StackDepths& b) noexcept {
    return a.style_var == b.style_var && a.style_color == b.style_color && a.id == b.id &&
           a.window == b.window && a.popup == b.popup && a.tree == b.tree && a.font == b.font &&
           a.disabled == b.disabled;
}

// 把 ImGui 自己检出并"可恢复"的错误接到我们的回调上。
// 这是"让错误可追溯"的关键一环：这类错误（栈不平、ID 冲突、Begin/End 误用）在 ImGui 内部
// 是**可恢复**的，若不接管就只能靠它自己的调试日志或弹窗，进程外无从查起。
// 该字段是内部字段（imgui_internal.h），故只在需要的地方引入本头。
using ErrorSink = void (*)(ImGuiContext*, void*, const char*);

inline void redirect_errors_to(ErrorSink sink, void* user_data) noexcept {
    if (ImGuiContext* g = ImGui::GetCurrentContext()) {
        g->ErrorCallback = sink;
        g->ErrorCallbackUserData = user_data;
    }
}

// ---- 帧级异常恢复（官方推荐的"异常后恢复"用法）----
//
// imgui.h 的说明原文是：record stack sizes with ErrorRecoveryStoreState(), disable assert,
// set log callback, recover with ErrorRecoveryTryToRecoverState()。这里只做最小必要的两步
// （记录 / 恢复）—— 断言与日志回调由 platform.cpp 统一配置。
//
// 用途：绘制中途抛出异常时，把 ImGui 的内部栈拉回帧首基线，让本帧仍能正常 EndFrame，
// 而不是把脏状态带到下一帧。
inline void store_frame_state(ImGuiErrorRecoveryState& out) noexcept {
    if (ImGui::GetCurrentContext() != nullptr) ImGui::ErrorRecoveryStoreState(&out);
}

inline void recover_frame_state(const ImGuiErrorRecoveryState& in) noexcept {
    if (ImGui::GetCurrentContext() != nullptr) ImGui::ErrorRecoveryTryToRecoverState(&in);
}

}  // namespace lr::app::ig
