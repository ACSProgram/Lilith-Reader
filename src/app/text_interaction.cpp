// text_interaction.cpp — Lilith Reader 应用层：文本交互与剪贴板（Phase 8）
//
// 从 session.cpp 拆出（ADR-087）。坐标纪律：本 TU 内部只用"未旋转页面 pt"（page_map.h），
// 屏幕上的一切（命中测试、绘制、链接热区）都先经 page_map 折算，故旋转视图下没有额外分支。
// 线程纪律：零 fz_*；文本布局由渲染工作线程抽好后发布快照（request_page_content /
// take_page_content），本 TU 只做纯浮点命中测试。

#include "app_internal.h"

namespace lr::app {

// ============================================================================
// 文本交互 / 剪贴板（Phase 8）
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

// 由当前内容快照重算选区几何（复制端点 + 按行合并的高亮矩形），并**冻结**进 g_app.sel。
// 冻结的理由见 app_internal.h 的 Selection 注释。
void selection_rebuild() {
    g_app.sel.rects.clear();
    g_app.sel.ax_pt = g_app.sel.ay_pt = g_app.sel.bx_pt = g_app.sel.by_pt = 0.0f;
    if (!g_app.sel.active || g_app.sel.page < 0 || g_app.sel.page != g_app.content_page) return;
    const int cnt = static_cast<int>(g_app.content.chars.size());
    if (cnt <= 0 || g_app.sel.anchor < 0 || g_app.sel.head < 0) return;
    if (g_app.sel.anchor >= cnt || g_app.sel.head >= cnt) return;

    const int lo = std::min(g_app.sel.anchor, g_app.sel.head);
    const int hi = std::max(g_app.sel.anchor, g_app.sel.head);

    g_app.sel.ax_pt = sel_point_x(g_app.content.chars[lo].quad, false);
    g_app.sel.ay_pt = sel_point_y(g_app.content.chars[lo].quad);
    g_app.sel.bx_pt = sel_point_x(g_app.content.chars[hi].quad, true);
    g_app.sel.by_pt = sel_point_y(g_app.content.chars[hi].quad);

    // 按行合并：逐字符画四边形会因相邻框重叠而出现"深一块浅一块"，看着像马赛克。
    // 同一行内取并集，得到"一行一段"的干净色块（与桌面阅读器的观感一致）。
    int     line = -1;
    SelRect acc{};
    for (int i = lo; i <= hi; ++i) {
        const lr::TextQuad& q = g_app.content.chars[static_cast<std::size_t>(i)].quad;
        const int cl = g_app.content.chars[static_cast<std::size_t>(i)].line;
        const float x0 = std::min(std::min(q.ulx, q.llx), std::min(q.urx, q.lrx));
        const float x1 = std::max(std::max(q.ulx, q.llx), std::max(q.urx, q.lrx));
        const float y0 = std::min(std::min(q.uly, q.ury), std::min(q.lly, q.lry));
        const float y1 = std::max(std::max(q.uly, q.ury), std::max(q.lly, q.lry));
        if (cl != line) {
            if (line >= 0) g_app.sel.rects.push_back(acc);
            line = cl;
            acc = SelRect{ x0, y0, x1, y1 };
        } else {
            acc.x0 = std::min(acc.x0, x0);
            acc.y0 = std::min(acc.y0, y0);
            acc.x1 = std::max(acc.x1, x1);
            acc.y1 = std::max(acc.y1, y1);
        }
    }
    if (line >= 0) g_app.sel.rects.push_back(acc);
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
    const HINSTANCE r = ShellExecuteW(g_app.hwnd, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) show_toast("无法打开链接");
}

}  // namespace

