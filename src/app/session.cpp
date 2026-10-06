// session.cpp — Lilith Reader 应用层：会话与状态
//
// 职责（把"用户在读什么、读到哪里"组织成一个状态机，是 UI 与渲染层之间的控制中枢）：
//   · 文档状态机（UiDoc）：本地拒绝 / 后台打开 / 阅读 / 失败 / 需密码
//   · 打开、关闭、认证结果的轮询与落定
//   · 阅读位置与书签的读写（reader_state.bin，ADR-034）
//   · 视图变换（旋转 / 配色）与逐页尺寸下发
//   · 画布输入映射、缩放防抖、渲染请求（可见页 + 方向感知预加载）
//
// 依赖：utils / canvas / render / reader_state + platform（px/偏好）；不直接 import document
// （经 render 层间接使用）。共享状态与函数声明见 app_internal.h。

#include "app_internal.h"

namespace lr::app {

// ---------------- 标题 ----------------

void update_title() {
    if (!g_hwnd) return;
    std::wstring title = kWindowTitle;
    if (!g_doc.path_w.empty()) {
        title += L" — ";
        title += lr::file_name_of(g_doc.path_w);
    }
    SetWindowTextW(g_hwnd, title.c_str());
}

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
    { "ToggleInvert",   "视图", "深色",                      false, false, kb(ImGuiKey_I),              ImGuiKey_None },
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
        g_binds[i][0] = kCmds[i].def0;
        g_binds[i][1] = kCmds[i].def1;
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
    if (g_capture_cmd < kCmdCount) return false;   // 捕获中：不派发任何命令
    const int i = static_cast<int>(c);
    const bool rep = kCmds[i].repeat;
    return chord_pressed(g_binds[i][0], rep) || chord_pressed(g_binds[i][1], rep);
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
    const ImGuiKeyChord c = g_binds[cmd][slot];
    if (c == ImGuiKey_None) return -1;
    if (kCmds[cmd].mod_only) return -1;   // 修饰键型绑定不与"主键组合"同域，谈不上冲突
    for (int i = 0; i < kCmdCount; ++i) {
        if (i == cmd) continue;   // 同一命令的两个槽不算冲突
        if (kCmds[i].mod_only) continue;
        for (int s = 0; s < kBindSlots; ++s)
            if (g_binds[i][s] == c) return i;
    }
    return -1;
}

// 滚轮缩放的修饰键：取 ZoomWheelMod 的绑定（mods-only），与当前按住的修饰键**严格相等**。
// 未设置（无）→ 恒 false，滚轮只滚动。用"严格相等"的理由与 chord_pressed 一致：
// 绑了 Alt 之后按住 Ctrl+Alt 滚轮不该缩放，否则用户无法用 Ctrl+滚轮做别的事。
bool wheel_zoom_mod_held() {
    const ImGuiKeyChord want = g_binds[static_cast<int>(Cmd::ZoomWheelMod)][0] & ImGuiMod_Mask_;
    if (want == 0) return false;
    const ImGuiIO& io = ImGui::GetIO();
    int held = 0;
    if (io.KeyCtrl)  held |= ImGuiMod_Ctrl;
    if (io.KeyShift) held |= ImGuiMod_Shift;
    if (io.KeyAlt)   held |= ImGuiMod_Alt;
    return (held & ImGuiMod_Mask_) == want;
}

void update_key_capture() {
    if (g_capture_cmd >= kCmdCount) return;
    // 修饰键型命令不参与捕获（它在界面上是下拉框）；万一被置进来就立刻退出，避免卡住。
    if (kCmds[g_capture_cmd].mod_only) { g_capture_cmd = kCmdCount; return; }
    // 鼠标点别处 / 右键 → 取消（不写入）。跳过进入捕获的那一次点击所在帧。
    if (g_capture_frame >= 0 && ImGui::GetFrameCount() > g_capture_frame &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
         ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        g_capture_cmd = kCmdCount;
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
        g_binds[g_capture_cmd][g_capture_slot] = c;
        g_capture_cmd = kCmdCount;
        save_binds();
        return;
    }
}

// ---------------- 视图变换 / 页尺寸 ----------------

// 把（可能已旋转的）页尺寸交给画布：90/270 交换宽高。
// 画布本身不感知旋转——旋转被折算成"页尺寸宽高互换"，布局/翻页/缩放全部复用。
void push_canvas_sizes() {
    const bool swap = (g_rotation == 90 || g_rotation == 270);
    auto conv = [swap](lr::PageSizePt s) {
        if (swap) { const float t = s.w; s.w = s.h; s.h = t; }
        return s;
    };
    const lr::PageSizePt def = conv(g_raw_default);
    g_canvas.set_default_size(def);
    if (g_raw_sizes.empty()) {
        g_canvas.set_uniform(g_doc.info.page_count, def);
    } else {
        std::vector<lr::PageSizePt> v;
        v.reserve(g_raw_sizes.size());
        for (const lr::PageSizePt& s : g_raw_sizes) v.push_back(conv(s));
        g_canvas.set_page_sizes(std::move(v));
    }
}

// 把当前旋转/配色下发给渲染层（变化即让全部纹理失效并整篇重渲）。
void apply_view_transform() {
    g_renderer->set_view_transform(g_rotation, static_cast<lr::PageScheme>(g_scheme));
}

// 直接设定旋转角（菜单按角度选）；与按键 R（+90 循环）共用同一套重排逻辑。
void set_rotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg == g_rotation) return;
    g_rotation = deg;
    const int anchor = g_canvas.current_page();
    push_canvas_sizes();                                   // 尺寸宽高互换 → 布局变化
    g_canvas.scroll_to_page(anchor < 0 ? 0 : anchor, 0.0f);
    apply_view_transform();
}

void rotate_view(int delta) { set_rotation(g_rotation + delta); }

// 直接设定纸张方案（0 原色 / 1 深色 / 2 暖色）；按键 I/E 走 toggle。
void set_scheme(int mode) {
    if (g_scheme == mode) return;
    g_scheme = mode;
    apply_view_transform();
}

void toggle_scheme(int mode) { set_scheme(g_scheme == mode ? 0 : mode); }

// ---------------- 阅读位置与书签 ----------------

void save_reading_state() {
    if (g_doc_key == 0 || g_doc.kind != UiDoc::Kind::Reading) return;
    lr::DocRecord& r = g_state.upsert(g_doc_key);
    const int page = g_canvas.current_page();
    r.page = page < 0 ? 0 : page;
    r.zoom = g_canvas.state().zoom;
    r.columns = g_canvas.state().columns;
    r.rotation = g_rotation;
    r.fit_width = g_canvas.state().fit_width;
    r.spread = g_canvas.state().spread;
    r.scheme = g_scheme;
    // 身份随每次落盘刷新：将来换键方案时能按位置重算，也能给管理窗口显示"上次在哪 / 还在哪"
    r.path_key = g_identity.path;
    r.page_count = g_identity.page_count;
    lr::remember_location(r, g_identity.path_u8);
    (void)lr::save_state(g_state_path, g_state);  // 书签在增删时已写入 g_state，这里不覆盖
}

// ---- 身份解析与阅读数据维护（ADR-062）----
//
// 打开成功时才解析：内容指纹由**工作线程**在打开前算好随 DocState 带回来（UI 线程不读
// 文件内容，ADR-009），页数也只在打开后才知道（页数不同 = 不是同一版，不能继承书签）。
void resolve_document_identity(std::uint64_t content_fp) {
    g_identity = lr::DocIdentity{};
    g_identity.content = content_fp;
    g_identity.path = lr::document_key(g_doc.path_w);   // 元数据调用，微秒级（ADR-009 取舍）
    g_identity.page_count = g_doc.info.page_count;
    g_identity.path_u8 = lr::wide_to_utf8(g_doc.path_w);

    const bool smart = g_prefs.smart_match != kSmartMatchOff;
    const lr::DocMatch m = lr::locate(g_state, g_identity, smart);
    if (m.index < 0) {
        g_doc_key = lr::primary_key(g_identity);   // 新文档：主键 = 指纹（取不到则路径键）
        return;
    }

    // 命中：**先沿用**（绝不因为询问流程丢进度），再按设置决定要不要问一句。
    const lr::DocRecord& hit = g_state.docs[static_cast<std::size_t>(m.index)].second;
    const std::string old_path = lr::last_location(hit);
    const int old_page = hit.page;
    const int old_marks = static_cast<int>(hit.bookmarks.size());
    g_doc_key = lr::adopt(g_state, static_cast<std::size_t>(m.index), g_identity);

    if (m.relocated && g_prefs.smart_match == kSmartMatchAsk) {
        char logged[64];
        std::snprintf(logged, sizeof logged, "第 %d 页 · %d 个书签", old_page + 1, old_marks);
        request_confirm(ConfirmKind::Relocate, "沿用这份阅读数据？",
                        "这份文档与库中已记录的一份内容相同。",
                        { { "原位置", old_path.empty() ? "(未知位置)" : old_path },
                          { "已记录", logged } },
                        "沿用进度", "另起一份", 0);
    }
}

