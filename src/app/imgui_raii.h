// imgui_raii.h — ImGui 栈操作的 RAII 包装（Phase 7 稳定性加固）
//
// 为什么要这一层：
//   ImGui 的 Push/Pop 与 Begin/End 是**手工配对**的栈操作。绘制中途一旦抛出 C++ 异常
//   （最常见的是拼串/容器增长触发的 std::bad_alloc），栈就留下脏项：此后每一帧的颜色、
//   样式、窗口、弹窗栈全部错位 —— 轻则界面错乱，重则在 ImGui 内部断言处崩溃。
//   靠 catch 事后"补 Pop"是不可能的：抛出点在哪、压了几层，catch 处无从得知。
//
// 这一层把每一对操作包成**词法作用域对象**：构造即 Push/Begin，析构即 Pop/End。
// 于是栈的收口交给 C++ 的栈展开保证 —— 只要对象活在作用域里，无论正常返回、提前 return
// 还是异常展开，配对都必然发生。try/catch 于是只需要"把错误截住并转成状态"。
//
// 两类语义（**必须分清**，混用就是新的 bug 来源）：
//   · **无条件配对**：Begin() 无返回值、或必须始终 End。
//     Window / Child / Disabled / 全部 Push*（StyleVar/StyleColor/Font/ID/ItemWidth/
//     TextWrapPos/ClipRect）。
//   · **条件配对**：只有 Begin* 返回 true 才允许 End。
//     Menu / Popup / PopupModal / PopupContextWindow / TabBar / TabItem / Table / TreeNode / Combo / Tooltip。
//     这类对象用 `explicit operator bool()` 表达"是否可见/展开"，写法与原生一致：
//         if (auto m = ig::Menu("视图")) { ... }
//
// **dismiss() 的作用**：有些 Push 只需要覆盖紧邻的一次调用（最典型的是
// `PushStyleVar(WindowPadding, 0)` + `BeginChild` + 立刻 `PopStyleVar()` —— 零内边距只该
// 影响这个子窗口的 Begin，不该泄漏给子窗口里后续弹出的菜单）。这类"提前收口"用
// `dismiss()` 表达，语义与手工 Pop 完全一致，且仍然享受"异常时必定收口"的保证。
//
// 约束：包装器全部 noexcept、不分配内存、不写日志、不参与逻辑判断
//       （除了 operator bool 如实转达 Begin* 的返回值）。它们会出现在每帧路径上，
//       任何额外开销都是不可接受的。
//
// 用法：**不要**用 `auto&` 或拷贝持有这些对象 —— 它们不可拷贝也不可移动；
//       `auto x = ig::Xxx(...)` 与 `if (auto x = ig::Xxx(...))` 依赖 C++17 的强制复制消除，
//       是唯一受支持的用法。

#pragma once

#include "imgui.h"

