// session.cpp — Lilith Reader 应用层：会话与状态
//
// 职责（把"用户在读什么、读到哪里"组织成一个状态机，是 UI 与渲染层之间的控制中枢）：
//   · 承载 SessionController（文档/会话状态的唯一真源，ADR-086）：文档状态机（UiDoc）、
//     身份与阅读数据、阅读位置恢复、视图变换、逐页尺寸、目录、密码框
//   · 打开、关闭、认证结果在 SessionController::poll() **按文档代次 + 请求代次统一验收**
//   · 阅读位置与书签的读写（reader_state.bin，ADR-034）
//   · 视图变换与逐页尺寸、画布输入映射、缩放防抖与渲染请求
//
// 已按职责拆出的相邻 TU（ADR-087）：命令/按键绑定见 input_bindings.cpp，视图动效见
// view_motion.cpp，文本交互见 text_interaction.cpp，全文搜索见 search.cpp。
//
// 依赖：utils / canvas / render / reader_state + platform（px/偏好）；不直接 import document
// （经 render 层间接使用）。跨 TU 状态见 AppContext（app_internal.h）。

#include "app_internal.h"

namespace lr::app {

// ---------------- 标题 ----------------

void update_title() {
    if (!g_app.hwnd) return;
    std::wstring title = kWindowTitle;
    if (!g_app.session.doc.path_w.empty()) {
        title += L" — ";
        title += lr::file_name_of(g_app.session.doc.path_w);
    }
    SetWindowTextW(g_app.hwnd, title.c_str());
}

// ---------------- 视图变换 / 页尺寸 ----------------

// 把（可能已旋转的）页尺寸交给画布：90/270 交换宽高。
// 画布本身不感知旋转——旋转被折算成"页尺寸宽高互换"，布局/翻页/缩放全部复用。
void push_canvas_sizes() {
    const bool swap = (g_app.session.rotation == 90 || g_app.session.rotation == 270);
    auto conv = [swap](lr::PageSizePt s) {
        if (swap) { const float t = s.w; s.w = s.h; s.h = t; }
        return s;
    };
    const lr::PageSizePt def = conv(g_app.session.raw_default);
    g_app.canvas.set_default_size(def);
    if (g_app.session.raw_sizes.empty()) {
        g_app.canvas.set_uniform(g_app.session.doc.info.page_count, def);
    } else {
        std::vector<lr::PageSizePt> v;
        v.reserve(g_app.session.raw_sizes.size());
        for (const lr::PageSizePt& s : g_app.session.raw_sizes) v.push_back(conv(s));
        g_app.canvas.set_page_sizes(std::move(v));
    }
}

// 把当前旋转/配色下发给渲染层（变化即让全部纹理失效并整篇重渲）。
void apply_view_transform() {
    g_app.renderer->set_view_transform(g_app.session.rotation, static_cast<lr::PageScheme>(g_app.session.scheme));
}

// 直接设定旋转角（菜单按角度选）；与按键 R（+90 循环）共用同一套重排逻辑。
void set_rotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg == g_app.session.rotation) return;
    g_app.session.rotation = deg;
    const int anchor = g_app.canvas.current_page();
    push_canvas_sizes();                                   // 尺寸宽高互换 → 布局变化
    g_app.canvas.scroll_to_page(anchor < 0 ? 0 : anchor, 0.0f);
    apply_view_transform();
}

void rotate_view(int delta) { set_rotation(g_app.session.rotation + delta); }

// 直接设定纸张方案（0 原色 / 1 深色纸张 / 2 暖色）；按键 I/E 走 toggle。
void set_scheme(int mode) {
    if (g_app.session.scheme == mode) return;
    g_app.session.scheme = mode;
    apply_view_transform();
}

void toggle_scheme(int mode) { set_scheme(g_app.session.scheme == mode ? 0 : mode); }

// ---------------- 阅读位置与书签 ----------------

// 把 g_app.session.state 交给异步持久化服务（ADR-082）：本函数只在调用线程做纯序列化，写盘在工作线程。
void request_state_save() {
    if (g_app.session.persist) g_app.session.persist->request_save(g_app.session.state);
}

// 帧首采样"阅读数据落盘是否失败"（ADR-089）：失败首次出现时弹一次 toast 提醒（常驻告警在状态栏，
// 由 draw_status_bar 读 g_app.persist_failed 呈现）。服务本身已在失败时写日志，这里只负责让用户看得见。
void update_save_failure_notice() {
    const bool failed = g_app.session.persist && g_app.session.persist->last_save_failed();
    if (failed && !g_app.persist_failed) show_toast("阅读数据保存失败，详见 logs 日志");
    g_app.persist_failed = failed;
}