// 「另起一份」：把当前这份从共享的阅读数据里**摘出去**，两边各自独立记。
//
// 不能只把页码清零了事——记录是共享的，清零会把**另一处**的进度一起抹掉。
// 摘出去后两边都退回"按路径认"：内容指纹一条记录只能挂一个，留着它下次打开又会
// 因为"内容相同"再问一遍，成了甩不掉的循环。
void detach_current_progress() {
    if (g_doc_key == 0 || g_doc.kind != UiDoc::Kind::Reading) return;
    const std::string me = g_identity.path_u8;

    std::size_t idx = g_state.docs.size();
    for (std::size_t i = 0; i < g_state.docs.size(); ++i)
        if (g_state.docs[i].first == g_doc_key) { idx = i; break; }

    if (idx < g_state.docs.size()) {
        lr::DocRecord& r = g_state.docs[idx].second;
        for (auto it = r.locations.begin(); it != r.locations.end(); ++it) {
            if (*it == me) { r.locations.erase(it); break; }
        }
        if (r.locations.empty()) {
            g_state.docs.erase(g_state.docs.begin() + static_cast<std::ptrdiff_t>(idx));
        } else {
            // 还有别的份：这条退回按"剩下那一处"的路径键认，腾出内容指纹
            const std::uint64_t rk = lr::document_key(lr::utf8_to_wide(r.locations.front()));
            if (rk != 0 && g_state.find(rk) == nullptr) (void)lr::rekey(g_state, idx, rk);
        }
    }

    // 当前这份：新起一条按路径键认的记录，从零开始
    g_doc_key = g_identity.path;
    if (g_doc_key != 0) {
        lr::DocRecord& nr = g_state.upsert(g_doc_key);
        nr.page = 0;
        nr.bookmarks.clear();
        nr.path_key = g_identity.path;
        nr.page_count = g_identity.page_count;
        lr::remember_location(nr, me);
    }
    (void)lr::save_state(g_state_path, g_state);
    g_restore_pending = false;      // 首帧待恢复的位置作废
    request_jump_scroll(0, 0.0f);
}

void clear_reading_data(std::uint64_t key) {
    if (key == 0) return;
    g_state.erase(key);
    // 删的正是当前在读的那本：解除本次会话的绑定（不再写入），下次打开按新文档处理。
    if (key == g_doc_key) g_doc_key = 0;
    (void)lr::save_state(g_state_path, g_state);
}

void clear_all_reading_data() {
    g_state.docs.clear();
    g_doc_key = 0;
    (void)lr::save_state(g_state_path, g_state);
}

int unknown_reading_data_count() {
    int n = 0;
    for (const auto& kv : g_state.docs)
        if (kv.second.locations.empty()) ++n;
    return n;
}

// 只清"位置未知"的那批：升级前的 v1 记录没存过路径，显示不出文档名，也永远认不出来
// （除非再打开一次同一个文件让它按路径键命中）。它们对用户是没有信息的噪音，单独给个入口。
void clear_unknown_reading_data() {
    std::vector<std::pair<std::uint64_t, lr::DocRecord>> keep;
    keep.reserve(g_state.docs.size());
    for (auto& kv : g_state.docs) {
        if (kv.second.locations.empty()) {
            if (kv.first == g_doc_key) g_doc_key = 0;   // 正在读的那本被清掉：解除绑定
            continue;
        }
        keep.push_back(std::move(kv));
    }
    if (keep.size() == g_state.docs.size()) return;     // 没有可清的
    g_state.docs = std::move(keep);
    (void)lr::save_state(g_state_path, g_state);
}

bool current_page_has_bookmark() {
    const lr::DocRecord* r = g_state.find(g_doc_key);
    if (!r) return false;
    const int page = g_canvas.current_page();
    for (const lr::Bookmark& b : r->bookmarks)
        if (b.page == page) return true;
    return false;
}

void toggle_bookmark_current() {
    if (g_doc_key == 0 || g_doc.kind != UiDoc::Kind::Reading) return;
    const int page = g_canvas.current_page();
    if (page < 0) return;
    lr::DocRecord& r = g_state.upsert(g_doc_key);
    for (auto it = r.bookmarks.begin(); it != r.bookmarks.end(); ++it) {
        if (it->page == page) {
            r.bookmarks.erase(it);
            (void)lr::save_state(g_state_path, g_state);
            return;
        }
    }
    r.bookmarks.push_back(lr::Bookmark{ page, {} });
    (void)lr::save_state(g_state_path, g_state);
}

void remove_bookmark_at(int index) {
    if (g_doc_key == 0) return;
    lr::DocRecord* r = nullptr;
    for (auto& kv : g_state.docs)
        if (kv.first == g_doc_key) { r = &kv.second; break; }
    if (r == nullptr || index < 0 || index >= static_cast<int>(r->bookmarks.size())) return;
    r->bookmarks.erase(r->bookmarks.begin() + index);
    (void)lr::save_state(g_state_path, g_state);
}

// ---------------- 文档状态机 ----------------

void reset_doc_state() {
    g_doc.kind = UiDoc::Kind::None;
    g_doc.path_w.clear();
    g_doc.name_u8.clear();
    g_doc.ext_u8.clear();
    g_doc.error = lr::DocError::Ok;
    g_doc.detail_u8.clear();
    g_doc.info = lr::DocumentInfo{};
    g_doc.request_id = 0;
    g_canvas = lr::Canvas{};
    g_want_scale = -1.0f;
    g_last_target_scale = -1.0f;
    g_zoom_dirty_since = -1.0;
    g_canvas_scale = -1.0f;
    g_scroll_dir = 0;
    g_prev_scroll_y = 0.0f;
    g_doc_key = 0;
    g_identity = lr::DocIdentity{};
    g_outline.clear();
    g_raw_sizes.clear();
    g_page_fade.clear();
    g_restore_pending = false;
    // 动效状态：关闭文档后必须归零，否则下一个文档会带着上一个的待定量/滑行起点开场
    g_scroll_pending = 0.0f;
    g_jump_repin_page = -1;
    g_zoom_anim = false;
    g_zoom_end_fit = false;
    g_top_bar_h = -1.0f;
    g_sidebar_w = 0.0f;
    g_scroll_drag = false;
    g_scroll_drag_off = 0.0f;
    g_scroll_hover = false;
    g_rotation = 0;
    g_scheme = 0;
    g_show_sidebar = false;
    // Phase 8：文本交互与检索状态必须随文档一起清掉 —— 否则换文档后
    // 选区/命中仍指向旧文档的页与字符下标（会复制出错内容、或高亮到无关位置）。
    g_sel = Selection{};
    g_select_all_pending = false;
    g_content = lr::PageContent{};
    g_content_page = -1;
    g_content_want = -1;
    g_content_since = -1.0;
    g_ctx_page = -1;
    g_ctx_link_valid = false;
    g_ctx_image_valid = false;
    g_hover_page = g_hover_link = g_hover_char = -1;
    g_press_link_valid = false;
    g_press_on_text = false;
    g_press_page = -1;
    g_toast.clear();
    g_toast_since = -1.0;
    std::memset(g_search_buf, 0, sizeof g_search_buf);
    search_clear();
    g_open_password = false;
    g_auth_pending = false;
    std::memset(g_password_buf, 0, sizeof g_password_buf);  // 明文密码整个缓冲清零（ADR-039）
    g_password_error.clear();
    update_title();
}

