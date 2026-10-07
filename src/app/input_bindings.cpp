// input_bindings.cpp — Lilith Reader 应用层：命令表与按键绑定（ADR-054）
//
// 从 session.cpp 拆出（ADR-087）。职责：
//   · 命令表 kCmds：每条命令的 id / 分组 / 显示名 / 是否连发 / 是否全局 / 默认两个槽；
//   · 按键解析与序列化：chord_to_string / chord_label / chord_from_string；
//   · 派发判定：cmd_pressed（修饰键严格匹配）、wheel_zoom_mod_held；
//   · 改键捕获与冲突检测：update_key_capture / find_bind_conflict。
//
// 绑定状态仍在 AppContext（g_app.binds / capture_*，ADR-086），本 TU 只读写它；
// ini 读写由 platform.cpp 的 load_binds / save_binds 提供。

#include "app_internal.h"

namespace lr::app {

// ---------------- 命令与按键绑定（ADR-054） ----------------

namespace {

// 构造键组合：ImGuiKey | ImGuiMod_*（与 ImGui::GetKeyChordName 同一编码）。
constexpr ImGuiKeyChord kb(ImGuiKey k, int mods = 0) {
    return static_cast<ImGuiKeyChord>(static_cast<int>(k) | mods);
}

}  // namespace

// 命令表：默认键、是否连发、是否全局可用。**改显示文案不得改 id**（id 是 ini 键名）。
// · 数字/算术类给两个槽（主键盘 + 小键盘）：落实 ADR-027"两套都判"，不再写别名。
// · 全屏给 F11 + Esc 两个槽：Esc 不再承担"关闭文档/退出"（误触代价过大，关闭走菜单），
//   改作可绑定键，默认与 F11 同为全屏（ADR-054）。
const CmdDef kCmds[kCmdCount] = {
    // id             group   name                        repeat global  def0                        def1
    { "NextRow",        "导航", "下一行 / 下一页",           false, false, kb(ImGuiKey_RightArrow),     ImGuiKey_None },
    { "PrevRow",        "导航", "上一行 / 上一页",           false, false, kb(ImGuiKey_LeftArrow),      ImGuiKey_None },
    { "ScrollDown",     "导航", "向下滚动",                  true,  false, kb(ImGuiKey_DownArrow),      ImGuiKey_None },
    { "ScrollUp",       "导航", "向上滚动",                  true,  false, kb(ImGuiKey_UpArrow),        ImGuiKey_None },
    { "PageDown",       "导航", "向下翻屏",                  true,  false, kb(ImGuiKey_PageDown),       ImGuiKey_None },
    { "PageUp",         "导航", "向上翻屏",                  true,  false, kb(ImGuiKey_PageUp),         ImGuiKey_None },
    { "FirstPage",      "导航", "首页",                      false, false, kb(ImGuiKey_Home),           ImGuiKey_None },
    { "LastPage",       "导航", "末页",                      false, false, kb(ImGuiKey_End),            ImGuiKey_None },
    { "JumpPage",       "导航", "跳转页码",                  false, false, kb(ImGuiKey_G),              ImGuiKey_None },

    { "ZoomIn",         "缩放", "放大",                      true,  false, kb(ImGuiKey_Equal),          kb(ImGuiKey_KeypadAdd) },
    { "ZoomOut",        "缩放", "缩小",                      true,  false, kb(ImGuiKey_Minus),          kb(ImGuiKey_KeypadSubtract) },
    { "FitWidth",       "缩放", "适合宽度",                  false, false, kb(ImGuiKey_F),              ImGuiKey_None },
    // 修饰键型：绑定的是"按住不放的键"（ImGuiMod_*），不是一条命令。
    // 末位 mod_only=true → 按键分栏渲染成下拉框，不走捕获；派发由 handle_canvas_input 读。
    { "ZoomWheelMod",   "缩放", "滚轮缩放",                  false, false, ImGuiMod_Ctrl,               ImGuiKey_None, true },

    { "Col1",           "视图", "单页",                      false, false, kb(ImGuiKey_1),              kb(ImGuiKey_Keypad1) },
    { "Col2",           "视图", "双页",                      false, false, kb(ImGuiKey_2),              kb(ImGuiKey_Keypad2) },
    { "Col3",           "视图", "三页",                      false, false, kb(ImGuiKey_3),              kb(ImGuiKey_Keypad3) },
    { "Col4",           "视图", "四页",                      false, false, kb(ImGuiKey_4),              kb(ImGuiKey_Keypad4) },
    { "ToggleSpread",   "视图", "双页对开（书籍模式）",      false, false, kb(ImGuiKey_D),              ImGuiKey_None },
    { "RotateCW",       "视图", "旋转 90°",                  false, false, kb(ImGuiKey_R),              ImGuiKey_None },
    // id 保持 Phase 5 的历史名（"ToggleInvert"/"ToggleSepia"）：它是 ini 的**稳定标识**，
    // 改名会让用户已自定义的 I / E 绑定找不到而退回默认值。语义已在 ADR-067 里改过，
    // 显示名（第三列）随之更新，标识不动。
    { "ToggleInvert",   "视图", "深色纸张",                  false, false, kb(ImGuiKey_I),              ImGuiKey_None },
    { "ToggleSepia",    "视图", "暖色",                      false, false, kb(ImGuiKey_E),              ImGuiKey_None },

    { "ToggleSidebar",  "界面", "侧栏（目录 / 书签 / 缩略图）", false, false, kb(ImGuiKey_O),           ImGuiKey_None },
    { "ToggleBookmark", "界面", "当前页书签增删",            false, false, kb(ImGuiKey_B),              ImGuiKey_None },
    { "OpenSettings",   "界面", "设置",                      false, true,  kb(ImGuiKey_Comma, ImGuiMod_Ctrl), ImGuiKey_None },
    { "OpenKeys",       "界面", "按键设置",                  false, true,  kb(ImGuiKey_F1),             ImGuiKey_None },
    { "ToggleDebug",    "界面", "调试浮层",                  false, true,  kb(ImGuiKey_F3),             ImGuiKey_None },
    { "ToggleFullscreen","界面","全屏",                      false, true,  kb(ImGuiKey_F11),            kb(ImGuiKey_Escape) },
    { "OpenFile",       "界面", "打开文档…",                 false, true,  kb(ImGuiKey_O, ImGuiMod_Ctrl), ImGuiKey_None },

    // Phase 8：文本。三条都**非全局**（global=false）—— 它们只在阅读态、且画布不在
    // 文本输入中时派发（见 handle_canvas_input 的 io.WantTextInput 门）：
    // 搜索框获得焦点时 Ctrl+C/Ctrl+A 必须归输入框，不能被这里抢走。
    { "Copy",           "文本", "复制选中文本",              false, false, kb(ImGuiKey_C, ImGuiMod_Ctrl), ImGuiKey_None },
    { "SelectAll",      "文本", "全选当前页文本",            false, false, kb(ImGuiKey_A, ImGuiMod_Ctrl), ImGuiKey_None },
    { "OpenSearch",     "文本", "查找",                      false, false, kb(ImGuiKey_F, ImGuiMod_Ctrl), ImGuiKey_None },
};

void reset_binds_to_default() {
    for (int i = 0; i < kCmdCount; ++i) {
        g_app.binds[i][0] = kCmds[i].def0;
        g_app.binds[i][1] = kCmds[i].def1;
    }
}

namespace {

// 单个键组合是否在本帧按下：修饰键**严格匹配**（没绑 Ctrl 时按住 Ctrl 不触发），
// 与 ImGui::IsKeyChordPressed 同一语义，但这里自己判以便控制"连发"。
bool chord_pressed(ImGuiKeyChord c, bool repeat) {
    if (c == ImGuiKey_None) return false;
    const ImGuiKey mods = static_cast<ImGuiKey>(c & ImGuiMod_Mask_);
    if (ImGui::GetIO().KeyMods != mods) return false;
    const ImGuiKey key = static_cast<ImGuiKey>(c & ~ImGuiMod_Mask_);
    if (key == ImGuiKey_None) return false;
    return ImGui::IsKeyPressed(key, repeat);
}

}  // namespace

bool cmd_pressed(Cmd c) {
    if (g_app.capture_cmd < kCmdCount) return false;   // 捕获中：不派发任何命令
    const int i = static_cast<int>(c);
    const bool rep = kCmds[i].repeat;
    return chord_pressed(g_app.binds[i][0], rep) || chord_pressed(g_app.binds[i][1], rep);
}

// 修饰键/鼠标/手柄不参与键盘绑定（捕获与解析都过滤）。
bool is_bindable_key(ImGuiKey k) {
    if (k < ImGuiKey_NamedKey_BEGIN || k >= ImGuiKey_NamedKey_END) return false;
    switch (k) {
    case ImGuiKey_LeftCtrl: case ImGuiKey_LeftShift: case ImGuiKey_LeftAlt: case ImGuiKey_LeftSuper:
    case ImGuiKey_RightCtrl: case ImGuiKey_RightShift: case ImGuiKey_RightAlt: case ImGuiKey_RightSuper:
    case ImGuiKey_ReservedForModCtrl: case ImGuiKey_ReservedForModShift:
    case ImGuiKey_ReservedForModAlt: case ImGuiKey_ReservedForModSuper:
        return false;
    default:
        break;
    }
    if (k >= ImGuiKey_MouseLeft && k <= ImGuiKey_MouseWheelY) return false;
    if (k >= ImGuiKey_GamepadStart && k <= ImGuiKey_GamepadRStickDown) return false;
    return true;
}

// ini 存储用：纯 ASCII 的 "Ctrl+Shift+RightArrow"。键名取 ImGui::GetKeyName 原文，
// 保证 load 时能按名反查（不受显示美化影响）。
// 修饰键型命令（mod_only）没有主键：只写修饰键本身（"Ctrl" / "Ctrl+Alt"），空 = 无。
std::string chord_to_string(ImGuiKeyChord c) {
    if (c == ImGuiKey_None) return {};
    std::string s;
    if (c & ImGuiMod_Ctrl)  s += "Ctrl+";
    if (c & ImGuiMod_Shift) s += "Shift+";
    if (c & ImGuiMod_Alt)   s += "Alt+";
    const ImGuiKey key = static_cast<ImGuiKey>(c & ~ImGuiMod_Mask_);
    if (key == ImGuiKey_None) {          // 只有修饰键：去掉末尾的 '+'
        if (!s.empty()) s.pop_back();
        return s;
    }
    s += ImGui::GetKeyName(key);
    return s;
}

// 显示用：把 ImGui 的 US 键名换成更直观的符号（"Ctrl+Equal" → "Ctrl+="）。
std::string chord_label(ImGuiKeyChord c) {
    if (c == ImGuiKey_None) return {};
    const std::string s = chord_to_string(c);
    static const struct { const char* from; const char* to; } kMap[] = {
        { "Equal", "=" }, { "Minus", "-" }, { "Comma", "," }, { "Period", "." },
        { "Slash", "/" }, { "Semicolon", ";" }, { "Apostrophe", "'" },
        { "LeftBracket", "[" }, { "RightBracket", "]" }, { "Backslash", "\\" },
        { "GraveAccent", "`" }, { "Escape", "Esc" }, { "Space", "空格" }, { "Enter", "回车" },
        { "RightArrow", "→" }, { "LeftArrow", "←" }, { "UpArrow", "↑" }, { "DownArrow", "↓" },
        { "PageUp", "PgUp" }, { "PageDown", "PgDn" },
        { "KeypadAdd", "小键盘 +" }, { "KeypadSubtract", "小键盘 -" },
    };
    for (const auto& m : kMap) {
        const std::string suffix = std::string("+") + m.from;
        if (s.size() >= suffix.size() &&
            s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0)
            return s.substr(0, s.size() - std::strlen(m.from)) + m.to;
    }
    return s;
}

ImGuiKeyChord chord_from_string(const std::string& s) {
    if (s.empty()) return ImGuiKey_None;
    int mods = 0;
    std::string key_name;
    std::size_t start = 0;
    for (;;) {
        const std::size_t p = s.find('+', start);
        const std::string tok = (p == std::string::npos) ? s.substr(start) : s.substr(start, p - start);
        // 末段也要先判修饰键名："Ctrl" / "Ctrl+Alt" 是修饰键型命令的合法存储串
        // （它们没有主键，整串由修饰键组成），不这样判会被当键名去做反查而落空。
        if (p == std::string::npos) {
            if (tok == "Ctrl")       mods |= ImGuiMod_Ctrl;
            else if (tok == "Shift") mods |= ImGuiMod_Shift;
            else if (tok == "Alt")   mods |= ImGuiMod_Alt;
            else                     key_name = tok;
            break;
        }
        if (tok == "Ctrl")       mods |= ImGuiMod_Ctrl;
        else if (tok == "Shift") mods |= ImGuiMod_Shift;
        else if (tok == "Alt")   mods |= ImGuiMod_Alt;
        else { key_name = tok; break; }   // 意外前缀：把剩下的整体当键名，交给反查判定
        start = p + 1;
    }
    // 只有修饰键（"Ctrl" / "Ctrl+Alt"）：修饰键型命令的绑定，直接返回修饰键集合。
    if (key_name.empty()) return static_cast<ImGuiKeyChord>(mods);
    // 按名反查：遍历具名键（量级 140，仅解析 ini 时调用，可接受）
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const char* n = ImGui::GetKeyName(static_cast<ImGuiKey>(k));
        if (n != nullptr && key_name == n)
            return static_cast<ImGuiKeyChord>(k | mods);
    }
    return ImGuiKey_None;   // 未知键名（改过 ImGui 版本等）：安全忽略，保留默认
}