// 帧首节流（kAutoSaveSec）：阅读位置除退出时 flush 外，进程内也定期落一次盘 —— 此前位置
// **只在退出时写**（main.cpp 的 WM_DESTROY），一旦程序异常结束（崩溃 / 被强杀），从上次退出
// 到崩溃之间的阅读进度会全部丢失。位置签名（含 doc_key，故换书即变）相同则跳过，避免空闲时
// 反复序列化；真正的写盘由 PersistService 的防抖窗口合并，UI 线程只做一次便宜的序列化。
void maybe_autosave_reading_state() {
    if (g_app.session.doc_key == 0 || g_app.session.doc.kind != UiDoc::Kind::Reading) return;
    const double now = ImGui::GetTime();
    if (now - g_app.session.autosave_t < kAutoSaveSec) return;
    g_app.session.autosave_t = now;

    std::uint64_t h = 1469598103934665603ull;   // FNV-1a：把位置各分量揉成一个签名
    auto mix = [&](long long v) {
        h ^= static_cast<std::uint64_t>(v);
        h *= 1099511628211ull;
    };
    const auto cs = g_app.canvas.state();
    mix(static_cast<long long>(g_app.session.doc_key));
    mix(g_app.canvas.current_page());
    mix(static_cast<long long>(cs.zoom * 1000.0f + (cs.zoom >= 0.0f ? 0.5f : -0.5f)));
    mix(cs.columns);
    mix(g_app.session.rotation);
    mix(cs.fit_width ? 1 : 0);
    mix(cs.spread ? 1 : 0);
    mix(g_app.session.scheme);
    const long long sig = static_cast<long long>(h);
    if (sig == g_app.session.autosave_sig) return;
    g_app.session.autosave_sig = sig;
    save_reading_state();
}

void SessionController::save() {
    if (g_app.session.doc_key == 0 || g_app.session.doc.kind != UiDoc::Kind::Reading) return;
    lr::DocRecord& r = g_app.session.state.upsert(g_app.session.doc_key);
    const int page = g_app.canvas.current_page();
    r.page = page < 0 ? 0 : page;
    r.zoom = g_app.canvas.state().zoom;
    r.columns = g_app.canvas.state().columns;
    r.rotation = g_app.session.rotation;
    r.fit_width = g_app.canvas.state().fit_width;
    r.spread = g_app.canvas.state().spread;
    r.scheme = g_app.session.scheme;
    // 身份随每次落盘刷新：将来换键方案时能按位置重算，也能给管理窗口显示"上次在哪 / 还在哪"
    r.path_key = g_app.session.identity.path;
    r.page_count = g_app.session.identity.page_count;
    lr::remember_location(r, g_app.session.identity.path_u8);
    request_state_save();  // 书签在增删时已写入 g_app.session.state，这里不覆盖
}