bool page_view_of(const ImVec2& origin, int page, PageView& view, PageGeom& geom) {
    const int n = g_app.canvas.page_count();
    if (page < 0 || page >= n) return false;
    const lr::PageRect r = g_app.canvas.page_rect(page);
    view.x = origin.x + r.x;
    view.y = origin.y + r.y;
    view.w = r.w;
    view.h = r.h;
    // 未旋转页面尺寸：优先逐页真实尺寸（ADR-022），整表缺失时退回首页尺寸
    if (static_cast<std::size_t>(page) < g_app.session.raw_sizes.size()) {
        geom.w_pt = g_app.session.raw_sizes[static_cast<std::size_t>(page)].w;
        geom.h_pt = g_app.session.raw_sizes[static_cast<std::size_t>(page)].h;
    } else {
        geom.w_pt = g_app.session.raw_default.w;
        geom.h_pt = g_app.session.raw_default.h;
    }
    return view.w > 0.0f && view.h > 0.0f && geom.w_pt > 0.0f && geom.h_pt > 0.0f;
}

int page_at_screen(const ImVec2& origin, const ImVec2& screen) {
    const int n = g_app.canvas.page_count();
    const int vf = g_app.canvas.visible_first();
    const int vl = g_app.canvas.visible_last();
    if (vf < 0 || n <= 0) return -1;
    for (int i = vf; i <= vl && i < n; ++i) {
        const lr::PageRect r = g_app.canvas.page_rect(i);
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
    if (page != g_app.content_page) return -1;
    const int cnt = static_cast<int>(g_app.content.chars.size());
    if (cnt <= 0) return -1;

    // 1) 选行。带"行高 1/4"的容差：点在行间空白（行距小于半个字高）仍算落在该行。
    //    **超出容差就是"此处无文字"** —— 悬停/按下走这条路；拖拽时（clamp）才退回最近行。
    int   hit_line = -1;
    int   near_line = -1;
    float near_d = 0.0f;
    for (std::size_t i = 0; i < g_app.content.lines.size(); ++i) {
        const lr::TextLine& L = g_app.content.lines[i];
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
    const float pad_x = line_text_height(g_app.content.lines[static_cast<std::size_t>(line)]) * 0.5f;
    int   best = -1;
    float bd = 0.0f;
    for (int i = 0; i < cnt; ++i) {
        const lr::TextChar& c = g_app.content.chars[static_cast<std::size_t>(i)];
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
    if (page != g_app.content_page) return -1;
    for (std::size_t i = 0; i < g_app.content.links.size(); ++i) {
        const lr::PageLink& l = g_app.content.links[i];
        // 热区在未旋转 pt 空间是轴对齐矩形；90° 的整数倍旋转仍把矩形映成矩形，
        // 故"在未旋转空间做点在矩形内测试"与"在屏幕上测"等价。
        if (x_pt >= l.x0 && x_pt <= l.x1 && y_pt >= l.y0 && y_pt <= l.y1)
            return static_cast<int>(i);
    }
    return -1;
}

int image_index_at(int page, float x_pt, float y_pt) {
    if (page != g_app.content_page) return -1;
    for (std::size_t i = 0; i < g_app.content.images.size(); ++i) {
        const lr::PageImageRect& r = g_app.content.images[i];
        if (x_pt >= r.x0 && x_pt <= r.x1 && y_pt >= r.y0 && y_pt <= r.y1)
            return static_cast<int>(i);
    }
    return -1;
}

void selection_clear() {
    g_app.sel = Selection{};
    g_app.select_all_pending = false;
}

void selection_select_all() {
    // 全选的是"当前正在读的那一页"。内容快照可能还没到（鼠标不在画布上），
    // 这时先记一个待办，等快照到达后在 update_hovered_content 里补做。
    const int page = (g_app.content_page >= 0) ? g_app.content_page : g_app.canvas.current_page();
    if (page < 0) return;
    if (page != g_app.content_page) {
        g_app.renderer->request_page_content(page);
        g_app.content_want = page;
        g_app.select_all_pending = true;
        return;
    }
    const int cnt = static_cast<int>(g_app.content.chars.size());
    if (cnt <= 0) {
        show_toast("本页没有可选择的文本");
        return;
    }
    g_app.sel = Selection{};
    g_app.sel.active = true;
    g_app.sel.page = page;
    g_app.sel.anchor = 0;
    g_app.sel.head = cnt - 1;
    selection_rebuild();
}

void selection_copy() {
    if (!g_app.sel.active || g_app.sel.page < 0) {
        show_toast("没有选中文本");
        return;
    }
    if (g_app.sel.rects.empty()) return;
    g_app.renderer->request_copy_text(g_app.sel.page, g_app.sel.ax_pt, g_app.sel.ay_pt, g_app.sel.bx_pt, g_app.sel.by_pt);
}

void copy_image_at_context() {
    if (g_app.ctx_page < 0) {
        show_toast("此处没有可复制的图片");
        return;
    }
    g_app.renderer->request_copy_image(g_app.ctx_page, g_app.ctx_x_pt, g_app.ctx_y_pt);
}

void open_link_at_context() {
    if (!g_app.ctx_link_valid) return;
    open_link(g_app.ctx_link);
}

void show_toast(std::string text) {
    g_app.toast = std::move(text);
    g_app.toast_since = ImGui::GetTime();
}

std::string toast_text() {
    if (g_app.toast.empty() || g_app.toast_since < 0.0) return {};
    if (ImGui::GetTime() - g_app.toast_since > kToastSec) return {};
    return g_app.toast;
}

bool handle_text_interaction(const ImVec2& origin, const ImVec2& size, bool hovered) {
    (void)size;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mp = io.MousePos;

    // ---- 1) 悬停：命中测试 + 光标形状 ----
    g_app.hover_page = hovered ? page_at_screen(origin, mp) : -1;
    g_app.hover_link = -1;
    g_app.hover_char = -1;
    float hx = 0.0f, hy = 0.0f;
    if (g_app.hover_page >= 0 && g_app.hover_page == g_app.content_page) {
        PageView view;
        PageGeom geom;
        if (page_view_of(origin, g_app.hover_page, view, geom) &&
            screen_to_page_pt(view, geom, g_app.session.rotation, mp.x, mp.y, hx, hy)) {
            g_app.hover_link = link_index_at(g_app.hover_page, hx, hy);
            // 严格命中：页边空白 / 图注之外的地方必须如实报告"没有文字"，
            // 否则光标会在整页上一直是 I 型（实测反馈）。
            g_app.hover_char = char_index_at(g_app.hover_page, hx, hy, /*clamp_to_nearest=*/false);
        }
    }
    // 链接优先于文本：链接常常叠在文字上，此时"手型"比"I 型"更能说明点下去会发生什么。
    // 悬停在**滚动条**上时不改光标：滚动条压在画布右缘之上，其下若有文字会误报成
    // "可选文本"（滚动条本身也不是选择区域）。
    const bool over_bar = g_app.scroll_drag || g_app.scroll_hover;
    if (hovered && !over_bar && g_app.hover_link >= 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    else if (hovered && !over_bar && g_app.hover_char >= 0)
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

    // ---- 2) 右键：记下菜单锚点（此刻鼠标还在画布上）----
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        g_app.ctx_page = g_app.hover_page;
        g_app.ctx_x_pt = hx;
        g_app.ctx_y_pt = hy;
        g_app.ctx_link_valid = (g_app.hover_link >= 0);
        if (g_app.ctx_link_valid) g_app.ctx_link = g_app.content.links[static_cast<std::size_t>(g_app.hover_link)];
        // 图片命中测试在这里做一次即可：菜单项要据此置灰，而菜单打开后鼠标就离开画布了。
        g_app.ctx_image_valid = (g_app.ctx_page >= 0 && image_index_at(g_app.ctx_page, hx, hy) >= 0);
    }

    // ---- 3) 左键按下 ----
    if (hovered && !g_app.scroll_drag && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_app.press_x = mp.x;
        g_app.press_y = mp.y;
        g_app.press_page = g_app.hover_page;
        g_app.press_link_valid = (g_app.hover_link >= 0);
        if (g_app.press_link_valid) g_app.press_link = g_app.content.links[static_cast<std::size_t>(g_app.hover_link)];
        g_app.press_on_text = (g_app.hover_char >= 0);
        if (g_app.press_on_text) {
            g_app.sel.active = true;
            g_app.sel.page = g_app.hover_page;
            g_app.sel.anchor = g_app.hover_char;
            g_app.sel.head = g_app.hover_char;
            g_app.sel.dragging = true;
            selection_rebuild();
        }
        // 按在非文本处**不立刻**清选区：还要等抬起时区分"单击"与"拖拽平移"。
        // 若在这里清，用户想平移一下再回来看选区就没了。
    }

    // ---- 4) 拖动：更新拖动端 ----
    bool took_drag = false;
    if (g_app.sel.dragging && g_app.press_on_text) {
        took_drag = true;   // 整段拖拽期间接管左键：平移必须让位
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_app.sel.page == g_app.content_page) {
            PageView view;
            PageGeom geom;
            if (page_view_of(origin, g_app.sel.page, view, geom)) {
                float x_pt = 0.0f, y_pt = 0.0f;
                if (screen_to_page_pt(view, geom, g_app.session.rotation, mp.x, mp.y, x_pt, y_pt)) {
                    // 拖拽用 clamp：拖到行尾之外要吸附到最后一个字符，选区能"到底"。
                    const int idx = char_index_at(g_app.sel.page, x_pt, y_pt, /*clamp_to_nearest=*/true);
                    if (idx >= 0) {
                        g_app.sel.head = idx;
                        selection_rebuild();
                    }
                }
            }
        }
    }

    // ---- 5) 抬起：区分单击与拖拽 ----
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        const float dx = mp.x - g_app.press_x;
        const float dy = mp.y - g_app.press_y;
        const float slop = px(kClickSlopPx);
        const bool is_click = (dx * dx + dy * dy) <= slop * slop;
        if (is_click) {
            if (g_app.press_link_valid) {
                open_link(g_app.press_link);
            } else if (g_app.press_on_text) {
                // 单击文字（未拖动）= 取消选区：与"点一下空白即取消"的桌面习惯一致
                selection_clear();
            }
        }
        g_app.sel.dragging = false;
        g_app.press_on_text = false;
        g_app.press_link_valid = false;
        g_app.press_page = -1;
    }
    return took_drag;
}

void update_hovered_content(const ImVec2& origin, bool hovered) {
    if (g_app.renderer == nullptr) return;

    // 1) 接收新快照
    if (g_app.content_want >= 0) {
        lr::PageContent pc;
        if (g_app.renderer->take_page_content(g_app.content_want, pc)) {
            g_app.content = std::move(pc);
            g_app.content_page = g_app.content_want;
            g_app.content_since = -1.0;
            if (g_app.select_all_pending) {
                g_app.select_all_pending = false;
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
        g_app.content_want = -1;
        g_app.content_since = -1.0;
        return;
    }
    const int want = page_at_screen(origin, ImGui::GetIO().MousePos);
    if (want < 0) return;

    const double now = ImGui::GetTime();
    if (want == g_app.content_page) return;   // 已就绪
    if (want != g_app.content_want) {
        g_app.renderer->request_page_content(want);
        g_app.content_want = want;
        g_app.content_since = now;
        return;
    }
    // 同一页请求在途：超时（0.6s）未到达则重发一次。抽文本可能因页复杂而慢，
    // 也可能失败（损坏页），超时重发让"失败"不至于变成"永远没有 I 型光标"。
    if (g_app.content_since >= 0.0 && (now - g_app.content_since) > 0.6) {
        g_app.renderer->request_page_content(want);
        g_app.content_since = now;
    }
}

void update_clipboard_results() {
    if (g_app.renderer == nullptr) return;

    std::string text;
    if (g_app.renderer->take_copy_text(text)) {
        if (text.empty()) show_toast("没有可复制的文本");
        else if (set_clipboard_text(text)) show_toast("已复制文本");
        else show_toast("复制失败：剪贴板被占用");
    }

    lr::ImageData img;
    if (g_app.renderer->take_copy_image(img)) {
        if (!img.valid()) {
            show_toast("此处没有可复制的图片");
        } else if (set_clipboard_image_rgba(img.w, img.h, img.rgba.data())) {
            show_toast("已复制图片 " + std::to_string(img.w) + "×" + std::to_string(img.h));
        } else {
            show_toast("复制失败：剪贴板被占用");
        }
    }
}

}  // namespace lr::app