// 文档打开成功 → 初始化画布并进入阅读态（恢复该文档记忆的阅读位置与视图参数）
void enter_reading() {
    g_raw_default.w = g_doc.info.page_width_pt > 0.0f ? g_doc.info.page_width_pt : 595.0f;
    g_raw_default.h = g_doc.info.page_height_pt > 0.0f ? g_doc.info.page_height_pt : 842.0f;
    g_raw_sizes.clear();
    g_raw_sizes.reserve(g_doc.info.page_sizes.size());
    for (const lr::PageSize& ps : g_doc.info.page_sizes) {
        lr::PageSizePt e;
        e.w = ps.width_pt > 0.0f ? ps.width_pt : g_raw_default.w;
        e.h = ps.height_pt > 0.0f ? ps.height_pt : g_raw_default.h;
        g_raw_sizes.push_back(e);
    }

    // 恢复记忆状态（无记录则用默认：fit-width / 单列 / 不旋转 / 正常配色）
    const lr::DocRecord* rec = g_state.find(g_doc_key);
    g_rotation = rec ? rec->rotation : 0;
    g_scheme = rec ? rec->scheme : 0;

    lr::CanvasState st;  // 默认：fit_width=true, columns=1
    st.margin_px = px(kCanvasMarginPx);   // 屏幕像素 → 随 DPI
    st.gap_ratio = gap_ratio_pref();      // 列宽比例 → 随缩放（ADR-029），不随 DPI
    if (rec) {
        st.zoom = rec->zoom;
        st.columns = rec->columns;
        st.fit_width = rec->fit_width;
        st.spread = rec->spread;
    }

    g_canvas = lr::Canvas{};
    g_canvas.set_margin_gap(st.margin_px, st.gap_ratio);
    push_canvas_sizes();   // 逐页真实尺寸（旋转折算），异构 PDF 不形变
    g_canvas.set_state(st);

    g_want_scale = -1.0f;  // 首帧立即采用目标倍率，不走防抖
    g_last_target_scale = -1.0f;
    g_zoom_dirty_since = -1.0;
    g_canvas_scale = ui_scale();  // 留白已按当前缩放写入
    g_scroll_dir = 0;             // 方向未定：首帧两侧都预取
    g_prev_scroll_y = 0.0f;

    // 阅读位置待首帧视口就绪后恢复（fit-width 派生 zoom 依赖视口尺寸，此刻视口还是 0）
    g_restore_pending = true;
    g_restore_page = rec ? rec->page : 0;

    g_page_fade.assign(static_cast<std::size_t>(std::max(0, g_doc.info.page_count)), PageFade{});
    g_toolbar_visible = true;         // 新文档从显示顶栏开始
    g_toolbar_idle_since = -1.0;
    // 动效状态复位：开文档不播动画（顶栏直接展开，首帧就是稳定态）
    g_scroll_pending = 0.0f;
    g_jump_repin_page = -1;
    g_zoom_anim = false;
    g_zoom_end_fit = false;
    g_top_bar_h = px(kTopBarH);
    g_sidebar_w = 0.0f;               // 开文档不播侧栏动画（侧栏本就没开）
    g_scroll_drag = false;
    g_scroll_hover = false;

    apply_view_transform();            // 旋转/配色下发（首次会触发整篇重渲）
    g_outline = g_renderer->outline(); // 目录快照（一次性）
}

void request_open_document(std::wstring path) {
    path = lr::to_absolute(path);
    g_doc.path_w = path;
    g_doc.name_u8 = lr::wide_to_utf8(lr::file_name_of(path));
    g_doc.ext_u8 = lr::wide_to_utf8(lr::extension_of(path));
    g_doc.error = lr::DocError::Ok;
    g_doc.detail_u8.clear();
    g_doc.info = lr::DocumentInfo{};
    g_canvas = lr::Canvas{};
    g_outline.clear();
    g_show_sidebar = false;
    g_auth_pending = false;  // 新开文档：清掉上一次可能残留的认证在途标记
    // 身份要等打开成功（指纹由工作线程算、页数只有打开后才知道）才解析，见 resolve_document_identity
    g_doc_key = 0;
    g_identity = lr::DocIdentity{};
    g_confirm_open = false;          // 上一次可能还挂着一个"是否沿用"的询问
    g_confirm_kind = ConfirmKind::None;

    // 本地即时判定：不存在 / 不在支持清单内（不必浪费一次线程往返）
    if (!lr::file_exists(path)) {
        g_doc.kind = UiDoc::Kind::Rejected;
        g_doc.error = lr::DocError::NotFound;
        update_title();
        return;
    }
    if (!lr::is_supported(path)) {
        g_doc.kind = UiDoc::Kind::Rejected;
        g_doc.error = lr::DocError::Unsupported;
        update_title();
        return;
    }

    g_doc.kind = UiDoc::Kind::Opening;
    g_doc.request_id = g_renderer->open(path);
    update_title();
}

void close_document() {
    save_reading_state();  // 关闭前落盘阅读位置（书签已实时落盘）
    if (g_doc.kind == UiDoc::Kind::Opening || g_doc.kind == UiDoc::Kind::Reading ||
        g_doc.kind == UiDoc::Kind::NeedsPassword)
        g_renderer->close();
    reset_doc_state();
}

void poll_document() {
    // 认证请求在途时，文档仍停在 NeedsPassword，但请求已投出，需要在这里接收结果。
    const bool awaiting_auth = (g_doc.kind == UiDoc::Kind::NeedsPassword && g_auth_pending);
    if (g_doc.kind != UiDoc::Kind::Opening && !awaiting_auth) return;

    const lr::DocState snap = g_renderer->doc_state();
    if (snap.id != g_doc.request_id) return;  // 已被更新的请求取代
    if (snap.phase == lr::DocPhase::Opening) return;

    switch (snap.phase) {
    case lr::DocPhase::Ready:
        g_doc.kind = UiDoc::Kind::Reading;
        g_doc.info = snap.info;
        g_doc.error = lr::DocError::Ok;
        g_auth_pending = false;
        g_open_password = false;
        std::memset(g_password_buf, 0, sizeof g_password_buf);  // 解锁成功：明文密码不再需要（ADR-039）
        resolve_document_identity(snap.file_fingerprint);  // 先定位记录，再恢复阅读位置
        enter_reading();
        break;
    case lr::DocPhase::Failed:
        g_doc.error = snap.error;
        g_doc.detail_u8 = snap.detail_u8;
        if (snap.error == lr::DocError::NeedsPassword) {
            // 密码错误：**保持弹窗打开**（只更新错误提示），不关→开跳变，避免闪烁。
            g_doc.kind = UiDoc::Kind::NeedsPassword;
            g_auth_pending = false;
            g_open_password = true;
            g_password_error = snap.detail_u8.find("invalid password") != std::string::npos
                                   ? "密码错误，请重试"
                                   : std::string();
        } else {
            g_doc.kind = UiDoc::Kind::Failed;
            g_auth_pending = false;
        }
        break;
    default:  // Idle：被显式关闭
        reset_doc_state();
        return;
    }
    update_title();
}

void submit_password() {
    if (g_password_buf[0] == '\0') { g_password_error = "请输入密码"; return; }
    // 保持 kind=NeedsPassword（背景不变）、弹窗不关闭：认证结果由 poll_document 接收。
    // 这样"解锁/输错"都不会出现弹窗关闭再打开的闪烁。
    g_auth_pending = true;
    g_password_error.clear();
    g_doc.request_id = g_renderer->authenticate(g_password_buf);
}

// ---------------- 侧栏 / 跳页 ----------------

void set_sidebar(bool on, int tab) {
    g_show_sidebar = on;
    if (on && tab >= 0) {
        g_sidebar_tab = tab;
        // ImGui 的 TabBar 自带选中态：只改 g_sidebar_tab 不会真的切过去（Ctrl+F 只开了
        // 侧栏却停在"目录"就是这条）。下一次绘制时对该分栏带一次 SetSelected 才生效。
        g_sidebar_tab_want = tab;
    }
}

void open_jump_popup() {
    g_open_jump = true;
    g_jump_page = g_canvas.current_page() + 1;
}