namespace lr::app::ig {

// 不可拷贝、不可移动：保证析构恰好一次，也就保证 Pop/End 恰好一次。
#define LR_IG_NOCOPY(Type)                 \
    Type(const Type&) = delete;            \
    Type& operator=(const Type&) = delete; \
    Type(Type&&) = delete;                 \
    Type& operator=(Type&&) = delete

// 统一的"配对一次"骨架：dismiss() 显式收口，析构兜底。二者共享 done_ 保证只执行一次。
// dismiss() 是 const 的：这些对象在调用点一律以 `const ig::Xxx x = ...` 形式声明（强制
// 复制消除），让"提前收口"也能作用在 const 对象上；done_ 相应为 mutable。
#define LR_IG_GUARD(Type, PopExpr)                                       \
    Type(const Type&) = delete;                                          \
    Type& operator=(const Type&) = delete;                               \
    Type(Type&&) = delete;                                               \
    Type& operator=(Type&&) = delete;                                    \
    void dismiss() const noexcept {                                      \
        if (!done_) {                                                    \
            done_ = true;                                                \
            PopExpr;                                                     \
        }                                                                \
    }                                                                    \
    ~Type() noexcept { dismiss(); }                                      \
                                                                         \
private:                                                                 \
    mutable bool done_ = false;                                          \
                                                                         \
public:

// 样式值的载体：让 StyleVar 家族接受"float 与 ImVec2 的任意混搭"，而不必为每种
// 排列组合写一个构造函数。float / ImVec2 各自隐式转换为本类型。
namespace detail {
struct StyleVal {
    enum class Kind : unsigned char { F, V };
    Kind   kind;
    float  f;
    ImVec2 v;
    StyleVal(float x) noexcept : kind(Kind::F), f(x), v{} {}
    StyleVal(const ImVec2& x) noexcept : kind(Kind::V), f(0.0f), v(x) {}
};

inline void push_style_var(int idx, const StyleVal& s) noexcept {
    if (s.kind == StyleVal::Kind::F) ImGui::PushStyleVar(idx, s.f);
    else                             ImGui::PushStyleVar(idx, s.v);
}
}  // namespace detail

// ============================================================================
// 无条件配对：Push* / Pop*
// ============================================================================

struct StyleVar {
    StyleVar(int idx, detail::StyleVal v) noexcept { detail::push_style_var(idx, v); }
    LR_IG_GUARD(StyleVar, ImGui::PopStyleVar())
};

struct StyleVar2 {
    StyleVar2(int i0, detail::StyleVal v0, int i1, detail::StyleVal v1) noexcept {
        detail::push_style_var(i0, v0);
        detail::push_style_var(i1, v1);
    }
    LR_IG_GUARD(StyleVar2, ImGui::PopStyleVar(2))
};

struct StyleVar3 {
    StyleVar3(int i0, detail::StyleVal v0, int i1, detail::StyleVal v1,
              int i2, detail::StyleVal v2) noexcept {
        detail::push_style_var(i0, v0);
        detail::push_style_var(i1, v1);
        detail::push_style_var(i2, v2);
    }
    LR_IG_GUARD(StyleVar3, ImGui::PopStyleVar(3))
};

struct StyleColor {
    StyleColor(ImGuiCol idx, ImU32 col) noexcept { ImGui::PushStyleColor(idx, col); }
    StyleColor(ImGuiCol idx, const ImVec4& col) noexcept { ImGui::PushStyleColor(idx, col); }
    LR_IG_GUARD(StyleColor, ImGui::PopStyleColor())
};

struct StyleColor3 {
    StyleColor3(ImGuiCol i0, const ImVec4& c0, ImGuiCol i1, const ImVec4& c1,
                ImGuiCol i2, const ImVec4& c2) noexcept {
        ImGui::PushStyleColor(i0, c0);
        ImGui::PushStyleColor(i1, c1);
        ImGui::PushStyleColor(i2, c2);
    }
    LR_IG_GUARD(StyleColor3, ImGui::PopStyleColor(3))
};

struct StyleColor4 {
    StyleColor4(ImGuiCol i0, const ImVec4& c0, ImGuiCol i1, const ImVec4& c1,
                ImGuiCol i2, const ImVec4& c2, ImGuiCol i3, const ImVec4& c3) noexcept {
        ImGui::PushStyleColor(i0, c0);
        ImGui::PushStyleColor(i1, c1);
        ImGui::PushStyleColor(i2, c2);
        ImGui::PushStyleColor(i3, c3);
    }
    LR_IG_GUARD(StyleColor4, ImGui::PopStyleColor(4))
};

// PushFont(font, size)：size = 0 保持当前字号；font = nullptr 保持当前字体。
struct Font {
    Font(ImFont* f, float size_base_unscaled) noexcept { ImGui::PushFont(f, size_base_unscaled); }
    explicit Font(ImFont* f) noexcept { ImGui::PushFont(f, 0.0f); }
    LR_IG_GUARD(Font, ImGui::PopFont())
};

struct Id {
    explicit Id(int id) noexcept { ImGui::PushID(id); }
    explicit Id(const void* id) noexcept { ImGui::PushID(id); }
    explicit Id(const char* id) noexcept { ImGui::PushID(id); }
    Id(const char* begin, const char* end) noexcept { ImGui::PushID(begin, end); }
    LR_IG_GUARD(Id, ImGui::PopID())
};

struct ItemWidth {
    explicit ItemWidth(float w) noexcept { ImGui::PushItemWidth(w); }
    LR_IG_GUARD(ItemWidth, ImGui::PopItemWidth())
};

struct TextWrapPos {
    explicit TextWrapPos(float wrap_local_pos_x = 0.0f) noexcept {
        ImGui::PushTextWrapPos(wrap_local_pos_x);
    }
    LR_IG_GUARD(TextWrapPos, ImGui::PopTextWrapPos())
};

// ImDrawList 的裁剪栈（自绘画布用）。必须把**同一个** draw list 传进来。
struct ClipRect {
    ClipRect(ImDrawList* dl, const ImVec2& min, const ImVec2& max, bool intersect) noexcept
        : dl_(dl) {
        if (dl_) dl_->PushClipRect(min, max, intersect);
    }
    LR_IG_GUARD(ClipRect, if (dl_) dl_->PopClipRect())

private:
    ImDrawList* dl_ = nullptr;
};

// ============================================================================
// 无条件配对：Begin/End（返回值只表达"是否可见"，不影响配对）
// ============================================================================

struct Window {
    Window(const char* name, bool* p_open, ImGuiWindowFlags flags) noexcept
        : visible_(ImGui::Begin(name, p_open, flags)) {}
    LR_IG_GUARD(Window, ImGui::End())
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] explicit operator bool() const noexcept { return visible_; }

private:
    bool visible_;
};

struct Child {
    explicit Child(const char* str_id, const ImVec2& size = ImVec2(0, 0),
                   ImGuiChildFlags child_flags = 0, ImGuiWindowFlags window_flags = 0) noexcept
        : visible_(ImGui::BeginChild(str_id, size, child_flags, window_flags)) {}
    LR_IG_GUARD(Child, ImGui::EndChild())
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] explicit operator bool() const noexcept { return visible_; }