// ---- 身份解析与阅读数据维护（ADR-062）----
//
// 打开成功时才解析：内容指纹由**工作线程**在打开前算好随 DocState 带回来（UI 线程不读
// 文件内容，ADR-009），页数也只在打开后才知道（页数不同 = 不是同一版，不能继承书签）。
void SessionController::resolve_identity(std::uint64_t content_fp) {
    g_app.session.identity = lr::DocIdentity{};
    g_app.session.identity.content = content_fp;
    g_app.session.identity.path = lr::document_key(g_app.session.doc.path_w);   // 元数据调用，微秒级（ADR-009 取舍）
    g_app.session.identity.page_count = g_app.session.doc.info.page_count;
    g_app.session.identity.path_u8 = lr::wide_to_utf8(g_app.session.doc.path_w);

    const bool smart = g_app.prefs.smart_match != kSmartMatchOff;
    const lr::DocMatch m = lr::locate(g_app.session.state, g_app.session.identity, smart);
    if (m.index < 0) {
        g_app.session.doc_key = lr::primary_key(g_app.session.identity);   // 新文档：主键 = 指纹（取不到则路径键）
        return;
    }

    // 命中：**先沿用**（绝不因为询问流程丢进度），再按设置决定要不要问一句。
    const lr::DocRecord& hit = g_app.session.state.docs[static_cast<std::size_t>(m.index)].second;
    const std::string old_path = lr::last_location(hit);
    const int old_page = hit.page;
    const int old_marks = static_cast<int>(hit.bookmarks.size());
    g_app.session.doc_key = lr::adopt(g_app.session.state, static_cast<std::size_t>(m.index), g_app.session.identity);

    if (m.relocated && g_app.prefs.smart_match == kSmartMatchAsk) {
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
    if (g_app.session.doc_key == 0 || g_app.session.doc.kind != UiDoc::Kind::Reading) return;
    const std::string me = g_app.session.identity.path_u8;

    std::size_t idx = g_app.session.state.docs.size();
    for (std::size_t i = 0; i < g_app.session.state.docs.size(); ++i)
        if (g_app.session.state.docs[i].first == g_app.session.doc_key) { idx = i; break; }

    if (idx < g_app.session.state.docs.size()) {
        lr::DocRecord& r = g_app.session.state.docs[idx].second;
        for (auto it = r.locations.begin(); it != r.locations.end(); ++it) {
            if (*it == me) { r.locations.erase(it); break; }
        }
        if (r.locations.empty()) {
            g_app.session.state.docs.erase(g_app.session.state.docs.begin() + static_cast<std::ptrdiff_t>(idx));
        } else {
            // 还有别的份：这条退回按"剩下那一处"的路径键认，腾出内容指纹
            const std::uint64_t rk = lr::document_key(lr::utf8_to_wide(r.locations.front()));
            if (rk != 0 && g_app.session.state.find(rk) == nullptr) (void)lr::rekey(g_app.session.state, idx, rk);
        }
    }

    // 当前这份：新起一条按路径键认的记录，从零开始
    g_app.session.doc_key = g_app.session.identity.path;
    if (g_app.session.doc_key != 0) {
        lr::DocRecord& nr = g_app.session.state.upsert(g_app.session.doc_key);
        nr.page = 0;
        nr.bookmarks.clear();
        nr.path_key = g_app.session.identity.path;
        nr.page_count = g_app.session.identity.page_count;
        lr::remember_location(nr, me);
    }
    request_state_save();
    g_app.session.restore_pending = false;      // 首帧待恢复的位置作废
    request_jump_scroll(0, 0.0f);
}

void clear_reading_data(std::uint64_t key) {
    if (key == 0) return;
    g_app.session.state.erase(key);
    // 删的正是当前在读的那本：解除本次会话的绑定（不再写入），下次打开按新文档处理。
    if (key == g_app.session.doc_key) g_app.session.doc_key = 0;
    request_state_save();
}

void clear_all_reading_data() {
    g_app.session.state.docs.clear();
    g_app.session.doc_key = 0;
    request_state_save();
}

int unknown_reading_data_count() {
    int n = 0;
    for (const auto& kv : g_app.session.state.docs)
        if (kv.second.locations.empty()) ++n;
    return n;
}

// 只清"位置未知"的那批：升级前的 v1 记录没存过路径，显示不出文档名，也永远认不出来
// （除非再打开一次同一个文件让它按路径键命中）。它们对用户是没有信息的噪音，单独给个入口。
void clear_unknown_reading_data() {
    std::vector<std::pair<std::uint64_t, lr::DocRecord>> keep;
    keep.reserve(g_app.session.state.docs.size());
    for (auto& kv : g_app.session.state.docs) {
        if (kv.second.locations.empty()) {
            if (kv.first == g_app.session.doc_key) g_app.session.doc_key = 0;   // 正在读的那本被清掉：解除绑定
            continue;
        }
        keep.push_back(std::move(kv));
    }
    if (keep.size() == g_app.session.state.docs.size()) return;     // 没有可清的
    g_app.session.state.docs = std::move(keep);
    request_state_save();
}

bool current_page_has_bookmark() {
    const lr::DocRecord* r = g_app.session.state.find(g_app.session.doc_key);
    if (!r) return false;
    const int page = g_app.canvas.current_page();
    for (const lr::Bookmark& b : r->bookmarks)
        if (b.page == page) return true;
    return false;
}

void toggle_bookmark_current() {
    if (g_app.session.doc_key == 0 || g_app.session.doc.kind != UiDoc::Kind::Reading) return;
    const int page = g_app.canvas.current_page();
    if (page < 0) return;
    lr::DocRecord& r = g_app.session.state.upsert(g_app.session.doc_key);
    for (auto it = r.bookmarks.begin(); it != r.bookmarks.end(); ++it) {
        if (it->page == page) {
            r.bookmarks.erase(it);
            request_state_save();
            return;
        }
    }
    r.bookmarks.push_back(lr::Bookmark{ page, {} });
    request_state_save();
}

void remove_bookmark_at(int index) {
    if (g_app.session.doc_key == 0) return;
    lr::DocRecord* r = nullptr;
    for (auto& kv : g_app.session.state.docs)
        if (kv.first == g_app.session.doc_key) { r = &kv.second; break; }
    if (r == nullptr || index < 0 || index >= static_cast<int>(r->bookmarks.size())) return;
    r->bookmarks.erase(r->bookmarks.begin() + index);
    request_state_save();
}

// ---------------- 文档状态机 ----------------

void SessionController::reset() {
    ++doc_gen;   // 文档代次推进：作废任何在途结果的归属（ADR-086）
    g_app.session.doc.kind = UiDoc::Kind::None;
    g_app.session.doc.path_w.clear();
    g_app.session.doc.name_u8.clear();
    g_app.session.doc.ext_u8.clear();
    g_app.session.doc.error = lr::DocError::Ok;
    g_app.session.doc.detail_u8.clear();
    g_app.session.doc.info = lr::DocumentInfo{};
    g_app.session.doc.request_id = 0;
    g_app.canvas = lr::Canvas{};
    g_app.want_scale = -1.0f;
    g_app.last_target_scale = -1.0f;
    g_app.zoom_dirty_since = -1.0;
    g_app.canvas_scale = -1.0f;
    g_app.scroll_dir = 0;
    g_app.prev_scroll_y = 0.0f;
    g_app.session.doc_key = 0;
    g_app.session.identity = lr::DocIdentity{};
    g_app.session.outline.clear();
    g_app.session.raw_sizes.clear();
    g_app.page_fade.clear();
    g_app.session.restore_pending = false;
    // 动效状态：关闭文档后必须归零，否则下一个文档会带着上一个的待定量/滑行起点开场
    g_app.scroll_pending = 0.0f;
    g_app.jump_repin_page = -1;
    g_app.zoom_anim = false;
    g_app.zoom_end_fit = false;
    g_app.top_bar_h = -1.0f;
    g_app.sidebar_w = 0.0f;
    g_app.scroll_drag = false;
    g_app.scroll_drag_off = 0.0f;
    g_app.scroll_hover = false;
    g_app.session.rotation = 0;
    g_app.session.scheme = 0;
    g_app.show_sidebar = false;
    // 文本交互与检索状态必须随文档一起清掉 —— 否则换文档后
    // 选区/命中仍指向旧文档的页与字符下标（会复制出错内容、或高亮到无关位置）。
    g_app.sel = Selection{};
    g_app.select_all_pending = false;
    g_app.content = lr::PageContent{};
    g_app.content_page = -1;
    g_app.content_want = -1;
    g_app.content_since = -1.0;
    g_app.ctx_page = -1;
    g_app.ctx_link_valid = false;
    g_app.ctx_image_valid = false;
    g_app.hover_page = g_app.hover_link = g_app.hover_char = -1;
    g_app.press_link_valid = false;
    g_app.press_on_text = false;
    g_app.press_page = -1;
    g_app.toast.clear();
    g_app.toast_since = -1.0;
    std::memset(g_app.search_buf, 0, sizeof g_app.search_buf);
    search_clear();
    g_app.session.open_password = false;
    g_app.session.auth_pending = false;
    std::memset(g_app.session.password_buf, 0, sizeof g_app.session.password_buf);  // 明文密码整个缓冲清零（ADR-039）
    g_app.session.password_error.clear();
    update_title();
}

// 文档打开成功 → 初始化画布并进入阅读态（恢复该文档记忆的阅读位置与视图参数）
void SessionController::enter_reading() {
    g_app.session.raw_default.w = g_app.session.doc.info.page_width_pt > 0.0f ? g_app.session.doc.info.page_width_pt : 595.0f;
    g_app.session.raw_default.h = g_app.session.doc.info.page_height_pt > 0.0f ? g_app.session.doc.info.page_height_pt : 842.0f;
    g_app.session.raw_sizes.clear();
    g_app.session.raw_sizes.reserve(g_app.session.doc.info.page_sizes.size());
    for (const lr::PageSize& ps : g_app.session.doc.info.page_sizes) {
        lr::PageSizePt e;
        e.w = ps.width_pt > 0.0f ? ps.width_pt : g_app.session.raw_default.w;
        e.h = ps.height_pt > 0.0f ? ps.height_pt : g_app.session.raw_default.h;
        g_app.session.raw_sizes.push_back(e);
    }

    // 恢复记忆状态（无记录则用默认：fit-width / 单列 / 不旋转 / 正常配色）
    const lr::DocRecord* rec = g_app.session.state.find(g_app.session.doc_key);
    g_app.session.rotation = rec ? rec->rotation : 0;
    g_app.session.scheme = rec ? rec->scheme : 0;

    lr::CanvasState st;  // 默认：fit_width=true, columns=1
    st.margin_px = px(kCanvasMarginPx);   // 屏幕像素 → 随 DPI
    st.gap_ratio = gap_ratio_pref();      // 列宽比例 → 随缩放（ADR-029），不随 DPI
    if (rec) {
        st.zoom = rec->zoom;
        st.columns = rec->columns;
        st.fit_width = rec->fit_width;
        st.spread = rec->spread;
    }

    g_app.canvas = lr::Canvas{};
    g_app.canvas.set_margin_gap(st.margin_px, st.gap_ratio);
    push_canvas_sizes();   // 逐页真实尺寸（旋转折算），异构 PDF 不形变
    g_app.canvas.set_state(st);

    g_app.want_scale = -1.0f;  // 首帧立即采用目标倍率，不走防抖
    g_app.last_target_scale = -1.0f;
    g_app.zoom_dirty_since = -1.0;
    g_app.canvas_scale = ui_scale();  // 留白已按当前缩放写入
    g_app.scroll_dir = 0;             // 方向未定：首帧两侧都预取
    g_app.prev_scroll_y = 0.0f;

    // 阅读位置待首帧视口就绪后恢复（fit-width 派生 zoom 依赖视口尺寸，此刻视口还是 0）
    g_app.session.restore_pending = true;
    g_app.session.restore_page = rec ? rec->page : 0;

    g_app.page_fade.assign(static_cast<std::size_t>(std::max(0, g_app.session.doc.info.page_count)), PageFade{});
    g_app.toolbar_visible = true;         // 新文档从显示顶栏开始
    g_app.toolbar_idle_since = -1.0;
    // 动效状态复位：开文档不播动画（顶栏直接展开，首帧就是稳定态）
    g_app.scroll_pending = 0.0f;
    g_app.jump_repin_page = -1;
    g_app.zoom_anim = false;
    g_app.zoom_end_fit = false;
    g_app.top_bar_h = px(kTopBarH);
    g_app.sidebar_w = 0.0f;               // 开文档不播侧栏动画（侧栏本就没开）
    g_app.scroll_drag = false;
    g_app.scroll_hover = false;

    apply_view_transform();            // 旋转/配色下发（首次会触发整篇重渲）
    g_app.session.outline = g_app.renderer->outline(); // 目录快照（一次性）
}

void SessionController::open(std::wstring path) {
    ++doc_gen;            // 新文档：推进文档代次（ADR-086）
    pending_gen = doc_gen;
    path = lr::to_absolute(path);
    g_app.session.doc.path_w = path;
    g_app.session.doc.name_u8 = lr::wide_to_utf8(lr::file_name_of(path));
    g_app.session.doc.ext_u8 = lr::wide_to_utf8(lr::extension_of(path));
    g_app.session.doc.error = lr::DocError::Ok;
    g_app.session.doc.detail_u8.clear();
    g_app.session.doc.info = lr::DocumentInfo{};
    g_app.canvas = lr::Canvas{};
    g_app.session.outline.clear();
    g_app.show_sidebar = false;
    g_app.session.auth_pending = false;  // 新开文档：清掉上一次可能残留的认证在途标记
    // 身份要等打开成功（指纹由工作线程算、页数只有打开后才知道）才解析，见 resolve_identity
    g_app.session.doc_key = 0;
    g_app.session.identity = lr::DocIdentity{};
    g_app.confirm_open = false;          // 上一次可能还挂着一个"是否沿用"的询问
    g_app.confirm_kind = ConfirmKind::None;

    // 扩展名闸门（ADR-013）：纯字符串策略、不碰磁盘，保留在 UI 线程做即时拒绝。
    // 文件**存在性**不再在 UI 线程预检（那是磁盘 I/O，违反「UI 零磁盘」，ADR-081）：
    // 交给后台打开，由 DocError::NotFound 经 poll → Failed 如实呈现。
    if (!lr::is_supported(path)) {
        g_app.session.doc.kind = UiDoc::Kind::Rejected;
        g_app.session.doc.error = lr::DocError::Unsupported;
        update_title();
        return;
    }

    g_app.session.doc.kind = UiDoc::Kind::Opening;
    g_app.session.doc.request_id = g_app.renderer->open(path);
    update_title();
}

void SessionController::close() {
    save();  // 关闭前落盘阅读位置（书签已实时落盘）
    if (g_app.session.doc.kind == UiDoc::Kind::Opening || g_app.session.doc.kind == UiDoc::Kind::Reading ||
        g_app.session.doc.kind == UiDoc::Kind::NeedsPassword)
        g_app.renderer->close();
    reset();
}

void SessionController::poll() {
    // 认证请求在途时，文档仍停在 NeedsPassword，但请求已投出，需要在这里接收结果。
    const bool awaiting_auth = (g_app.session.doc.kind == UiDoc::Kind::NeedsPassword && g_app.session.auth_pending);
    if (g_app.session.doc.kind != UiDoc::Kind::Opening && !awaiting_auth) return;

    const lr::DocState snap = g_app.renderer->doc_state();
    // **唯一验收点**（ADR-081/086）：请求代次（快照 id）与文档代次都须匹配当前在途请求，
    // 否则说明这是上一份文档 / 上一次请求的迟到结果，原样丢弃。
    if (snap.id != g_app.session.doc.request_id) return;
    if (pending_gen != doc_gen) return;
    if (snap.phase == lr::DocPhase::Opening) return;

    switch (snap.phase) {
    case lr::DocPhase::Ready:
        g_app.session.doc.kind = UiDoc::Kind::Reading;
        g_app.session.doc.info = snap.info;
        g_app.session.doc.error = lr::DocError::Ok;
        g_app.session.auth_pending = false;
        g_app.session.open_password = false;
        std::memset(g_app.session.password_buf, 0, sizeof g_app.session.password_buf);  // 解锁成功：明文密码不再需要（ADR-039）
        resolve_identity(snap.file_fingerprint);  // 先定位记录，再恢复阅读位置
        enter_reading();
        break;
    case lr::DocPhase::Failed:
        g_app.session.doc.error = snap.error;
        g_app.session.doc.detail_u8 = snap.detail_u8;
        if (snap.error == lr::DocError::NeedsPassword) {
            // 密码错误：**保持弹窗打开**（只更新错误提示），不关→开跳变，避免闪烁。
            g_app.session.doc.kind = UiDoc::Kind::NeedsPassword;
            g_app.session.auth_pending = false;
            g_app.session.open_password = true;
            g_app.session.password_error = snap.detail_u8.find("invalid password") != std::string::npos
                                   ? "密码错误，请重试"
                                   : std::string();
        } else {
            g_app.session.doc.kind = UiDoc::Kind::Failed;
            g_app.session.auth_pending = false;
        }
        break;
    default:  // Idle：被显式关闭
        reset();
        return;
    }
    update_title();
}

void SessionController::submit_password() {
    if (g_app.session.password_buf[0] == '\0') { g_app.session.password_error = "请输入密码"; return; }
    // 保持 kind=NeedsPassword（背景不变）、弹窗不关闭：认证结果由 poll 接收。
    // 这样"解锁/输错"都不会出现弹窗关闭再打开的闪烁。
    g_app.session.auth_pending = true;
    g_app.session.password_error.clear();
    pending_gen = doc_gen;   // 认证请求沿用当前文档代次（ADR-086）
    g_app.session.doc.request_id = g_app.renderer->authenticate(g_app.session.password_buf);
}

// ---- 会话 API 薄包装（调用点不变；实现见 SessionController，ADR-086）----

void reset_doc_state()     { g_app.session.reset(); }
void enter_reading()       { g_app.session.enter_reading(); }
void request_open_document(std::wstring path) { g_app.session.open(std::move(path)); }
void close_document()      { g_app.session.close(); }
void poll_document()       { g_app.session.poll(); }
void submit_password()     { g_app.session.submit_password(); }
void save_reading_state()  { g_app.session.save(); }

// ---------------- 侧栏 / 跳页 ----------------

void set_sidebar(bool on, int tab) {
    g_app.show_sidebar = on;
    if (on && tab >= 0) {
        g_app.sidebar_tab = tab;
        // ImGui 的 TabBar 自带选中态：只改 g_app.sidebar_tab 不会真的切过去（Ctrl+F 只开了
        // 侧栏却停在"目录"就是这条）。下一次绘制时对该分栏带一次 SetSelected 才生效。
        g_app.sidebar_tab_want = tab;
    }
}

void open_jump_popup() {
    g_app.open_jump = true;
    g_app.jump_page = g_app.canvas.current_page() + 1;
}

// ---------------- 画布输入 ----------------

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
// 全部由 kCmds / g_app.binds 决定，本函数只负责"命令被按下时做什么"。
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

    // 文本交互：**先于平移**处理，因为它要决定"这一串左键归谁"。
    // 规则（人工确认的交互设计）：指针悬停在可选文本上时光标变成 I 型，
    // **此时拖拽 = 选择文本**；悬停不到文本时拖拽仍是平移（1:1 跟手，手感不变）。
    const bool text_took_drag = handle_text_interaction(origin, size, hovered);

    // 左键拖拽平移（位移直接取鼠标物理像素增量，不做缩放换算）。
    // **刻意不做平滑**：直接操纵必须 1:1 跟手；同时掐掉滚轮残留的平滑尾巴。
    // 在滚动条上按下/拖动时不进入平移 —— 否则"拖滚动条"变成"拖页面"（实测反馈）。
    if (!text_took_drag && hovered && !g_app.scroll_drag &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        g_app.scroll_pending = 0.0f;
        g_app.jump_repin_page = -1;
        g_app.canvas.scroll_by(-io.MouseDelta.x, -io.MouseDelta.y);
    }

    // 中键拖拽平移：**任何位置都可用**。
    // 为什么必须有：左键在文字上已被"选择文本"接管，而放大到文字铺满视口时，
    // 左键处处都是选择 —— 没有这个兜底就无法平移。桌面阅读器的通行做法。
    if (hovered && !g_app.scroll_drag && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        g_app.scroll_pending = 0.0f;
        g_app.jump_repin_page = -1;
        g_app.canvas.scroll_by(-io.MouseDelta.x, -io.MouseDelta.y);
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
    if (cmd_pressed(Cmd::LastPage))   request_jump_scroll(g_app.canvas.page_count() - 1, 0.0f);
    if (cmd_pressed(Cmd::NextRow))    scroll_by_rows(+1);
    if (cmd_pressed(Cmd::PrevRow))    scroll_by_rows(-1);
    if (cmd_pressed(Cmd::JumpPage))   open_jump_popup();

    if (cmd_pressed(Cmd::ZoomIn))     zoom_by_animated(kZoomStep, cx, cy);
    if (cmd_pressed(Cmd::ZoomOut))    zoom_by_animated(1.0f / kZoomStep, cx, cy);
    if (cmd_pressed(Cmd::FitWidth))   fit_to_width_animated();

    if (cmd_pressed(Cmd::Col1))         g_app.canvas.set_columns(1);
    if (cmd_pressed(Cmd::Col2))         g_app.canvas.set_columns(2);
    if (cmd_pressed(Cmd::Col3))         g_app.canvas.set_columns(3);
    if (cmd_pressed(Cmd::Col4))         g_app.canvas.set_columns(4);
    if (cmd_pressed(Cmd::ToggleSpread)) g_app.canvas.set_spread(!g_app.canvas.state().spread);
    if (cmd_pressed(Cmd::RotateCW))     rotate_view(90);
    if (cmd_pressed(Cmd::ToggleDark)) toggle_scheme(1);
    if (cmd_pressed(Cmd::ToggleWarm))  toggle_scheme(2);

    if (cmd_pressed(Cmd::ToggleSidebar))  set_sidebar(!g_app.show_sidebar, 0);
    if (cmd_pressed(Cmd::ToggleBookmark)) toggle_bookmark_current();

    // 文本。注意本函数只在"无文本输入"时被调用（见 handle_canvas_input 的门），
    // 因此搜索框/密码框有焦点时 Ctrl+C / Ctrl+A 不会被这里抢走。
    if (cmd_pressed(Cmd::Copy))      selection_copy();
    if (cmd_pressed(Cmd::SelectAll)) selection_select_all();
    if (cmd_pressed(Cmd::OpenSearch)) {
        set_sidebar(true, 3);
        g_app.search_focus = true;
    }
}

// 全局命令：任何状态都可用。由 draw_shell 在「无弹窗/无文本输入/未在捕获按键」时调用
// （见 ui.cpp 的 any_dialog_open）。有弹窗时不派发，避免 Esc 之类"先关弹窗又触发命令"。
void handle_global_commands() {
    if (cmd_pressed(Cmd::ToggleDebug))      g_app.show_debug ^= 1;
    if (cmd_pressed(Cmd::ToggleFullscreen)) g_app.request_fullscreen_toggle = true;  // 帧间执行（ADR-060）
    if (cmd_pressed(Cmd::OpenFile))         g_app.request_open_dialog = true;
    if (cmd_pressed(Cmd::OpenSettings))     g_app.show_settings = true;
    if (cmd_pressed(Cmd::OpenKeys))         { g_app.show_settings = true; g_app.settings_open_tab = 2; }

    // 画布右键菜单的键盘等价入口（Shift+F10 / 菜单键）：标准 Windows 习惯、
    // 属"无鼠标兜底"而非可自定义命令，故仍写死。
    if (g_app.session.doc.kind == UiDoc::Kind::Reading &&
        ((ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false)) ||
         ImGui::IsKeyPressed(ImGuiKey_Menu, false)))
        g_app.open_canvas_ctx = true;
}