int find_bind_conflict(int cmd, int slot) {
    const ImGuiKeyChord c = g_app.binds[cmd][slot];
    if (c == ImGuiKey_None) return -1;
    if (kCmds[cmd].mod_only) return -1;   // 修饰键型绑定不与"主键组合"同域，谈不上冲突
    for (int i = 0; i < kCmdCount; ++i) {
        if (i == cmd) continue;   // 同一命令的两个槽不算冲突
        if (kCmds[i].mod_only) continue;
        for (int s = 0; s < kBindSlots; ++s)
            if (g_app.binds[i][s] == c) return i;
    }
    return -1;
}

// 滚轮缩放的修饰键：取 ZoomWheelMod 的绑定（mods-only），与当前按住的修饰键**严格相等**。
// 未设置（无）→ 恒 false，滚轮只滚动。用"严格相等"的理由与 chord_pressed 一致：
// 绑了 Alt 之后按住 Ctrl+Alt 滚轮不该缩放，否则用户无法用 Ctrl+滚轮做别的事。
bool wheel_zoom_mod_held() {
    const ImGuiKeyChord want = g_app.binds[static_cast<int>(Cmd::ZoomWheelMod)][0] & ImGuiMod_Mask_;
    if (want == 0) return false;
    const ImGuiIO& io = ImGui::GetIO();
    int held = 0;
    if (io.KeyCtrl)  held |= ImGuiMod_Ctrl;
    if (io.KeyShift) held |= ImGuiMod_Shift;
    if (io.KeyAlt)   held |= ImGuiMod_Alt;
    return (held & ImGuiMod_Mask_) == want;
}

void update_key_capture() {
    if (g_app.capture_cmd >= kCmdCount) return;
    // 修饰键型命令不参与捕获（它在界面上是下拉框）；万一被置进来就立刻退出，避免卡住。
    if (kCmds[g_app.capture_cmd].mod_only) { g_app.capture_cmd = kCmdCount; return; }
    // 鼠标点别处 / 右键 → 取消（不写入）。跳过进入捕获的那一次点击所在帧。
    if (g_app.capture_frame >= 0 && ImGui::GetFrameCount() > g_app.capture_frame &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        g_app.capture_cmd = kCmdCount;
        return;
    }
    ImGuiIO& io = ImGui::GetIO();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (!is_bindable_key(key) || !ImGui::IsKeyPressed(key, false)) continue;
        ImGuiKeyChord c = static_cast<ImGuiKeyChord>(k);
        if (io.KeyCtrl)  c |= ImGuiMod_Ctrl;
        if (io.KeyShift) c |= ImGuiMod_Shift;
        if (io.KeyAlt)   c |= ImGuiMod_Alt;
        g_app.binds[g_app.capture_cmd][g_app.capture_slot] = c;
        g_app.capture_cmd = kCmdCount;
        save_binds();
        return;
    }
}

}  // namespace lr::app