// ---------------- 视图动效（ADR-047） ----------------
//
// 设计：**动效只发生在 app 层写入画布之前**。画布保持"状态唯一真源 + 纯函数布局"，
// 不引入任何时间/插值概念，`canvas_test` 的断言与"滚动不重算布局"的 O(1) 性质都不受影响。
//
// 三条路径（统一由 [ui] Motion 开关，关掉即等价于动效前的直切行为）：
//   · 滚轮 / 方向键 / 整屏滚动 → request_scroll()：增量进 g_scroll_pending，逐帧一阶滞后消耗；
//   · 显式跳页（跳页框/目录/书签/缩略图/翻行）→ request_jump_scroll()：让画布先算出目标位置
//     （含对开/网格/钳制的唯一真源），再回退到起点交给动效滑行；
//   · 缩放 → zoom_*_animated()：显示倍率指数趋近目标倍率，锚点固定不动。

// 滑行到目标滚动位置：起点/终点由调用方用画布算好（本文件内部使用）。
// 末尾要重新钉游标的原因见 app_internal.h 的 g_jump_repin_page 注释。
namespace {
void start_scroll_glide(float from, float to, int repin_page, float repin_align) {
    if (!g_prefs.motion || std::fabs(to - from) < 1.0f) {
        g_scroll_pending = 0.0f;
        g_jump_repin_page = -1;
        return;
    }
    g_canvas.scroll_by(0.0f, from - to);  // 回退到起点（scroll_by 会把游标同步回视口顶部）
    g_scroll_pending = to - from;
    g_jump_repin_page = repin_page;
    g_jump_repin_align = repin_align;
}
}  // namespace

void request_scroll(float delta_px) {
    // 新的滚动意图取消未完成的跳页滑行：否则滑行会把用户刚滚到的位置又拽回目标页。
    g_jump_repin_page = -1;
    g_scroll_pending += delta_px;
}

void request_jump_scroll(int page, float align) {
    const float from = g_canvas.state().scroll_y;
    g_canvas.scroll_to_page(page, align);
    start_scroll_glide(from, g_canvas.state().scroll_y, page, align);
}

void zoom_to_animated(float z, float anchor_sx, float anchor_sy) {
    z = std::clamp(z, lr::kMinZoom, lr::kMaxZoom);
    g_zoom_anchor_x = anchor_sx;
    g_zoom_anchor_y = anchor_sy;
    g_zoom_end_fit = false;
    if (!g_prefs.motion) {
        g_zoom_anim = false;
        g_canvas.set_zoom(z, anchor_sx, anchor_sy);
        return;
    }
    if (std::fabs(z - view_zoom_target()) < 1e-4f) return;  // 目标未变：不重启动画
    g_zoom_anim = true;
    g_zoom_to = z;
}

void zoom_by_animated(float factor, float anchor_sx, float anchor_sy) {
    if (!(factor > 0.0f)) return;
    // 基于**目标倍率**累乘：动画进行中若基于显示值，连续滚轮的倍率会被滞后吞掉
    zoom_to_animated(view_zoom_target() * factor, anchor_sx, anchor_sy);
}

// 适合宽度：让画布先算派生倍率（fit 公式的唯一真源），再回退到当前倍率交给动效，
// 动画结束时重新置 fit（幂等；期间显示的是插值倍率）。
void fit_to_width_animated() {
    if (!g_prefs.motion) {
        g_zoom_anim = false;
        g_zoom_end_fit = false;
        g_canvas.fit_to_width();
        return;
    }
    const lr::CanvasState st = g_canvas.state();
    const float from = g_canvas.effective_zoom();
    g_canvas.fit_to_width();
    const float to = g_canvas.effective_zoom();
    if (std::fabs(to - from) < 1e-4f) return;  // 已在 fit 状态：无需动画
    g_canvas.set_state(st);                    // 回退（含 fit_width 标志）
    g_zoom_anim = true;
    g_zoom_to = to;
    g_zoom_anchor_x = g_canvas.viewport_w() * 0.5f;
    g_zoom_anchor_y = g_canvas.viewport_h() * 0.5f;
    g_zoom_end_fit = true;
}

float view_zoom_target() {
    return g_zoom_anim ? g_zoom_to : g_canvas.effective_zoom();
}

// 每帧推进（在画布输入处理之后、渲染请求之前调用）
void step_view_motion(float dt) {
    // 1) 待定滚动：一阶滞后消耗
    if (g_scroll_pending != 0.0f) {
        if (!g_prefs.motion) {
            g_canvas.scroll_by(0.0f, g_scroll_pending);
            g_scroll_pending = 0.0f;
        } else {
            const float step = g_scroll_pending * (1.0f - std::exp(-kScrollSmoothRate * dt));
            g_canvas.scroll_by(0.0f, step);
            g_scroll_pending -= step;
            if (std::fabs(g_scroll_pending) < kMotionEpsPx) g_scroll_pending = 0.0f;
        }
        if (g_scroll_pending == 0.0f && g_jump_repin_page >= 0) {
            g_canvas.scroll_to_page(g_jump_repin_page, g_jump_repin_align);
            g_jump_repin_page = -1;
        }
    }

    // 2) 缩放插值：锚点固定，显示倍率趋近目标倍率（收敛后吸附到目标，避免残留亚像素差）
    if (g_zoom_anim) {
        const float target = g_zoom_to;
        const bool done = !g_prefs.motion ||
                          std::fabs(g_canvas.effective_zoom() - target) <= 1e-3f * target;
        g_canvas.set_zoom(done ? target
                               : approach(g_canvas.effective_zoom(), target, kZoomSmoothRate, dt),
                          g_zoom_anchor_x, g_zoom_anchor_y);
        if (done) {
            g_zoom_anim = false;
            if (g_zoom_end_fit) { g_zoom_end_fit = false; g_canvas.fit_to_width(); }
        }
    }
}

// ---------------- 画布输入 ----------------

// 翻行/翻页的几何计算全在画布层（纯函数、可单测，见 canvas_test 的 scroll_rows 用例）。
// UI 层只做转发：不再用 visible_first() 自行推算目标行 —— 那正是"视口高于一行时末页反复
// 卡住"的根因（已由阅读游标修掉，见 ADR-023 与 canvas.ixx）。
// 动效（ADR-047）：翻行同样是一次"显式跳页"，故先记下起点、让画布跳到目标，再回退滑行。
void scroll_by_rows(int dir) {
    const float from = g_canvas.state().scroll_y;
    g_canvas.scroll_rows(dir);
    start_scroll_glide(from, g_canvas.state().scroll_y, g_canvas.current_page(), 0.0f);
}