// ---------------- 缩放防抖与渲染请求 ----------------

// 缩放防抖：目标倍率稳定 150ms 后，才按新倍率请求高清重渲染；
// 期间 g_app.want_scale 保持旧值，页面用现有纹理显示（双线性放大，不闪白）。
// 注意目标是**目标倍率**而不是显示倍率（ADR-047）：缩放插值进行中显示值每帧都在变，
// 若盯显示值，防抖会被动画不断重置，高清重渲要等动画结束后再等 150ms 才开始；
// 盯目标倍率则"最后一次缩放意图后 150ms"即触发，正好在动画收敛时换入高清。
void update_want_scale() {
    const float target = view_zoom_target();
    const double now = ImGui::GetTime();

    if (g_app.want_scale < 0.0f) {  // 首帧（刚进入阅读态）：立即采用
        g_app.want_scale = target;
        g_app.last_target_scale = target;
        g_app.zoom_dirty_since = -1.0;
        return;
    }
    if (std::fabs(target - g_app.want_scale) <= 0.002f) {  // 已与投递倍率一致
        g_app.last_target_scale = target;
        g_app.zoom_dirty_since = -1.0;
        return;
    }
    if (std::fabs(target - g_app.last_target_scale) > 0.002f) {
        g_app.last_target_scale = target;
        g_app.zoom_dirty_since = now;  // 倍率仍在变化，重置计时（真防抖）
    }
    if (g_app.zoom_dirty_since >= 0.0 && (now - g_app.zoom_dirty_since) >= kZoomDebounceSec) {
        g_app.want_scale = target;
        g_app.zoom_dirty_since = -1.0;
    }
}

