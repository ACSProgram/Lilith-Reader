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

    { "Col1",           "视图", "单页",                      false, false, kb(ImGuiKey_1),              kb(ImGuiKey_Keypad1) },
    { "Col2",           "视图", "双页",                      false, false, kb(ImGuiKey_2),              kb(ImGuiKey_Keypad2) },
    { "Col3",           "视图", "三页",                      false, false, kb(ImGuiKey_3),              kb(ImGuiKey_Keypad3) },
    { "Col4",           "视图", "四页",                      false, false, kb(ImGuiKey_4),              kb(ImGuiKey_Keypad4) },
    { "ToggleSpread",   "视图", "双页对开（书籍模式）",      false, false, kb(ImGuiKey_D),              ImGuiKey_None },
    { "RotateCW",       "视图", "旋转 90°",                  false, false, kb(ImGuiKey_R),              ImGuiKey_None },
    { "ToggleInvert",   "视图", "反色（深色）",              false, false, kb(ImGuiKey_I),              ImGuiKey_None },
    { "ToggleSepia",    "视图", "护眼（暖色）",              false, false, kb(ImGuiKey_E),              ImGuiKey_None },

    { "ToggleSidebar",  "界面", "侧栏（目录 / 书签 / 缩略图）", false, false, kb(ImGuiKey_O),           ImGuiKey_None },
    { "ToggleBookmark", "界面", "当前页书签增删",            false, false, kb(ImGuiKey_B),              ImGuiKey_None },
    { "OpenSettings",   "界面", "设置",                      false, true,  kb(ImGuiKey_Comma, ImGuiMod_Ctrl), ImGuiKey_None },
    { "OpenKeys",       "界面", "按键设置",                  false, true,  kb(ImGuiKey_F1),             ImGuiKey_None },
    { "ToggleDebug",    "界面", "调试浮层",                  false, true,  kb(ImGuiKey_F3),             ImGuiKey_None },
    { "ToggleFullscreen","界面","全屏",                      false, true,  kb(ImGuiKey_F11),            kb(ImGuiKey_Escape) },
    { "OpenFile",       "界面", "打开文档…",                 false, true,  kb(ImGuiKey_O, ImGuiMod_Ctrl), ImGuiKey_None },
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
std::string chord_to_string(ImGuiKeyChord c) {
    if (c == ImGuiKey_None) return {};
    std::string s;
    if (c & ImGuiMod_Ctrl)  s += "Ctrl+";
    if (c & ImGuiMod_Shift) s += "Shift+";
    if (c & ImGuiMod_Alt)   s += "Alt+";
    s += ImGui::GetKeyName(static_cast<ImGuiKey>(c & ~ImGuiMod_Mask_));
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
        if (p == std::string::npos) { key_name = tok; break; }
        if (tok == "Ctrl")       mods |= ImGuiMod_Ctrl;
        else if (tok == "Shift") mods |= ImGuiMod_Shift;
        else if (tok == "Alt")   mods |= ImGuiMod_Alt;
        else { key_name = tok; break; }   // 意外前缀：把剩下的整体当键名，交给反查判定
        start = p + 1;
    }
    if (key_name.empty()) return ImGuiKey_None;
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
    for (int i = 0; i < kCmdCount; ++i) {
        if (i == cmd) continue;   // 同一命令的两个槽不算冲突
        for (int s = 0; s < kBindSlots; ++s)
            if (g_binds[i][s] == c) return i;
    }
    return -1;
}

void update_key_capture() {
    if (g_capture_cmd >= kCmdCount) return;
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
    g_renderer->set_view_transform(g_rotation, static_cast<lr::ColorMode>(g_color_mode));
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

// 直接设定配色（0 正常 / 1 反色 / 2 护眼）；按键 I/E 走 toggle。
void set_color_mode(int mode) {
    if (g_color_mode == mode) return;
    g_color_mode = mode;
    apply_view_transform();
}

void toggle_color_mode(int mode) { set_color_mode(g_color_mode == mode ? 0 : mode); }

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
    r.color_mode = g_color_mode;
    // 身份随每次落盘刷新：将来换键方案时能按 last_path 重算，也能给管理窗口显示"上次在哪"
    r.path_key = g_identity.path;
    r.page_count = g_identity.page_count;
    r.last_path_u8 = g_identity.path_u8;
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
    const std::string old_path = hit.last_path_u8;
    const int old_page = hit.page;
    const int old_marks = static_cast<int>(hit.bookmarks.size());
    g_doc_key = lr::adopt(g_state, static_cast<std::size_t>(m.index), g_identity);

    if (m.relocated && g_prefs.smart_match == kSmartMatchAsk) {
        char body[640];
        std::snprintf(body, sizeof body,
                      "这份文档与库中已有的一份阅读数据内容相同。\n"
                      "原位置：%s\n"
                      "已记录：第 %d 页，%d 个书签",
                      old_path.empty() ? "(未知位置)" : old_path.c_str(),
                      old_page + 1, old_marks);
        request_confirm(ConfirmKind::Relocate, "沿用这份阅读数据？", body,
                        "沿用进度", "从头开始", 0);
    }
}

// 「从头开始」：只清"读到哪"与书签；视图参数（缩放/列数/配色）是通用偏好，保留。
void reset_current_progress() {
    if (g_doc_key == 0) return;
    lr::DocRecord* r = nullptr;
    for (auto& kv : g_state.docs)
        if (kv.first == g_doc_key) { r = &kv.second; break; }
    if (r == nullptr) return;
    r->page = 0;
    r->bookmarks.clear();
    (void)lr::save_state(g_state_path, g_state);
    if (g_doc.kind == UiDoc::Kind::Reading) {
        g_restore_pending = false;      // 首帧待恢复的位置作废
        request_jump_scroll(0, 0.0f);
    }
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
    g_color_mode = 0;
    g_show_sidebar = false;
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
    g_color_mode = rec ? rec->color_mode : 0;

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
    if (on && tab >= 0) g_sidebar_tab = tab;
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

    // 滚轮：Ctrl 缩放（以鼠标为不动点），否则滚动。
    // 两者都进动效层（ADR-047）：滚动进待定量、缩放进插值；关闭动效时即等价于直接改画布。
    if (hovered && io.MouseWheel != 0.0f) {
        if (io.KeyCtrl) {
            zoom_by_animated(std::pow(kZoomStep, io.MouseWheel),
                             io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        } else {
            request_scroll(-io.MouseWheel * px(kScrollStepPx));
        }
    }

    // 左键拖拽平移（位移直接取鼠标物理像素增量，不做缩放换算）。
    // **刻意不做平滑**：直接操纵必须 1:1 跟手；同时掐掉滚轮残留的平滑尾巴。
    // 在滚动条上按下/拖动时不进入平移 —— 否则"拖滚动条"变成"拖页面"（实测反馈）。
    if (hovered && !g_scroll_drag && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
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
    if (cmd_pressed(Cmd::ToggleInvert)) toggle_color_mode(1);
    if (cmd_pressed(Cmd::ToggleSepia))  toggle_color_mode(2);

    if (cmd_pressed(Cmd::ToggleSidebar))  set_sidebar(!g_show_sidebar, 0);
    if (cmd_pressed(Cmd::ToggleBookmark)) toggle_bookmark_current();
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

}  // namespace lr::app