// 键盘快捷键**刻意不以 ImGui 窗口焦点为门**（ADR-026）：
//   1. 窗口结构是「无边框 shell 根窗口 + ##canvas 子窗口」，两者都带
//      ImGuiWindowFlags_NoNav（= NoNavInputs | NoNavFocus）。NoNavFocus 使 ImGui
//      在窗口出现时**不**把它设为 g.NavWindow，于是**启动后、首次点进画布之前
//      ImGui::IsWindowFocused() 恒为 false** —— 这段时间里所有快捷键（含 1/2/3/4
//      切列）全是死的，必须先点一下画布。用户反馈"按 1234 不切列"即由此而来。
//   2. 快捷键是**应用级语义**（整个窗口只有一个阅读视图），本就不该由 ImGui 的窗口
//      焦点决定；跳页弹窗/调试浮层出现时焦点会移走，同样会让快捷键莫名失效。
// 故此处只以「无文本输入（io.WantTextInput）」为门；阅读态与弹窗由调用方保证。
// 鼠标（滚轮/拖拽）仍需悬停在画布上，与键盘分开判断。
//
// 按键判定自 ADR-054 起统一走命令表（cmd_pressed）：绑哪个键、是否连发、几个槽
// 全部由 kCmds / g_binds 决定，本函数只负责"命令被按下时做什么"。
void handle_canvas_input(const ImVec2& origin, const ImVec2& size, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();

    // 滚轮：按住「滚轮缩放」的修饰键（默认 Ctrl，可改）→ 以鼠标为不动点缩放，否则滚动。
    // 两者都进动效层（ADR-047）：滚动进待定量、缩放进插值；关闭动效时即等价于直接改画布。
    if (hovered && io.MouseWheel != 0.0f) {
        if (wheel_zoom_mod_held()) {
            zoom_by_animated(std::pow(kZoomStep, io.MouseWheel),
                             io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        } else {
            request_scroll(-io.MouseWheel * px(kScrollStepPx));
        }
    }

    // 文本交互（Phase 8）：**先于平移**处理，因为它要决定"这一串左键归谁"。
    // 规则（人工确认的交互设计）：指针悬停在可选文本上时光标变成 I 型，
    // **此时拖拽 = 选择文本**；悬停不到文本时拖拽仍是平移（1:1 跟手，手感不变）。
    const bool text_took_drag = handle_text_interaction(origin, size, hovered);

    // 左键拖拽平移（位移直接取鼠标物理像素增量，不做缩放换算）。
    // **刻意不做平滑**：直接操纵必须 1:1 跟手；同时掐掉滚轮残留的平滑尾巴。
    // 在滚动条上按下/拖动时不进入平移 —— 否则"拖滚动条"变成"拖页面"（实测反馈）。
    if (!text_took_drag && hovered && !g_scroll_drag &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        g_scroll_pending = 0.0f;
        g_jump_repin_page = -1;
        g_canvas.scroll_by(-io.MouseDelta.x, -io.MouseDelta.y);
    }

    // 中键拖拽平移：**任何位置都可用**（Phase 8）。
    // 为什么必须有：左键在文字上已被"选择文本"接管，而放大到文字铺满视口时，
    // 左键处处都是选择 —— 没有这个兜底就无法平移。桌面阅读器的通行做法。
    if (hovered && !g_scroll_drag && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        g_scroll_pending = 0.0f;
        g_jump_repin_page = -1;
        g_canvas.scroll_by(-io.MouseDelta.x, -io.MouseDelta.y);
    }

    if (io.WantTextInput) return;  // 有文本输入在跑：键盘归它
    handle_reading_commands(size);
}

// 阅读态命令派发。全部走"动效入口"（ADR-047），与菜单/按钮同一批语义。
void handle_reading_commands(const ImVec2& size) {
    const float cx = size.x * 0.5f;
    const float cy = size.y * 0.5f;
    const float vh = size.y;

    if (cmd_pressed(Cmd::ScrollDown)) request_scroll(px(kKeyScrollPx));
    if (cmd_pressed(Cmd::ScrollUp))   request_scroll(-px(kKeyScrollPx));
    if (cmd_pressed(Cmd::PageDown))   request_scroll(vh * 0.9f);
    if (cmd_pressed(Cmd::PageUp))     request_scroll(-vh * 0.9f);
    if (cmd_pressed(Cmd::FirstPage))  request_jump_scroll(0, 0.0f);
    if (cmd_pressed(Cmd::LastPage))   request_jump_scroll(g_canvas.page_count() - 1, 0.0f);
    if (cmd_pressed(Cmd::NextRow))    scroll_by_rows(+1);
    if (cmd_pressed(Cmd::PrevRow))    scroll_by_rows(-1);
    if (cmd_pressed(Cmd::JumpPage))   open_jump_popup();

    if (cmd_pressed(Cmd::ZoomIn))     zoom_by_animated(kZoomStep, cx, cy);
    if (cmd_pressed(Cmd::ZoomOut))    zoom_by_animated(1.0f / kZoomStep, cx, cy);
    if (cmd_pressed(Cmd::FitWidth))   fit_to_width_animated();

    if (cmd_pressed(Cmd::Col1))         g_canvas.set_columns(1);
    if (cmd_pressed(Cmd::Col2))         g_canvas.set_columns(2);
    if (cmd_pressed(Cmd::Col3))         g_canvas.set_columns(3);
    if (cmd_pressed(Cmd::Col4))         g_canvas.set_columns(4);
    if (cmd_pressed(Cmd::ToggleSpread)) g_canvas.set_spread(!g_canvas.state().spread);
    if (cmd_pressed(Cmd::RotateCW))     rotate_view(90);
    if (cmd_pressed(Cmd::ToggleDark)) toggle_scheme(1);
    if (cmd_pressed(Cmd::ToggleWarm))  toggle_scheme(2);

    if (cmd_pressed(Cmd::ToggleSidebar))  set_sidebar(!g_show_sidebar, 0);
    if (cmd_pressed(Cmd::ToggleBookmark)) toggle_bookmark_current();

    // 文本（Phase 8）。注意本函数只在"无文本输入"时被调用（见 handle_canvas_input 的门），
    // 因此搜索框/密码框有焦点时 Ctrl+C / Ctrl+A 不会被这里抢走。
    if (cmd_pressed(Cmd::Copy))      selection_copy();
    if (cmd_pressed(Cmd::SelectAll)) selection_select_all();
    if (cmd_pressed(Cmd::OpenSearch)) {
        set_sidebar(true, 3);
        g_search_focus = true;
    }
}

// 全局命令：任何状态都可用。由 draw_shell 在「无弹窗/无文本输入/未在捕获按键」时调用
// （见 ui.cpp 的 any_dialog_open）。有弹窗时不派发，避免 Esc 之类"先关弹窗又触发命令"。
void handle_global_commands() {
    if (cmd_pressed(Cmd::ToggleDebug))      g_show_debug ^= 1;
    if (cmd_pressed(Cmd::ToggleFullscreen)) g_request_fullscreen_toggle = true;  // 帧间执行（ADR-060）
    if (cmd_pressed(Cmd::OpenFile))         g_request_open_dialog = true;
    if (cmd_pressed(Cmd::OpenSettings))     g_show_settings = true;
    if (cmd_pressed(Cmd::OpenKeys))         { g_show_settings = true; g_settings_open_tab = 3; }

    // 画布右键菜单的键盘等价入口（Shift+F10 / 菜单键）：标准 Windows 习惯、
    // 属"无鼠标兜底"而非可自定义命令，故仍写死。
    if (g_doc.kind == UiDoc::Kind::Reading &&
        ((ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false)) ||
         ImGui::IsKeyPressed(ImGuiKey_Menu, false)))
        g_open_canvas_ctx = true;
}

// ---------------- 缩放防抖与渲染请求 ----------------

// 缩放防抖：目标倍率稳定 150ms 后，才按新倍率请求高清重渲染；
// 期间 g_want_scale 保持旧值，页面用现有纹理显示（双线性放大，不闪白）。
// 注意目标是**目标倍率**而不是显示倍率（ADR-047）：缩放插值进行中显示值每帧都在变，
// 若盯显示值，防抖会被动画不断重置，高清重渲要等动画结束后再等 150ms 才开始；
// 盯目标倍率则"最后一次缩放意图后 150ms"即触发，正好在动画收敛时换入高清。
void update_want_scale() {
    const float target = view_zoom_target();
    const double now = ImGui::GetTime();

    if (g_want_scale < 0.0f) {  // 首帧（刚进入阅读态）：立即采用
        g_want_scale = target;
        g_last_target_scale = target;
        g_zoom_dirty_since = -1.0;
        return;
    }
    if (std::fabs(target - g_want_scale) <= 0.002f) {  // 已与投递倍率一致
        g_last_target_scale = target;
        g_zoom_dirty_since = -1.0;
        return;
    }
    if (std::fabs(target - g_last_target_scale) > 0.002f) {
        g_last_target_scale = target;
        g_zoom_dirty_since = now;  // 倍率仍在变化，重置计时（真防抖）
    }
    if (g_zoom_dirty_since >= 0.0 && (now - g_zoom_dirty_since) >= kZoomDebounceSec) {
        g_want_scale = target;
        g_zoom_dirty_since = -1.0;
    }
}

// 可见页 + 预加载页 → 渲染请求（可见页优先）。
// 预加载策略（ADR-032）：可见行 ±1 行，且**方向感知** —— 向下滚只预取下方一行、
// 向上滚只预取上方一行。理由：滚动有方向，反向的预加载在下一帧多半就被逐出，
// 纯属浪费渲染线程与显存；方向未定（刚打开/跳页后）时两侧都预取。
void emit_wants() {
    const int n = g_canvas.page_count();
    if (n <= 0) return;
    const float scale = g_want_scale > 0.0f ? g_want_scale : g_canvas.effective_zoom();

    const int first = g_canvas.visible_first();
    const int last = g_canvas.visible_last();
    if (first < 0 || last < first) return;

    const int r0 = g_canvas.row_of(first);
    const int r1 = g_canvas.row_of(last);
    int pf = first, pl = last;
    if (g_scroll_dir >= 0 && r1 + 1 < g_canvas.rows())
        pl = std::max(pl, g_canvas.row_page_end(r1 + 1));      // 下方一行
    if (g_scroll_dir <= 0 && r0 - 1 >= 0)
        pf = std::min(pf, g_canvas.first_page_in_row(r0 - 1)); // 上方一行

    std::vector<lr::RenderWant> wants;
    wants.reserve(static_cast<std::size_t>(pl - pf + 1));
    for (int i = first; i <= last && i < n; ++i) wants.push_back({ i, scale });
    for (int i = pf; i <= pl; ++i)
        if (i >= 0 && i < n && (i < first || i > last)) wants.push_back({ i, scale });

    g_renderer->set_wanted(std::move(wants));
}