// 可见页 + 预加载页 → 渲染请求（可见页优先）。
// 预加载策略（ADR-032）：可见行 ±1 行，且**方向感知** —— 向下滚只预取下方一行、
// 向上滚只预取上方一行。理由：滚动有方向，反向的预加载在下一帧多半就被逐出，
// 纯属浪费渲染线程与显存；方向未定（刚打开/跳页后）时两侧都预取。
void emit_wants() {
    const int n = g_app.canvas.page_count();
    if (n <= 0) return;
    const float scale = g_app.want_scale > 0.0f ? g_app.want_scale : g_app.canvas.effective_zoom();

    const int first = g_app.canvas.visible_first();
    const int last = g_app.canvas.visible_last();
    if (first < 0 || last < first) return;

    const int r0 = g_app.canvas.row_of(first);
    const int r1 = g_app.canvas.row_of(last);
    int pf = first, pl = last;
    if (g_app.scroll_dir >= 0 && r1 + 1 < g_app.canvas.rows())
        pl = std::max(pl, g_app.canvas.row_page_end(r1 + 1));      // 下方一行
    if (g_app.scroll_dir <= 0 && r0 - 1 >= 0)
        pf = std::min(pf, g_app.canvas.first_page_in_row(r0 - 1)); // 上方一行

    std::vector<lr::RenderWant> wants;
    wants.reserve(static_cast<std::size_t>(pl - pf + 1));
    for (int i = first; i <= last && i < n; ++i) wants.push_back({ i, scale });
    for (int i = pf; i <= pl; ++i)
        if (i >= 0 && i < n && (i < first || i > last)) wants.push_back({ i, scale });

    g_app.renderer->set_wanted(std::move(wants));
}

}  // namespace lr::app