private:
    bool visible_;
};

struct Disabled {
    explicit Disabled(bool disabled = true) noexcept { ImGui::BeginDisabled(disabled); }
    LR_IG_GUARD(Disabled, ImGui::EndDisabled())
};

// ============================================================================
// 条件配对：只有 Begin* 返回 true 才 End
// ============================================================================

// 把"只有可见才配对"的重复代码收成一个小模板：End 用传入的仿函数。
namespace detail {

template <class EndFn>
struct Conditional {
    explicit Conditional(bool visible, EndFn end) noexcept : end_(end) {
        if (visible) active_ = true;
    }
    void dismiss() const noexcept {
        if (active_) {
            active_ = false;
            end_();
        }
    }
    ~Conditional() noexcept { dismiss(); }

    Conditional(const Conditional&) = delete;
    Conditional& operator=(const Conditional&) = delete;
    Conditional(Conditional&&) = delete;
    Conditional& operator=(Conditional&&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return active_; }

private:
    mutable bool active_ = false;
    EndFn end_;
};

struct EndMenuFn    { void operator()() const noexcept { ImGui::EndMenu(); } };
struct EndPopupFn   { void operator()() const noexcept { ImGui::EndPopup(); } };
struct EndTabBarFn  { void operator()() const noexcept { ImGui::EndTabBar(); } };
struct EndTabItemFn { void operator()() const noexcept { ImGui::EndTabItem(); } };
struct EndTableFn   { void operator()() const noexcept { ImGui::EndTable(); } };
struct EndComboFn   { void operator()() const noexcept { ImGui::EndCombo(); } };
struct TreePopFn    { void operator()() const noexcept { ImGui::TreePop(); } };
struct EndTooltipFn { void operator()() const noexcept { ImGui::EndTooltip(); } };

}  // namespace detail

struct Menu {
    explicit Menu(const char* label, bool enabled = true) noexcept
        : c_(ImGui::BeginMenu(label, enabled), detail::EndMenuFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndMenuFn> c_;
};

struct Popup {
    explicit Popup(const char* str_id, ImGuiWindowFlags flags = 0) noexcept
        : c_(ImGui::BeginPopup(str_id, flags), detail::EndPopupFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndPopupFn> c_;
};

struct PopupModal {
    explicit PopupModal(const char* name, bool* p_open = nullptr,
                        ImGuiWindowFlags flags = 0) noexcept
        : c_(ImGui::BeginPopupModal(name, p_open, flags), detail::EndPopupFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndPopupFn> c_;
};

struct PopupContextWindow {
    explicit PopupContextWindow(const char* str_id = nullptr,
                                ImGuiPopupFlags popup_flags = ImGuiPopupFlags_MouseButtonRight) noexcept
        : c_(ImGui::BeginPopupContextWindow(str_id, popup_flags), detail::EndPopupFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndPopupFn> c_;
};

struct TabBar {
    explicit TabBar(const char* str_id, ImGuiTabBarFlags flags = 0) noexcept
        : c_(ImGui::BeginTabBar(str_id, flags), detail::EndTabBarFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndTabBarFn> c_;
};

struct TabItem {
    explicit TabItem(const char* label, bool* p_open = nullptr,
                     ImGuiTabItemFlags flags = 0) noexcept
        : c_(ImGui::BeginTabItem(label, p_open, flags), detail::EndTabItemFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndTabItemFn> c_;
};

struct Table {
    explicit Table(const char* str_id, int columns, ImGuiTableFlags flags = 0,
                   const ImVec2& outer_size = ImVec2(0.0f, 0.0f), float inner_width = 0.0f) noexcept
        : c_(ImGui::BeginTable(str_id, columns, flags, outer_size, inner_width),
             detail::EndTableFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndTableFn> c_;
};

// TreeNode / TreeNodeEx：返回 true = 已展开，析构时 TreePop（等价于原生配对）。
// 注意：使用 ImGuiTreeNodeFlags_NoTreePushOnOpen / Leaf 时原生**不应**调用 TreePop，
// 这几种情形请继续直接用 ImGui::TreeNodeEx，不要套这个包装。
struct TreeNode {
    TreeNode(const char* label, ImGuiTreeNodeFlags flags = 0) noexcept
        : c_(ImGui::TreeNodeEx(label, flags), detail::TreePopFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::TreePopFn> c_;
};

struct Combo {
    explicit Combo(const char* label, const char* preview_value,
                   ImGuiComboFlags flags = 0) noexcept
        : c_(ImGui::BeginCombo(label, preview_value, flags), detail::EndComboFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndComboFn> c_;
};

struct Tooltip {
    Tooltip() noexcept : c_(ImGui::BeginTooltip(), detail::EndTooltipFn{}) {}
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(c_); }

private:
    detail::Conditional<detail::EndTooltipFn> c_;
};

#undef LR_IG_GUARD
#undef LR_IG_NOCOPY

}  // namespace lr::app::ig