// ============================================================================
// 文本交互 / 剪贴板 / 全文搜索（Phase 8）
// ============================================================================
//
// 坐标纪律：本段所有函数内部只用"未旋转页面 pt"（page_map.h 的约定）。
// 屏幕上的一切（命中测试、绘制、链接热区）都先经 page_map 折算，故旋转视图下
// 没有额外分支 —— 这也是把旋转折算单独抽成纯函数并单测的原因。

namespace {

// 选区端点：偏向字符框的哪一侧（false 左 / true 右），偏移量为框宽的 30%。
// **不取到边缘本身**：字形的前进宽度与墨迹盒并不重合，边缘点可能被 MuPDF 判给相邻字符，
// 于是复制范围会莫名多/少一个字。取 30% 处既稳稳落在本字符内，又把范围顶到了外侧。
float sel_point_x(const lr::TextQuad& q, bool right) {
    const float cx = (q.ulx + q.urx + q.llx + q.lrx) * 0.25f;
    const float edge = right ? (q.urx + q.lrx) * 0.5f : (q.ulx + q.llx) * 0.5f;
    return cx + (edge - cx) * 0.3f;
}
float sel_point_y(const lr::TextQuad& q) {
    return (q.uly + q.ury + q.lly + q.lry) * 0.25f;
}

// 由当前内容快照重算选区几何（复制端点 + 按行合并的高亮矩形），并**冻结**进 g_sel。
// 冻结的理由见 app_internal.h 的 Selection 注释。
void selection_rebuild() {
    g_sel.rects.clear();
    g_sel.ax_pt = g_sel.ay_pt = g_sel.bx_pt = g_sel.by_pt = 0.0f;
    if (!g_sel.active || g_sel.page < 0 || g_sel.page != g_content_page) return;
    const int cnt = static_cast<int>(g_content.chars.size());
    if (cnt <= 0 || g_sel.anchor < 0 || g_sel.head < 0) return;
    if (g_sel.anchor >= cnt || g_sel.head >= cnt) return;

    const int lo = std::min(g_sel.anchor, g_sel.head);
    const int hi = std::max(g_sel.anchor, g_sel.head);

    g_sel.ax_pt = sel_point_x(g_content.chars[lo].quad, false);
    g_sel.ay_pt = sel_point_y(g_content.chars[lo].quad);
    g_sel.bx_pt = sel_point_x(g_content.chars[hi].quad, true);
    g_sel.by_pt = sel_point_y(g_content.chars[hi].quad);

    // 按行合并：逐字符画四边形会因相邻框重叠而出现"深一块浅一块"，看着像马赛克。
    // 同一行内取并集，得到"一行一段"的干净色块（与桌面阅读器的观感一致）。
    int     line = -1;
    SelRect acc{};
    for (int i = lo; i <= hi; ++i) {
        const lr::TextQuad& q = g_content.chars[static_cast<std::size_t>(i)].quad;
        const int cl = g_content.chars[static_cast<std::size_t>(i)].line;
        const float x0 = std::min(std::min(q.ulx, q.llx), std::min(q.urx, q.lrx));
        const float x1 = std::max(std::max(q.ulx, q.llx), std::max(q.urx, q.lrx));
        const float y0 = std::min(std::min(q.uly, q.ury), std::min(q.lly, q.lry));
        const float y1 = std::max(std::max(q.uly, q.ury), std::max(q.lly, q.lry));
        if (cl != line) {
            if (line >= 0) g_sel.rects.push_back(acc);
            line = cl;
            acc = SelRect{ x0, y0, x1, y1 };
        } else {
            acc.x0 = std::min(acc.x0, x0);
            acc.y0 = std::min(acc.y0, y0);
            acc.x1 = std::max(acc.x1, x1);
            acc.y1 = std::max(acc.y1, y1);
        }
    }
    if (line >= 0) g_sel.rects.push_back(acc);
}

// 打开一个链接：内部跳转 → 翻页；外部 URL → 交系统默认浏览器。
void open_link(const lr::PageLink& link) {
    if (link.target_page >= 0) {
        request_jump_scroll(link.target_page, 0.0f);
        show_toast("跳转到第 " + std::to_string(link.target_page + 1) + " 页");
        return;
    }
    if (link.uri.empty()) return;
    const std::wstring w = lr::utf8_to_wide(link.uri);
    if (w.empty()) return;
    // ShellExecuteW：走宽字符，避免非 ASCII 的 URL 在 ANSI 代码页下被打断。
    // 返回值 ≤ 32 是"伪句柄"，表示失败（这是 ShellExecute 的老约定）。
    const HINSTANCE r = ShellExecuteW(g_hwnd, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) show_toast("无法打开链接");
}

}  // namespace

bool page_view_of(const ImVec2& origin, int page, PageView& view, PageGeom& geom) {
    const int n = g_canvas.page_count();
    if (page < 0 || page >= n) return false;
    const lr::PageRect r = g_canvas.page_rect(page);
    view.x = origin.x + r.x;
    view.y = origin.y + r.y;
    view.w = r.w;
    view.h = r.h;
    // 未旋转页面尺寸：优先逐页真实尺寸（ADR-022），整表缺失时退回首页尺寸
    if (static_cast<std::size_t>(page) < g_raw_sizes.size()) {
        geom.w_pt = g_raw_sizes[static_cast<std::size_t>(page)].w;
        geom.h_pt = g_raw_sizes[static_cast<std::size_t>(page)].h;
    } else {
        geom.w_pt = g_raw_default.w;
        geom.h_pt = g_raw_default.h;
    }
    return view.w > 0.0f && view.h > 0.0f && geom.w_pt > 0.0f && geom.h_pt > 0.0f;
}

int page_at_screen(const ImVec2& origin, const ImVec2& screen) {
    const int n = g_canvas.page_count();
    const int vf = g_canvas.visible_first();
    const int vl = g_canvas.visible_last();
    if (vf < 0 || n <= 0) return -1;
    for (int i = vf; i <= vl && i < n; ++i) {
        const lr::PageRect r = g_canvas.page_rect(i);
        const float x0 = origin.x + r.x;
        const float y0 = origin.y + r.y;
        if (screen.x >= x0 && screen.x <= x0 + r.w &&
            screen.y >= y0 && screen.y <= y0 + r.h)
            return i;
    }
    return -1;
}

namespace {

// 行的"文字高度"：取 bbox 高与宽的较小者。
// 竖排文字的 bbox 高是**文字长度**，直接拿它当行高会得出荒唐的容差（整页都算命中）。
float line_text_height(const lr::TextLine& L) {
    return std::max(std::min(L.y1 - L.y0, L.x1 - L.x0), 1.0f);
}

}  // namespace

int char_index_at(int page, float x_pt, float y_pt, bool clamp_to_nearest) {
    if (page != g_content_page) return -1;
    const int cnt = static_cast<int>(g_content.chars.size());
    if (cnt <= 0) return -1;

    // 1) 选行。带"行高 1/4"的容差：点在行间空白（行距小于半个字高）仍算落在该行。
    //    **超出容差就是"此处无文字"** —— 悬停/按下走这条路；拖拽时（clamp）才退回最近行。
    int   hit_line = -1;
    int   near_line = -1;
    float near_d = 0.0f;
    for (std::size_t i = 0; i < g_content.lines.size(); ++i) {
        const lr::TextLine& L = g_content.lines[i];
        const float pad = line_text_height(L) * 0.25f;
        if (y_pt >= L.y0 - pad && y_pt <= L.y1 + pad) {
            hit_line = static_cast<int>(i);
            break;
        }
        const float d = std::fabs(y_pt - (L.y0 + L.y1) * 0.5f);
        if (near_line < 0 || d < near_d) {
            near_line = static_cast<int>(i);
            near_d = d;
        }
    }
    const int line = (hit_line >= 0) ? hit_line : (clamp_to_nearest ? near_line : -1);
    if (line < 0) return -1;

    // 2) 行内选字符：x 落在字符框内即命中；否则取 x 距离最近的一个。
    //    严格模式下只有"离行内字符不超过半个字高"才算命中 —— 于是拖到行尾之外能吸附到
    //    最后一个字符（拖拽走 clamp），而**在页边空白上悬停则如实报告"没有文字"**。
    const float pad_x = line_text_height(g_content.lines[static_cast<std::size_t>(line)]) * 0.5f;
    int   best = -1;
    float bd = 0.0f;
    for (int i = 0; i < cnt; ++i) {
        const lr::TextChar& c = g_content.chars[static_cast<std::size_t>(i)];
        if (c.line != line) continue;
        const lr::TextQuad& q = c.quad;
        const float x0 = std::min(std::min(q.ulx, q.llx), std::min(q.urx, q.lrx));
        const float x1 = std::max(std::max(q.ulx, q.llx), std::max(q.urx, q.lrx));
        if (x_pt >= x0 && x_pt <= x1) return i;
        const float d = (x_pt < x0) ? (x0 - x_pt) : (x_pt - x1);
        if (best < 0 || d < bd) {
            best = i;
            bd = d;
        }
    }
    if (best < 0) return -1;
    if (clamp_to_nearest || bd <= pad_x) return best;
    return -1;
}

int link_index_at(int page, float x_pt, float y_pt) {
    if (page != g_content_page) return -1;
    for (std::size_t i = 0; i < g_content.links.size(); ++i) {
        const lr::PageLink& l = g_content.links[i];
        // 热区在未旋转 pt 空间是轴对齐矩形；90° 的整数倍旋转仍把矩形映成矩形，
        // 故"在未旋转空间做点在矩形内测试"与"在屏幕上测"等价。
        if (x_pt >= l.x0 && x_pt <= l.x1 && y_pt >= l.y0 && y_pt <= l.y1)
            return static_cast<int>(i);
    }
    return -1;
}

int image_index_at(int page, float x_pt, float y_pt) {
    if (page != g_content_page) return -1;
    for (std::size_t i = 0; i < g_content.images.size(); ++i) {
        const lr::PageImageRect& r = g_content.images[i];
        if (x_pt >= r.x0 && x_pt <= r.x1 && y_pt >= r.y0 && y_pt <= r.y1)
            return static_cast<int>(i);
    }
    return -1;
}

void selection_clear() {
    g_sel = Selection{};
    g_select_all_pending = false;
}

void selection_select_all() {
    // 全选的是"当前正在读的那一页"。内容快照可能还没到（鼠标不在画布上），
    // 这时先记一个待办，等快照到达后在 update_hovered_content 里补做。
    const int page = (g_content_page >= 0) ? g_content_page : g_canvas.current_page();
    if (page < 0) return;
    if (page != g_content_page) {
        g_renderer->request_page_content(page);
        g_content_want = page;
        g_select_all_pending = true;
        return;
    }
    const int cnt = static_cast<int>(g_content.chars.size());
    if (cnt <= 0) {
        show_toast("本页没有可选择的文本");
        return;
    }
    g_sel = Selection{};
    g_sel.active = true;
    g_sel.page = page;
    g_sel.anchor = 0;
    g_sel.head = cnt - 1;
    selection_rebuild();
}

void selection_copy() {
    if (!g_sel.active || g_sel.page < 0) {
        show_toast("没有选中文本");
        return;
    }
    if (g_sel.rects.empty()) return;
    g_renderer->request_copy_text(g_sel.page, g_sel.ax_pt, g_sel.ay_pt, g_sel.bx_pt, g_sel.by_pt);
}

void copy_image_at_context() {
    if (g_ctx_page < 0) {
        show_toast("此处没有可复制的图片");
        return;
    }
    g_renderer->request_copy_image(g_ctx_page, g_ctx_x_pt, g_ctx_y_pt);
}

void open_link_at_context() {
    if (!g_ctx_link_valid) return;
    open_link(g_ctx_link);
}

void show_toast(std::string text) {
    g_toast = std::move(text);
    g_toast_since = ImGui::GetTime();
}

std::string toast_text() {
    if (g_toast.empty() || g_toast_since < 0.0) return {};
    if (ImGui::GetTime() - g_toast_since > kToastSec) return {};
    return g_toast;
}

bool handle_text_interaction(const ImVec2& origin, const ImVec2& size, bool hovered) {
    (void)size;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mp = io.MousePos;

    // ---- 1) 悬停：命中测试 + 光标形状 ----
    g_hover_page = hovered ? page_at_screen(origin, mp) : -1;
    g_hover_link = -1;
    g_hover_char = -1;
    float hx = 0.0f, hy = 0.0f;
    if (g_hover_page >= 0 && g_hover_page == g_content_page) {
        PageView view;
        PageGeom geom;
        if (page_view_of(origin, g_hover_page, view, geom) &&
            screen_to_page_pt(view, geom, g_rotation, mp.x, mp.y, hx, hy)) {
            g_hover_link = link_index_at(g_hover_page, hx, hy);
            // 严格命中：页边空白 / 图注之外的地方必须如实报告"没有文字"，
            // 否则光标会在整页上一直是 I 型（实测反馈）。
            g_hover_char = char_index_at(g_hover_page, hx, hy, /*clamp_to_nearest=*/false);
        }
    }
    // 链接优先于文本：链接常常叠在文字上，此时"手型"比"I 型"更能说明点下去会发生什么。
    // 悬停在**滚动条**上时不改光标：滚动条压在画布右缘之上，其下若有文字会误报成
    // "可选文本"（滚动条本身也不是选择区域）。
    const bool over_bar = g_scroll_drag || g_scroll_hover;
    if (hovered && !over_bar && g_hover_link >= 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    else if (hovered && !over_bar && g_hover_char >= 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    // ---- 2) 右键：记下菜单锚点（此刻鼠标还在画布上）----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        g_ctx_page = g_hover_page;
        g_ctx_x_pt = hx;
        g_ctx_y_pt = hy;
        g_ctx_link_valid = (g_hover_link >= 0);
        if (g_ctx_link_valid) g_ctx_link = g_content.links[static_cast<std::size_t>(g_hover_link)];
        // 图片命中测试在这里做一次即可：菜单项要据此置灰，而菜单打开后鼠标就离开画布了。
        g_ctx_image_valid = (g_ctx_page >= 0 && image_index_at(g_ctx_page, hx, hy) >= 0);
    }

    // ---- 3) 左键按下 ----
    if (hovered && !g_scroll_drag && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_press_x = mp.x;
        g_press_y = mp.y;
        g_press_page = g_hover_page;
        g_press_link_valid = (g_hover_link >= 0);
        if (g_press_link_valid) g_press_link = g_content.links[static_cast<std::size_t>(g_hover_link)];
        g_press_on_text = (g_hover_char >= 0);
        if (g_press_on_text) {
            g_sel.active = true;
            g_sel.page = g_hover_page;
            g_sel.anchor = g_hover_char;
            g_sel.head = g_hover_char;
            g_sel.dragging = true;
            selection_rebuild();
        }
        // 按在非文本处**不立刻**清选区：还要等抬起时区分"单击"与"拖拽平移"。
        // 若在这里清，用户想平移一下再回来看选区就没了。
    }

    // ---- 4) 拖动：更新拖动端 ----
    bool took_drag = false;
    if (g_sel.dragging && g_press_on_text) {
        took_drag = true;   // 整段拖拽期间接管左键：平移必须让位
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_sel.page == g_content_page) {
            PageView view;
            PageGeom geom;
            if (page_view_of(origin, g_sel.page, view, geom)) {
                float x_pt = 0.0f, y_pt = 0.0f;
                if (screen_to_page_pt(view, geom, g_rotation, mp.x, mp.y, x_pt, y_pt)) {
                    // 拖拽用 clamp：拖到行尾之外要吸附到最后一个字符，选区能"到底"。
                    const int idx = char_index_at(g_sel.page, x_pt, y_pt, /*clamp_to_nearest=*/true);
                    if (idx >= 0) {
                        g_sel.head = idx;
                        selection_rebuild();
                    }
                }
            }
        }
    }

    // ---- 5) 抬起：区分单击与拖拽 ----
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const float dx = mp.x - g_press_x;
        const float dy = mp.y - g_press_y;
        const float slop = px(kClickSlopPx);
        const bool is_click = (dx * dx + dy * dy) <= slop * slop;
        if (is_click) {
            if (g_press_link_valid) {
                open_link(g_press_link);
            } else if (g_press_on_text) {
                // 单击文字（未拖动）= 取消选区：与"点一下空白即取消"的桌面习惯一致
                selection_clear();
            }
        }
        g_sel.dragging = false;
        g_press_on_text = false;
        g_press_link_valid = false;
        g_press_page = -1;
    }
    return took_drag;
}

void update_hovered_content(const ImVec2& origin, bool hovered) {
    if (g_renderer == nullptr) return;

    // 1) 接收新快照
    if (g_content_want >= 0) {
        lr::PageContent pc;
        if (g_renderer->take_page_content(g_content_want, pc)) {
            g_content = std::move(pc);
            g_content_page = g_content_want;
            g_content_since = -1.0;
            if (g_select_all_pending) {
                g_select_all_pending = false;
                selection_select_all();   // 内容到了，补做全选
            }
        }
    }

    // 2) 决定本帧想要哪一页：鼠标所在页。
    //    只请求"鼠标所在那一页"而不是全部可见页 —— 抽一次 stext 与渲染一页同价，
    //    全可见页盲发会在滚动时把工作线程塞满，明显拖慢出图。
    if (!hovered) {
        // 鼠标离开画布：清掉"在途请求"标记，回到画布时重新投递。
        // 不清的话，若那一次请求恰好被丢弃（页损坏等），该页会永远等不到内容。
        g_content_want = -1;
        g_content_since = -1.0;
        return;
    }
    const int want = page_at_screen(origin, ImGui::GetIO().MousePos);
    if (want < 0) return;

    const double now = ImGui::GetTime();
    if (want == g_content_page) return;   // 已就绪
    if (want != g_content_want) {
        g_renderer->request_page_content(want);
        g_content_want = want;
        g_content_since = now;
        return;
    }
    // 同一页请求在途：超时（0.6s）未到达则重发一次。抽文本可能因页复杂而慢，
    // 也可能失败（损坏页），超时重发让"失败"不至于变成"永远没有 I 型光标"。
    if (g_content_since >= 0.0 && (now - g_content_since) > 0.6) {
        g_renderer->request_page_content(want);
        g_content_since = now;
    }
}

void update_clipboard_results() {
    if (g_renderer == nullptr) return;

    std::string text;
    if (g_renderer->take_copy_text(text)) {
        if (text.empty()) show_toast("没有可复制的文本");
        else if (set_clipboard_text(text)) show_toast("已复制文本");
        else show_toast("复制失败：剪贴板被占用");
    }

    lr::ImageData img;
    if (g_renderer->take_copy_image(img)) {
        if (!img.valid()) {
            show_toast("此处没有可复制的图片");
        } else if (set_clipboard_image_rgba(img.w, img.h, img.rgba.data())) {
            show_toast("已复制图片 " + std::to_string(img.w) + "×" + std::to_string(img.h));
        } else {
            show_toast("复制失败：剪贴板被占用");
        }
    }
}

// ---------------- 全文搜索 ----------------

bool search_has_query() { return g_search_buf[0] != '\0'; }

void search_clear() {
    g_search_hits.clear();
    g_search_cur = -1;
    g_search_scanned = 0;
    g_search_total = 0;
    g_search_active = false;
    g_search_truncated = false;
    g_search_scroll_pending = false;
    g_search_committed.clear();
    g_search_pending = false;
    g_search_edit_at = -1.0;
    g_search_seen[0] = '\0';
    if (g_renderer != nullptr) g_renderer->cancel_search();
}

void search_start() {
    const std::string needle(g_search_buf);
    g_search_hits.clear();
    g_search_cur = -1;
    g_search_scanned = 0;
    g_search_truncated = false;
    g_search_scroll_pending = false;
    // 已经发起检索：防抖计时作废，否则 0.4s 后会被"自动检索"再发一次同样的查询。
    g_search_pending = false;
    g_search_edit_at = -1.0;
    g_search_committed = needle;
    if (g_renderer == nullptr) return;
    if (needle.empty()) {
        g_search_active = false;
        g_search_total = 0;
        g_renderer->cancel_search();
        // 空关键字**要说话**：否则用户按了回车什么都没发生，只会得出"搜索无效"的结论。
        show_toast("请输入要查找的关键字");
        return;
    }
    g_renderer->start_search(needle);
    const lr::SearchStatus st = g_renderer->search_status();
    g_search_total = st.total;
    g_search_active = st.active;
}

void search_stop() {
    g_search_pending = false;
    g_search_edit_at = -1.0;
    if (g_renderer != nullptr) g_renderer->stop_search();   // 收手：保留进度与已找到的命中
    g_search_active = false;
    // 已找到的命中**刻意保留**：用户按「停止」是想让长检索收手，不是想丢掉已找到的结果。
    // 但扫描进度停在原处，所以 UI 不能再写"共 N 处"（那是"已扫完全文"的措辞）——
    // 由 `g_search_scanned < g_search_total` 区分"扫完"与"中途停下"。
}

void search_goto(int index) {
    if (index < 0 || index >= static_cast<int>(g_search_hits.size())) return;
    g_search_cur = index;
    g_search_scroll_pending = true;
}

void search_step_hit(int dir) {
    const int n = static_cast<int>(g_search_hits.size());
    if (n <= 0) return;
    int idx = (g_search_cur < 0) ? (dir >= 0 ? 0 : n - 1) : g_search_cur + dir;
    if (idx < 0) idx = n - 1;         // 环绕
    if (idx >= n) idx = 0;
    search_goto(idx);
}

void update_search() {
    if (g_renderer == nullptr) return;

    // ---- 1) 输入防抖：关键字一变就作废旧查询，停手 kSearchDebounceSec 后自动检索 ----
    // 必须放在"取新命中"之前：本帧若判定用户改了字，就不该再把旧查询的命中并进列表。
    if (std::strcmp(g_search_buf, g_search_seen) != 0) {
        std::snprintf(g_search_seen, sizeof g_search_seen, "%s", g_search_buf);
        g_search_edit_at = ImGui::GetTime();
        g_search_pending = true;
        // 立即停掉在途检索：关键字已经变了，旧查询的结果既没意义、又白占工作线程
        // （它与渲染抢同一个线程，不停会明显拖慢出图）。这就是"改词即取消"。
        if (g_search_active) {
            g_renderer->cancel_search();
            g_search_active = false;
        }
        // 旧结果立即清掉：结果列表必须与关键字一致，否则列表里是"别的词"的命中，
        // 点进去跳到的地方与输入框里的词对不上（用户会当成"搜索不准"）。
        g_search_hits.clear();
        g_search_cur = -1;
        g_search_scanned = 0;
        g_search_truncated = false;
        g_search_committed.clear();
    }
    if (g_search_pending && ImGui::GetTime() - g_search_edit_at >= kSearchDebounceSec) {
        if (g_search_buf[0] == '\0') {
            // 清空关键字 = 收场。**不弹**"请输入要查找的关键字"：用户是在删除，
            // 不是在搜空词，弹提示只会让人以为操作错了。
            g_search_pending = false;
            g_search_edit_at = -1.0;
            g_search_total = 0;
        } else {
            search_start();   // 内部会清空并重新发起，同时把 committed 落定
        }
    }

    // ---- 2) 增量取用新命中（渲染层边搜边发布）----
    const std::size_t before = g_search_hits.size();
    g_renderer->take_search_hits(g_search_hits);
    if (g_search_hits.size() > before && g_search_cur < 0)
        search_goto(0);   // 首批结果到达：自动选中并滚到第一条

    const lr::SearchStatus st = g_renderer->search_status();
    g_search_active = st.active;
    g_search_scanned = st.scanned;
    g_search_total = st.total;
    g_search_truncated = st.truncated;

    if (g_search_scroll_pending) {
        g_search_scroll_pending = false;
        if (g_search_cur >= 0 && g_search_cur < static_cast<int>(g_search_hits.size()))
            request_jump_scroll(g_search_hits[static_cast<std::size_t>(g_search_cur)].page, 0.0f);
    }
}

}  // namespace lr::app