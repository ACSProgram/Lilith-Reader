// ui.cpp — Lilith Reader 应用层：全部绘制
//
// 职责：把会话状态（session.cpp）与偏好（platform.cpp）表现为 ImGui 界面。分五块：
//   · 外壳：顶栏 + 画布（可选侧栏）+ 状态栏三段（draw_shell 为帧入口）
//   · 阅读视图：画布区域、页占位/失败重试、滚动指示条、右键菜单
//   · 侧栏：目录 / 书签 / 缩略图
//   · 弹窗：跳页、密码、设置、帮助、调试浮层
//   · 引导页：拖放引导 / 打开中 / 失败 / 被拒绝
//
// 设计原则（ADR-045）：**命令集中、按钮克制**——可点控件只出现在顶栏一簇与各种菜单里；
// 状态栏只放只读信息；视图类命令都是带勾选的语义项。本文件不直接触碰渲染层，只经
// session 层暴露的操作（open/close/rotate/...）；只读快照（slot/thumb_slot）例外。
//
// 依赖：platform（px/主题/偏好）+ session（操作与状态）；共享声明见 app_internal.h。

#include "app_internal.h"

namespace lr::app {

// ---------------- 本文件专用常量 ----------------

constexpr const char* kFormatsLine =
    "支持 PDF · EPUB · MOBI · FB2 · CBZ · XPS · 图片(PNG/JPG/GIF/BMP/TIFF)";

constexpr float kSidebarWidthPx = 300.0f;   // 基准像素，用前过 px()
constexpr int   kThumbTargetPx = 150;       // 缩略图最长边目标像素

// ---- 自绘滚动条（画布右侧；可拖拽，ADR-050）----
// 命中区刻意比可见滑块宽：滑块视觉上要细（4~7px），但 4px 宽的目标很难按中。
constexpr float kScrollBarHitW   = 12.0f;   // 命中区宽度（基准像素）
constexpr float kScrollBarVisW   = 5.0f;    // 可见滑块宽度
constexpr float kScrollBarVisWHover = 7.0f; // 悬停/拖拽时加宽（给"可拖"以即时反馈）
constexpr float kScrollBarInsetY = 10.0f;   // 轨道上下内缩
constexpr float kScrollBarMinThumb = 30.0f; // 滑块最小长度

constexpr double kToolbarHideDelaySec = 1.6;   // 鼠标离开顶部区域多久后收起顶栏
constexpr float  kToolbarRevealBandPx = 4.0f;  // 距窗口顶端多少像素内即判定"要显示"

constexpr float  kPageFadeSec = 0.18f;         // 页面首次出现淡入时长
constexpr double kScrollIndHoldSec = 1.2;      // 滚动指示条静止后渐隐的等待

// ---------------- 引导 / 状态页 ----------------

// 引导页文字颜色**必须随主题**：早期写死浅色，浅色主题下几乎看不见（已修）。
// 还要**随纸张方案**（tone_apply）：状态页画在画布底色上，不跟着走就会在暖色/深色下
// 留下一块纯中性灰的字（ADR-068 统一走同一处派生）。
ImVec4 col_text()   { return tone_apply(g_dark_theme ? ImVec4(0.90f, 0.91f, 0.93f, 1.0f)
                                                     : ImVec4(0.15f, 0.17f, 0.20f, 1.0f)); }
ImVec4 col_dim()    { return tone_apply(g_dark_theme ? ImVec4(0.58f, 0.61f, 0.65f, 1.0f)
                                                     : ImVec4(0.42f, 0.46f, 0.51f, 1.0f)); }
ImVec4 col_warn()   { return tone_apply(g_dark_theme ? ImVec4(0.95f, 0.72f, 0.42f, 1.0f)
                                                     : ImVec4(0.72f, 0.35f, 0.10f, 1.0f)); }

// 居中排版要有一个**整帧稳定**的参考框：逐次读取 GetContentRegionAvail 会随文本放置而漂移。
struct CenterArea { ImVec2 origin; ImVec2 avail; };
CenterArea center_area() {
    return { ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail() };
}

void centered_text(const CenterArea& a, const char* text, float dy, const ImVec4& col,
                   float font_base = 0.0f) {
    if (font_base > 0.0f) ImGui::PushFont(nullptr, font_base);
    const ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::SetCursorScreenPos(ImVec2(a.origin.x + (a.avail.x - ts.x) * 0.5f,
                                     a.origin.y + (a.avail.y - ts.y) * 0.5f + dy * ui_scale()));
    ImGui::TextColored(col, "%s", text);
    if (font_base > 0.0f) ImGui::PopFont();
}

void draw_drop_guide() {
    const CenterArea a = center_area();
    centered_text(a, "Lilith Reader", -96.0f, col_text(), kUiFontBasePx * 2.1f);
    centered_text(a, "拖入文档即可开始阅读", -34.0f, col_dim());
    centered_text(a, kFormatsLine, 6.0f, col_dim());
    centered_text(a, "顶部菜单 / Ctrl+O 打开文件 · 在页面上右键进入全部命令 · F1 查看快捷键", 44.0f, col_dim());
    centered_text(a, "按 Esc 退出", 88.0f, col_dim());
}

void draw_opening() {
    const CenterArea a = center_area();
    centered_text(a, g_doc.name_u8.c_str(), -44.0f, col_text());
    static const char* kDots[] = { "正在打开 ．", "正在打开 ．．", "正在打开 ．．．" };
    const int frame = static_cast<int>(ImGui::GetTime() * 3.0) % 3;
    centered_text(a, kDots[frame], 0.0f, col_dim());
    centered_text(a, "按 Esc 取消", 44.0f, col_dim());
}

void draw_failed() {
    const CenterArea a = center_area();
    centered_text(a, lr::describe(g_doc.error).data(), -84.0f, col_warn());
    centered_text(a, g_doc.name_u8.c_str(), -42.0f, col_text());

    if (g_doc.error == lr::DocError::Unsupported) {
        centered_text(a, kFormatsLine, 0.0f, col_dim());
    } else if (g_doc.error == lr::DocError::Mismatched) {
        centered_text(a, "实际内容是一个压缩包（zip/tar）", 0.0f, col_dim());
        centered_text(a, "若是图片集，请把扩展名改回 .cbz；否则请先解压", 36.0f, col_dim());
    } else if (!g_doc.detail_u8.empty()) {
        std::string detail = g_doc.detail_u8;
        if (detail.size() > 160) detail = detail.substr(0, 160) + "…";
        centered_text(a, detail.c_str(), 0.0f, col_dim());
    }
    centered_text(a, "按 Esc 返回", 84.0f, col_dim());
}

void draw_rejected() {
    const CenterArea a = center_area();
    if (g_doc.error == lr::DocError::NotFound) {
        centered_text(a, lr::describe(g_doc.error).data(), -42.0f, col_text());
        centered_text(a, lr::wide_to_utf8(g_doc.path_w).c_str(), 0.0f, col_dim());
    } else {  // Unsupported
        centered_text(a, lr::describe(g_doc.error).data(), -84.0f, col_text());
        centered_text(a, g_doc.name_u8.c_str(), -42.0f, col_dim());
        centered_text(a, kFormatsLine, 0.0f, col_dim());
    }
    centered_text(a, "按 Esc 返回", 44.0f, col_dim());
}

// ---------------- 画布绘制 ----------------

// ---- 自绘滚动条（画布子窗口是 NoScrollbar，滚动反馈自己画）----
//
// 几何只在 scroll_bar_geom() 里算一次，**绘制与命中测试共用同一份** —— 早期版本只画一条
// 装饰性指示条、没有命中区，于是"拖右侧滚动条"实际落到了画布的左键拖拽平移上（实测反馈）。
struct ScrollBarGeom {
    bool   active = false;
    ImVec2 track_min{}, track_max{};   // 命中区（比可见滑块宽）
    ImVec2 thumb_min{}, thumb_max{};   // 可见滑块
    float  travel = 0.0f;              // 滑块可移动距离 = 轨道长 − 滑块长
};

ScrollBarGeom scroll_bar_geom(const ImVec2& origin, const ImVec2& size) {
    ScrollBarGeom g;
    const float max_sy = g_canvas.max_scroll_y();
    if (max_sy <= 1.0f) return g;      // 内容不高于视口：没有滚动条
    const float top = origin.y + px(kScrollBarInsetY);
    const float bot = origin.y + size.y - px(kScrollBarInsetY);
    const float track_len = std::max(1.0f, bot - top);
    const float content = std::max(1.0f, g_canvas.content_height_px());
    const float frac = std::clamp(size.y / content, 0.06f, 1.0f);
    const float thumb_len = std::min(track_len, std::max(px(kScrollBarMinThumb), track_len * frac));
    const float t = std::clamp(g_canvas.state().scroll_y / max_sy, 0.0f, 1.0f);
    const float thumb_top = top + t * (track_len - thumb_len);
    const float vis_w = (g_scroll_hover || g_scroll_drag) ? px(kScrollBarVisWHover)
                                                          : px(kScrollBarVisW);
    const float x_right = origin.x + size.x - px(4.0f);
    g.active = true;
    g.track_min = ImVec2(x_right - px(kScrollBarHitW), top);
    g.track_max = ImVec2(x_right, bot);
    g.thumb_min = ImVec2(x_right - vis_w, thumb_top);
    g.thumb_max = ImVec2(x_right, thumb_top + thumb_len);
    g.travel = std::max(1.0f, track_len - thumb_len);
    return g;
}

// 滚动条交互。返回 true = 本次左键输入已被滚动条消费（调用方跳过画布的命中测试）。
bool update_scroll_bar(const ScrollBarGeom& g) {
    if (!g.active) {
        g_scroll_drag = false;
        g_scroll_hover = false;
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mp = io.MousePos;
    const float slop = px(4.0f);

    if (g_scroll_drag) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_scroll_drag = false;
            return true;
        }
        // 拖拽中即使鼠标移出轨道也继续跟随（与系统滚动条一致）
        const float top = std::clamp(mp.y - g_scroll_drag_off, g.track_min.y,
                                     g.track_min.y + g.travel);
        const float t = (top - g.track_min.y) / g.travel;
        g_canvas.scroll_by(0.0f, t * g_canvas.max_scroll_y() - g_canvas.state().scroll_y);
        g_scroll_pending = 0.0f;   // 直接定位：掐掉平滑尾巴，避免"松手后还在飘"
        g_jump_repin_page = -1;
        g_scroll_hover = true;
        return true;
    }

    const bool over = mp.x >= g.track_min.x - slop && mp.x <= g.track_max.x + slop &&
                      mp.y >= g.track_min.y - slop && mp.y <= g.track_max.y + slop;
    g_scroll_hover = over;
    if (!over || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return false;

    const float thumb_len = g.thumb_max.y - g.thumb_min.y;
    if (mp.y >= g.thumb_min.y && mp.y <= g.thumb_max.y) {
        g_scroll_drag_off = mp.y - g.thumb_min.y;      // 按在滑块上：保持抓取点
    } else {
        g_scroll_drag_off = thumb_len * 0.5f;          // 按在空白处：滑块中心跳到该处
        const float top = std::clamp(mp.y - g_scroll_drag_off, g.track_min.y,
                                     g.track_min.y + g.travel);
        const float t = (top - g.track_min.y) / g.travel;
        g_canvas.scroll_by(0.0f, t * g_canvas.max_scroll_y() - g_canvas.state().scroll_y);
    }
    g_scroll_pending = 0.0f;
    g_jump_repin_page = -1;
    g_scroll_drag = true;
    return true;
}

void draw_scroll_bar(ImDrawList* dl, const ScrollBarGeom& g) {
    if (!g.active) return;
    // 浮现/渐隐：滚动中、悬停、拖拽时全显，静止一段时间后渐隐（Motion 关闭则直切）
    const bool hot = g_scroll_hover || g_scroll_drag ||
                     (g_last_scroll_time > 0.0 &&
                      (ImGui::GetTime() - g_last_scroll_time) < kScrollIndHoldSec);
    const float target = hot ? 1.0f : 0.0f;
    g_scroll_ind_alpha = g_prefs.motion ? approach(g_scroll_ind_alpha, target, 12.0f,
                                                   ImGui::GetIO().DeltaTime)
                                        : target;
    if (g_scroll_ind_alpha < 0.02f) return;
    const float peak = (g_scroll_hover || g_scroll_drag) ? 235.0f : 170.0f;
    const ImU32 col = (g_pal.accent & 0x00FFFFFFu) |
                      (static_cast<ImU32>(g_scroll_ind_alpha * peak) << 24);
    const float r = (g.thumb_max.x - g.thumb_min.x) * 0.5f;
    dl->AddRectFilled(g.thumb_min, g.thumb_max, col, r);
}

void draw_page_placeholder(ImDrawList* dl, const ImVec2& pmin, const ImVec2& pmax,
                           const lr::PageSlot& s) {
    const bool failed = (s.status == lr::PageStatus::Failed);
    dl->AddRectFilled(pmin, pmax, failed ? g_pal.failed : g_pal.placeholder);
    dl->AddRect(pmin, pmax, failed ? g_pal.failed_border : g_pal.placeholder_border);
    // 失败占位提示"点击重试"（ADR-031）：命中测试在 draw_canvas_area 里做
    const char* txt = failed ? "渲染失败 · 点击重试"
                             : (s.status == lr::PageStatus::Loading ? "载入中…" : "");
    if (txt[0] != '\0') {
        const ImVec2 ts = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2((pmin.x + pmax.x - ts.x) * 0.5f, (pmin.y + pmax.y - ts.y) * 0.5f),
                    g_pal.placeholder_text, txt);
    }
}

// ---------------- 页内叠加层：选区 / 搜索命中 / 链接悬停（Phase 8） ----------------

namespace {

// 换掉颜色的 alpha（保留 RGB）
inline ImU32 with_alpha(ImU32 c, float a) {
    const float cl = a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a);
    return (c & 0x00FFFFFFu) | (static_cast<ImU32>(std::lround(cl * 255.0f)) << 24);
}

// 把一个"未旋转页面 pt 矩形"画到屏幕上。
// 90° 的整数倍旋转把轴对齐矩形映成轴对齐矩形，故只需换算**两个对角点**再归一化 ——
// 不必逐角换算再拼多边形。
void draw_pt_rect(ImDrawList* dl, const PageView& view, const PageGeom& geom, int rot,
                  float x0, float y0, float x1, float y1, ImU32 col) {
    float ax = 0, ay = 0, bx = 0, by = 0;
    if (!page_pt_to_screen(view, geom, rot, x0, y0, ax, ay)) return;
    if (!page_pt_to_screen(view, geom, rot, x1, y1, bx, by)) return;
    dl->AddRectFilled(ImVec2(std::min(ax, bx), std::min(ay, by)),
                      ImVec2(std::max(ax, bx), std::max(ay, by)), col);
}

}  // namespace

void draw_page_overlays(ImDrawList* dl, const ImVec2& origin, int page) {
    PageView view;
    PageGeom geom;
    if (!page_view_of(origin, page, view, geom)) return;

    // 1) 搜索命中：先画普通命中，**当前命中最后画**（叠在最上层，颜色也更重），
    //    否则当前命中会被后画的普通命中盖住一角。
    const int hn = static_cast<int>(g_search_hits.size());
    if (hn > 0) {
        const ImU32 normal = with_alpha(g_pal.accent, kFindAlpha);
        const ImU32 current = with_alpha(g_pal.accent, 0.55f);
        for (int i = 0; i < hn; ++i) {
            if (i == g_search_cur) continue;
            const lr::SearchHit& h = g_search_hits[static_cast<std::size_t>(i)];
            if (h.page != page) continue;
            draw_pt_rect(dl, view, geom, g_rotation, h.x0, h.y0, h.x1, h.y1, normal);
        }
        if (g_search_cur >= 0 && g_search_cur < hn) {
            const lr::SearchHit& h = g_search_hits[static_cast<std::size_t>(g_search_cur)];
            if (h.page == page)
                draw_pt_rect(dl, view, geom, g_rotation, h.x0, h.y0, h.x1, h.y1, current);
        }
    }

    // 2) 选区
    if (g_sel.active && g_sel.page == page) {
        const ImU32 col = with_alpha(g_pal.accent, kSelectAlpha);
        for (const SelRect& r : g_sel.rects)
            draw_pt_rect(dl, view, geom, g_rotation, r.x0, r.y0, r.x1, r.y1, col);
    }

    // 3) 链接悬停：淡色底（点击的落点提示，与手型光标互为印证）
    if (g_hover_link >= 0 && g_hover_page == page && g_content_page == page &&
        g_hover_link < static_cast<int>(g_content.links.size())) {
        const lr::PageLink& l = g_content.links[static_cast<std::size_t>(g_hover_link)];
        draw_pt_rect(dl, view, geom, g_rotation, l.x0, l.y0, l.x1, l.y1,
                     with_alpha(g_pal.accent, 0.16f));
    }
}

// 画布区：高度由 draw_shell 显式给出（不依赖 ImGui 的相邻项间距，见 draw_shell 注释）
void draw_canvas_area(float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##canvas", ImVec2(0, height), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();
    const float dt = ImGui::GetIO().DeltaTime;

    // DPI/界面缩放变化时同步画布留白（只在不一致时下发：set_margin_gap 会让布局缓存失效，
    // 每帧无条件调用会毁掉"滚动不重算布局"的 O(1) 性质）。间距是列宽比例，与 DPI 无关。
    if (g_canvas_scale != ui_scale()) {
        g_canvas.set_margin_gap(px(kCanvasMarginPx), gap_ratio_pref());
        g_canvas_scale = ui_scale();
    }
    g_canvas.set_viewport(size.x, size.y);

    // 首帧视口就绪后恢复阅读位置（fit-width 派生 zoom 依赖视口尺寸，打开时视口还是 0）
    if (g_restore_pending) {
        g_restore_pending = false;
        g_canvas.scroll_to_page(g_restore_page, 0.0f);
        g_prev_scroll_y = g_canvas.state().scroll_y;  // 避免首帧被误判为滚动
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), g_pal.backdrop);

    const bool hovered = ImGui::IsWindowHovered();
    g_canvas_hovered = hovered;
    g_canvas_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    // 右键按下/抬起计数（F3 诊断用）：排查"右键菜单没反应"时，
    // 一眼区分是输入根本没到、还是被别的条件挡住（配合 anyItem/hover 读数）。
    if (ImGui::GetIO().MouseClicked[ImGuiMouseButton_Right]) ++g_dbg_r_down;
    if (ImGui::GetIO().MouseReleased[ImGuiMouseButton_Right]) ++g_dbg_r_up;

    // 滚动条交互先于画布输入（ADR-050）：它一旦消费左键，画布就不再把这串输入当成平移/点击。
    const ScrollBarGeom bar_hit = scroll_bar_geom(origin, size);
    const bool bar_consumed = update_scroll_bar(bar_hit);

    // 失败占位点击重试（ADR-031）：在输入处理**之前**做命中测试，只针对
    // "Failed 且尚无纹理"的页（曾成功渲染过、因重渲染失败而保留旧图的页不显示占位，
    // 也无从点击）。单击不会触发拖拽平移（平移需要移动阈值），故两者不冲突。
    if (!bar_consumed && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mp = ImGui::GetIO().MousePos;
        const int cnt = g_canvas.page_count();
        const int vf = g_canvas.visible_first();
        const int vl = g_canvas.visible_last();
        for (int i = vf; i <= vl && i < cnt; ++i) {
            const lr::PageSlot s = g_renderer->slot(i);
            if (s.status != lr::PageStatus::Failed || s.texture != nullptr) continue;
            const lr::PageRect r = g_canvas.page_rect(i);
            const float x0 = origin.x + r.x, y0 = origin.y + r.y;
            if (mp.x >= x0 && mp.x <= x0 + r.w && mp.y >= y0 && mp.y <= y0 + r.h) {
                g_renderer->retry_page(i);
                break;
            }
        }
    }

    // 页内容快照（文本布局 / 图片矩形 / 链接）：接收上一帧请求的结果，并按鼠标位置发起新请求。
    // **必须在 handle_canvas_input 之前** —— 文本命中测试与光标形状都依赖它。
    update_hovered_content(origin, hovered);

    // 设置窗口/弹窗打开时画布不再吞键盘（否则方向键会同时翻页与移动焦点）。
    if (!g_open_jump && !g_open_password && !g_show_settings)
        handle_canvas_input(origin, size, hovered);

    // 视图动效（ADR-047）：在输入之后、取页与布局之前推进 —— 本帧绘制的就是插值后的状态。
    step_view_motion(dt);

    // 滚动方向（供方向感知预加载）：以内容坐标 scroll_y 的变化判定。
    // 阈值 0.5px 抑制浮点抖动导致的假翻转。
    {
        const float sy = g_canvas.state().scroll_y;
        if (sy > g_prev_scroll_y + 0.5f) g_scroll_dir = +1;
        else if (sy < g_prev_scroll_y - 0.5f) g_scroll_dir = -1;
        g_prev_scroll_y = sy;
        // 滚动条淡出计时：只在真正滚动时刷新"最近滚动时刻"
        if (std::fabs(sy - g_scroll_ind_last_y) > 0.5f) {
            g_scroll_ind_last_y = sy;
            g_last_scroll_time = ImGui::GetTime();
        }
    }

    update_want_scale();
    emit_wants();

    const int n = g_canvas.page_count();
    const int first = g_canvas.visible_first();
    const int last = g_canvas.visible_last();
    const ImVec2 clip_max(origin.x + size.x, origin.y + size.y);
    if (g_page_fade.size() < static_cast<std::size_t>(n)) g_page_fade.resize(n);
    dl->PushClipRect(origin, clip_max, true);
    for (int i = first; i <= last && i < n; ++i) {
        const lr::PageRect r = g_canvas.page_rect(i);
        const ImVec2 pmin(origin.x + r.x, origin.y + r.y);
        const ImVec2 pmax(pmin.x + r.w, pmin.y + r.h);
        if (pmax.x < origin.x || pmin.x > clip_max.x ||
            pmax.y < origin.y || pmin.y > clip_max.y)
            continue;
        const lr::PageSlot s = g_renderer->slot(i);
        if (s.texture != nullptr || !s.tiles.empty()) {
            // 页面投影：右下偏移的半透明矩形，给纸面一点立体感
            const float sh = px(3.0f);
            dl->AddRectFilled(ImVec2(pmin.x + sh, pmin.y + sh),
                              ImVec2(pmax.x + sh, pmax.y + sh), g_pal.shadow, px(2.0f));
            // 淡入：**只在"纹理在眼前就绪"时渐显**（占位 → 内容的换入），
            // 页面预加载好之后才滑入视野的不淡入（直接显示）—— 否则每次翻页都从透明渐显，
            // 看起来就像"翻到哪才开始加载"（人工反馈的"换页轻微闪烁"）。
            PageFade& pf = g_page_fade[static_cast<std::size_t>(i)];
            if (g_prefs.motion) {
                if (!pf.seen) { pf.seen = true; pf.alpha = pf.waiting ? 0.0f : 1.0f; }
                if (pf.alpha < 1.0f)
                    pf.alpha = std::min(1.0f, pf.alpha + dt / kPageFadeSec);
                pf.waiting = false;
            } else {
                pf.alpha = 1.0f;
                pf.waiting = false;
            }
            const int a = static_cast<int>(std::lround(pf.alpha * 255.0f));
            if (!s.tiles.empty() && s.full_pixel_w > 0 && s.full_pixel_h > 0) {
                for (const lr::PageSlot::Tile& tile : s.tiles) {
                    const float tx0 = pmin.x + r.w * (static_cast<float>(tile.x) / s.full_pixel_w);
                    const float ty0 = pmin.y + r.h * (static_cast<float>(tile.y) / s.full_pixel_h);
                    const float tx1 = pmin.x + r.w * (static_cast<float>(tile.x + tile.w) / s.full_pixel_w);
                    const float ty1 = pmin.y + r.h * (static_cast<float>(tile.y + tile.h) / s.full_pixel_h);
                    dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(tile.texture)),
                                 ImVec2(tx0, ty0), ImVec2(tx1, ty1), ImVec2(0, 0), ImVec2(1, 1),
                                 IM_COL32(255, 255, 255, a));
                }
            } else {
                dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(s.texture)),
                             pmin, pmax, ImVec2(0, 0), ImVec2(1, 1),
                             IM_COL32(255, 255, 255, a));
            }
            dl->AddRect(pmin, pmax, g_pal.page_border);
        } else {
            // 无纹理：本页正在"眼前等纹理"（waiting）→ 纹理到达时才值得淡入。
            g_page_fade[static_cast<std::size_t>(i)].seen = false;
            g_page_fade[static_cast<std::size_t>(i)].waiting = true;
            g_page_fade[static_cast<std::size_t>(i)].alpha = 1.0f;
            draw_page_placeholder(dl, pmin, pmax, s);
        }
        // 页内叠加层（选区 / 搜索命中 / 链接悬停）。画在**占位框之上**：
        // 纹理还没到位时选区照样看得见，语义上更一致（选区属于"页"而不是"纹理"）。
        draw_page_overlays(dl, origin, i);
    }
    dl->PopClipRect();

    // 滚动条用**交互后**的几何重算一次：拖动刚改过 scroll_y，滑块位置必须同帧跟上
    draw_scroll_bar(dl, scroll_bar_geom(origin, size));
    draw_canvas_context_menu();  // 命中区是画布，命令集中在上下文菜单里

    ImGui::EndChild();
}

void draw_status_bar() {
    const float bar_h = px(kStatusBarH);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(kChromePadX), px(2)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(7), 0));
    ImGui::BeginChild("##status", ImVec2(0, bar_h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), g_pal.chrome);
    dl->AddLine(ImVec2(wp.x, wp.y + 0.5f), ImVec2(wp.x + ws.x, wp.y + 0.5f),
                g_pal.chrome_border);

    const int total = g_canvas.page_count();
    // 页码用画布游标（到底时即末行首页），而不是 visible_first()：后者在
    // 视口高于一行时会停在末行前一行，页码会与所见不符（详见 canvas.ixx）。
    const int cur = std::min(total, std::max(1, g_canvas.current_page() + 1));

    // 状态栏只承载**只读信息**（可点的控件全部集中在顶栏与菜单，避免状态栏变成按钮堆）。
    const float ty = (bar_h - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SetCursorPos(ImVec2(px(kChromePadX), ty));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_pal.chrome_text),
                       "第 %d / %d 页", cur, std::max(1, total));

    // 非默认视图状态以 chip 形式跟在后面，默认态不占地方（也减少视觉噪音）。
    auto chip = [](const char* fmt, ...) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim), "·");
        ImGui::SameLine();
        va_list ap; va_start(ap, fmt);
        char buf[96];
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim), "%s", buf);
    };
    if (g_canvas.state().spread) chip("对开");
    else if (g_canvas.state().columns > 1) chip("%d 列", g_canvas.state().columns);
    if (g_rotation != 0) chip("旋转 %d°", g_rotation);
    if (g_scheme == 1) chip("深色");
    else if (g_scheme == 2) chip("暖色");
    if (current_page_has_bookmark()) chip("已加书签");

    // 操作提示（Phase 8）：居中显示"已复制 / 此处没有图片"这类一次性反馈。
    // 居中而不是挤在左侧：左侧已承载页码与状态 chip，右侧是格式信息，中间正好空着。
    // 用状态栏而不是弹窗：复制是高频小动作，弹窗会打断阅读。
    {
        const std::string toast = toast_text();
        if (!toast.empty()) {
            const float tw = ImGui::CalcTextSize(toast.c_str()).x;
            ImGui::SetCursorPos(ImVec2((ws.x - tw) * 0.5f, ty));
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_pal.accent), "%s", toast.c_str());
        }
    }

    // 右侧：格式（扩展名与内容不符时把提示也放这里，不挤占顶栏）。
    std::string right;
    if (!g_doc.info.format.empty()) {
        right = g_doc.info.format;
        if (!lr::format_matches_extension(g_doc.info.format, g_doc.ext_u8))
            right += "（扩展名 " + g_doc.ext_u8 + " 不符）";
    }
    if (!right.empty()) {
        const float rw = ImGui::CalcTextSize(right.c_str()).x;
        ImGui::SetCursorPos(ImVec2(ws.x - rw - px(kChromePadX), ty));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim), "%s", right.c_str());
    }

    ImGui::EndChild();
}

// ---------------- 调试浮层 ----------------

void draw_debug_overlay() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(px(12), px(12)), ImGuiCond_Always);
    if (ImGui::Begin("##debug", nullptr, ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (io.Framerate > 0.0f)
            ImGui::Text("%.1f FPS / %.2f ms", io.Framerate, 1000.0f / io.Framerate);
        else
            ImGui::TextUnformatted("-- FPS");
        ImGui::TextDisabled("%dx%d  dpi %.0f%%  ui %.0f%%  font %.0fpx",
                            (int)io.DisplaySize.x, (int)io.DisplaySize.y,
                            (double)(g_dpi_scale * 100.0f), (double)(g_user_scale * 100.0f),
                            (double)ImGui::GetFontSize());

        static const char* kKindNames[] = { "none", "rejected", "opening", "reading", "failed",
                                            "needs-password" };
        ImGui::Separator();
        ImGui::Text("doc: %s", kKindNames[static_cast<int>(g_doc.kind)]);
        ImGui::TextDisabled("err: %.*s", (int)lr::to_string(g_doc.error).size(),
                            lr::to_string(g_doc.error).data());
        if (g_doc.kind == UiDoc::Kind::Reading) {
            ImGui::Text("pages: %d / %.0fx%.0f pt", g_doc.info.page_count,
                        (double)g_doc.info.page_width_pt, (double)g_doc.info.page_height_pt);
            ImGui::TextDisabled("fmt: %s / ext: %s",
                                g_doc.info.format.empty() ? "?" : g_doc.info.format.c_str(),
                                g_doc.ext_u8.c_str());
            ImGui::Separator();
            ImGui::Text("zoom: %.3f (want %.3f)", g_canvas.effective_zoom(), g_want_scale);
            ImGui::Text("vis: %d..%d  rows: %d", g_canvas.visible_first(),
                        g_canvas.visible_last(), g_canvas.rows());
            ImGui::Text("cur: page %d  row %d", g_canvas.current_page(),
                        g_canvas.current_row());
            ImGui::Text("scroll: %.0f, %.0f / %.0f, %.0f", g_canvas.state().scroll_x,
                        g_canvas.state().scroll_y, g_canvas.max_scroll_x(),
                        g_canvas.max_scroll_y());
            ImGui::Text("cols: %d  fit: %d", g_canvas.state().columns,
                        g_canvas.state().fit_width ? 1 : 0);
            ImGui::Text("rot: %d  color: %d  spread: %d",
                        g_rotation, g_scheme, g_canvas.state().spread ? 1 : 0);
            const lr::DocRecord* rec = g_state.find(g_doc_key);
            ImGui::TextDisabled("outline: %d  bookmarks: %d  sidebar: %d tab %d",
                                static_cast<int>(g_outline.size()),
                                rec ? static_cast<int>(rec->bookmarks.size()) : 0,
                                g_show_sidebar ? 1 : 0, g_sidebar_tab);
            // 缓存统计：驻留字节/预算、驻留页数、累计逐出页数
            const lr::CacheStats cs = g_renderer->cache_stats();
            ImGui::Text("cache: %.1f / %.0f MB  pages %d  evict %d",
                        (double)cs.used_bytes / 1048576.0,
                        (double)cs.budget_bytes / 1048576.0,
                        cs.resident_pages, cs.evictions);
            ImGui::TextDisabled("preload dir %d  (+1 下 / -1 上 / 0 两侧)",
                                g_scroll_dir);
            // 动效读数（ADR-047）：验证"动效是否真的在跑"时看这里，不必靠肉眼猜
            ImGui::TextDisabled("motion: pending %.1f  zoom->%.3f %s  topbar %.0f/%.0f  bar drag %d",
                                (double)g_scroll_pending, (double)view_zoom_target(),
                                g_zoom_anim ? "anim" : "idle", (double)g_top_bar_h,
                                (double)px(kTopBarH), g_scroll_drag ? 1 : 0);
            // 快捷键**不**看这两个量（ADR-026），列出仅为排查"某个键没反应"时定位用
            ImGui::TextDisabled("canvas hover %d  focus %d  text-input %d  r-click down/up %d/%d",
                                g_canvas_hovered ? 1 : 0, g_canvas_focused ? 1 : 0,
                                ImGui::GetIO().WantTextInput ? 1 : 0, g_dbg_r_down, g_dbg_r_up);
            // 文本交互读数（Phase 8）：排查"选不中 / 光标不变 / 复制没反应"时先看这里 ——
            // hover_char 为 -1 说明内容快照还没到（content 行能看到是哪一页），
            // 而不是命中测试算错了。
            ImGui::TextDisabled("hover: page %d link %d char %d", g_hover_page, g_hover_link,
                                g_hover_char);
            ImGui::TextDisabled("content: page %d (want %d)  chars %d lines %d links %d img %d",
                                g_content_page, g_content_want,
                                static_cast<int>(g_content.chars.size()),
                                static_cast<int>(g_content.lines.size()),
                                static_cast<int>(g_content.links.size()),
                                static_cast<int>(g_content.images.size()));
            ImGui::TextDisabled("sel: %s page %d anchor %d head %d rects %d",
                                g_sel.active ? "on" : "off", g_sel.page, g_sel.anchor, g_sel.head,
                                static_cast<int>(g_sel.rects.size()));
            ImGui::TextDisabled("find: active %d %d/%d  hits %d  cur %d  trunc %d  pend %d",
                                g_search_active ? 1 : 0, g_search_scanned, g_search_total,
                                static_cast<int>(g_search_hits.size()), g_search_cur,
                                g_search_truncated ? 1 : 0, g_search_pending ? 1 : 0);
        }
        if (g_doc.kind == UiDoc::Kind::Failed && !g_doc.detail_u8.empty())
            ImGui::TextDisabled("last_error: %.120s", g_doc.detail_u8.c_str());
    }
    ImGui::End();
}

// ---------------- 弹窗开合动效（ADR-059）----------------
//
// 与设置窗口同一"开合对称"的语感，但**不做缩放**：这两个弹窗是 AlwaysAutoResize 的输入框，
// 缩小外框会让内部控件（页码输入框等）被窗口裁剪 —— 观感损失大于收益。改用
// **淡入淡出 + 轻微滑落**（未收敛时整体下移 kPopupSlidePx，收敛到位）。
// ImGui 的 popup 关闭是"立即销毁"，故用**延迟关闭**：逻辑关闭只把目标进度降到 0，
// 等动画收敛后才真正 CloseCurrentPopup —— 否则关闭路径根本没有动画可言。
ToggleAnim g_jump_anim;
ToggleAnim g_pwd_anim;

// 弹窗统一的开合样式：以视口中心为不动点（pivot 0.5）做滑落，外加整体透明度。
// 用 pivot 而不是自己算左上角，是因为弹窗是自动尺寸、开合首帧还不知道它多大。
//
// **透明度必须留一个下限（>0）**：ImGui 的 `Begin()` 里有一条
//   `if (style.Alpha <= 0.0f) window->HiddenFramesCanSkipItems = 1;`
// 于是收敛帧（动画值恰好为 0）弹窗会被判为 Hidden，`Begin()` 返回 false ——
// 而"收敛后再 CloseCurrentPopup"恰恰写在 `if (BeginPopupModal(...))` 为真的分支里，
// 结果就是**弹窗永远关不掉**：它已经看不见，却作为模态留在 OpenPopupStack 里，
// 把整窗输入全部挡住（实测现象："点完按钮整页卡死，什么都不能操作"，ADR-064）。
// 留 1/255 的透明度肉眼不可见，却让收敛帧仍是一个"真实帧"，关闭逻辑得以执行。
constexpr float kPopupMinAlpha = 1.0f / 255.0f;
void popup_anim_apply(const ToggleAnim& a) {
    const float alpha = a.value < kPopupMinAlpha ? kPopupMinAlpha : a.value;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
               vp->Pos.y + vp->Size.y * 0.5f + (1.0f - a.value) * px(kPopupSlidePx)),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
}

// ---------------- 弹窗：跳页 ----------------

void draw_jump_popup() {
    const bool alive = g_jump_anim.step(ImGui::GetIO().DeltaTime, g_open_jump, g_prefs.motion);
    const bool is_open = ImGui::IsPopupOpen("跳转页码");
    if (!is_open && !g_open_jump) return;   // 无弹窗、也无打开请求：不参与
    if (!is_open) ImGui::OpenPopup("跳转页码");
    // **始终**套用动画样式：收敛那一帧 value 已是 0，必须仍为全透明再销毁，
    // 否则会以默认不透明度多画一帧 —— 即"关闭时闪一下"。
    popup_anim_apply(g_jump_anim);
    if (ImGui::BeginPopupModal("跳转页码", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int total = std::max(1, g_canvas.page_count());
        ImGui::Text("页码 (1 - %d)", total);
        ImGui::SetNextItemWidth(px(140));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::BeginDisabled(!g_open_jump);   // 淡出中不再响应，避免重复触发
        const bool enter = ImGui::InputInt("##page", &g_jump_page, 0, 0,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        const bool do_jump = enter || ImGui::Button("跳转");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("取消");
        ImGui::EndDisabled();
        if (do_jump) {
            int p = g_jump_page - 1;
            if (p < 0) p = 0;
            if (p >= g_canvas.page_count()) p = g_canvas.page_count() - 1;
            request_jump_scroll(p, 0.0f);
            g_open_jump = false;   // 只置逻辑关：动画收敛后再真正 CloseCurrentPopup
        } else if (cancel) {
            g_open_jump = false;
        }
        if (!alive) ImGui::CloseCurrentPopup();   // 动画收敛：本帧已全透明，安全销毁
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---------------- 侧栏：目录 / 书签 / 缩略图 ----------------

void draw_outline_tab() {
    if (g_outline.empty()) { ImGui::TextDisabled("本文档没有目录"); return; }
    ImGui::BeginChild("##outline_list", ImVec2(0, 0), false);
    const int cur = g_canvas.current_page();
    for (int i = 0; i < static_cast<int>(g_outline.size()); ++i) {
        const lr::OutlineItem& it = g_outline[i];
        const char* label = it.title.empty() ? "(无标题)" : it.title.c_str();
        ImGui::PushID(i);
        if (it.depth > 0) ImGui::Indent(px(14.0f) * static_cast<float>(it.depth));
        const bool selected = (it.page >= 0 && it.page == cur);
        if (ImGui::Selectable(label, selected) && it.page >= 0)
            request_jump_scroll(it.page, 0.0f);
        if (it.depth > 0) ImGui::Unindent(px(14.0f) * static_cast<float>(it.depth));
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void draw_bookmarks_tab() {
    const int cur = g_canvas.current_page();
    if (ImGui::Button(current_page_has_bookmark() ? "删除当前页书签" : "添加当前页书签"))
        toggle_bookmark_current();
    ImGui::Separator();

    const lr::DocRecord* r = g_state.find(g_doc_key);
    if (r == nullptr || r->bookmarks.empty()) {
        ImGui::TextDisabled("暂无书签");
        ImGui::TextDisabled("（按 B 在当前页增删）");
        return;
    }
    ImGui::BeginChild("##bm_list", ImVec2(0, 0), false);
    // × 按钮与 Selectable 同排：Selectable 默认铺满整行，其命中区会盖住后面的按钮，
    // 导致按钮点不到。给 Selectable 显式留出按钮 + 间距的宽度，两者命中区不再重叠。
    const ImGuiStyle& style = ImGui::GetStyle();
    const float btn_w = ImGui::CalcTextSize("×").x + style.FramePadding.x * 2.0f;
    const float sel_w =
        std::max(px(40.0f), ImGui::GetContentRegionAvail().x - btn_w - style.ItemSpacing.x);
    int del = -1;
    for (int i = 0; i < static_cast<int>(r->bookmarks.size()); ++i) {
        const lr::Bookmark& b = r->bookmarks[i];
        ImGui::PushID(i);
        char label[64];
        std::snprintf(label, sizeof label, "第 %d 页", b.page + 1);
        if (ImGui::Selectable(label, b.page == cur, 0, ImVec2(sel_w, 0)))
            request_jump_scroll(b.page, 0.0f);
        ImGui::SameLine();
        if (ImGui::SmallButton("×")) del = i;
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (del >= 0) remove_bookmark_at(del);  // 循环外删除，避免迭代器失效
}

void draw_thumbnails_tab() {
    const int n = g_canvas.page_count();
    if (n <= 0) { ImGui::TextDisabled("无页面"); return; }

    // 只请求当前页附近一段（±40 页）：既够滚动浏览，又不至于一次性渲染整本书（ADR-037）。
    const int cur = std::max(0, g_canvas.current_page());
    const int lo = std::max(0, cur - 40);
    const int hi = std::min(n - 1, cur + 40);
    std::vector<int> want;
    want.reserve(static_cast<std::size_t>(hi - lo + 1));
    for (int i = lo; i <= hi; ++i) want.push_back(i);
    g_renderer->set_thumbs_wanted(std::move(want), kThumbTargetPx);

    ImGui::BeginChild("##thumb_list", ImVec2(0, 0), false);
    for (int i = lo; i <= hi; ++i) {
        const lr::PageSlot s = g_renderer->thumb_slot(i);
        ImGui::PushID(i);
        char label[32];
        std::snprintf(label, sizeof label, "第 %d 页", i + 1);
        if (ImGui::Selectable(label, i == cur)) request_jump_scroll(i, 0.0f);
        if (s.texture != nullptr && s.pixel_w > 0 && s.pixel_h > 0) {
            const float w = px(130.0f);
            const float h = w * static_cast<float>(s.pixel_h) / static_cast<float>(s.pixel_w);
            ImGui::Image(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(s.texture)),
                         ImVec2(w, h));
        } else {
            ImGui::TextDisabled(s.status == lr::PageStatus::Failed ? "（缩略图失败）" : "载入中…");
        }
        ImGui::Separator();
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// 侧栏：高度与画布同高（由 draw_shell 统一给出，避免"三段 + 间距"溢出客户区）。
//
// width 是**动画宽度**（0 ~ 整宽）。实现要点（ADR-055）：面板仍按**整宽**排布、整体左移
// (full_w − width)，由父窗口 ##shell 裁掉左侧超出部分 —— 展开是"从左侧滑入"、收起是
// "滑出左侧"（手机抽屉的手感），且**内容不随动画重排**（子窗口尺寸恒为整宽）。
// ImGui 明确支持"子窗口在视野外 / 负裁剪矩形"（imgui.cpp 的 ChildWindow 分支会按需折叠），
// SetCursorPosX 也不做钳制；BeginChildEx 的尺寸同样不向父窗口收敛，故这一手法是安全的。
// EndChild 后 CursorPosPrevLine.x = 起点 + 整宽 = width，调用方的 SameLine(0,0) 恰好把
// 画布定位到面板右缘 —— 画布随侧栏被推开 / 收回。
void draw_sidebar(float height, float width) {
    const float full_w = px(kSidebarWidthPx);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - (full_w - width));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(8)));
    ImGui::BeginChild("##sidebar", ImVec2(full_w, height), false,
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    // 分栏标题栏不贴着窗口上沿：那里是"顶栏自动唤出带"，紧贴着放会让点标题的手感
    // 变成"在顶栏边缘试探"（实测反馈）。留一点上边距，命中目标就明确了。
    ImGui::Dummy(ImVec2(0.0f, px(6.0f)));

    // 强制切换请求只在**被绘制的那一帧**消费一次：侧栏是滑入的，请求可能在侧栏还没
    // 露出来时就发出（Ctrl+F），故标志要留到真正绘制时才清。
    const int want_tab = g_sidebar_tab_want;
    g_sidebar_tab_want = -1;
    auto tab_flags = [want_tab](int i) {
        return (i == want_tab) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabBar("##sidebar_tabs")) {
        if (ImGui::BeginTabItem("目录", nullptr, tab_flags(0))) { g_sidebar_tab = 0; draw_outline_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("书签", nullptr, tab_flags(1))) { g_sidebar_tab = 1; draw_bookmarks_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("缩略图", nullptr, tab_flags(2))) { g_sidebar_tab = 2; draw_thumbnails_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("搜索", nullptr, tab_flags(3))) { g_sidebar_tab = 3; draw_search_tab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

// 侧栏「搜索」分栏（Phase 8）：输入框 + 进度/统计 + 结果列表。
//
// 为什么放在侧栏而不是独立窗口：它天然是"列表 + 跳转"的形态，与目录/书签/缩略图同类；
// 复用侧栏即可继承既有的滑入滑出、宽度与开关（`O` 或 `Ctrl+F`），不必再造一个窗口，
// 也不会遮挡正文。中文输入由既有的输入法关联切换自动支持（ADR-028/040）。
void draw_search_tab() {
    // 输入框：**改字即取消旧查询，停手 kSearchDebounceSec 后自动检索**（防抖）。
    // 回车与「搜索」按钮是"不等防抖、立刻检索"的快捷路；「停止」让长检索收手。
    // 为什么必须有「停止」：此前只能"把关键字删掉再按一次搜索"才能中断，是反直觉的操作。
    if (g_search_focus) {
        ImGui::SetKeyboardFocusHere();
        g_search_focus = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputTextWithHint("##search", "输入关键字，停手自动搜索",
                                                g_search_buf, sizeof g_search_buf,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const bool click = ImGui::Button("搜索", ImVec2(bw, 0.0f));
    ImGui::SameLine();
    // 「停止」只在"有检索在跑"或"防抖计时中"时可用：其余时候置灰，避免用户误以为点了没反应。
    ImGui::BeginDisabled(!(g_search_active || g_search_pending));
    const bool stop = ImGui::Button("停止", ImVec2(bw, 0.0f));
    ImGui::EndDisabled();
    if (enter || click) {
        search_start();
        g_search_focus = true;   // 保持焦点，方便连续改词
    }
    if (stop) search_stop();

    const int n = static_cast<int>(g_search_hits.size());
    // "扫完"与"中途停下"必须分开说：停下时若也写"共 N 处"，用户会以为全文只有这么多命中。
    const bool scanned_all = (g_search_scanned >= g_search_total);
    if (!search_has_query()) {
        ImGui::TextDisabled("在全文范围内查找文字");
    } else if (g_search_pending) {
        ImGui::TextDisabled("待检索…（停手后自动搜索）");
    } else if (g_search_active) {
        ImGui::TextDisabled("检索中… %d / %d 页 · 已找到 %d 处",
                            g_search_scanned, g_search_total, n);
    } else if (n == 0) {
        if (scanned_all)
            ImGui::TextDisabled("没有找到「%s」", g_search_buf);
        else
            ImGui::TextDisabled("已停止 · 已扫描 %d / %d 页 · 未找到",
                                g_search_scanned, g_search_total);
    } else {
        if (scanned_all)
            ImGui::TextDisabled("共 %d 处", n);
        else
            ImGui::TextDisabled("已停止 · 已扫描 %d / %d 页 · 已找到 %d 处",
                                g_search_scanned, g_search_total, n);
        if (g_search_truncated) {
            ImGui::SameLine();
            ImGui::TextDisabled("（已达上限）");
        }
    }

    const bool has_hits = (n > 0);
    ImGui::BeginDisabled(!has_hits);
    if (ImGui::Button("上一处")) search_step_hit(-1);
    ImGui::SameLine();
    if (ImGui::Button("下一处")) search_step_hit(+1);
    ImGui::EndDisabled();
    if (has_hits) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d / %d", g_search_cur + 1, n);
    }

    ImGui::Separator();

    // 结果列表。用 ListClipper 虚拟化：命中可达数千条，不虚拟化会每帧排版全部文本而掉帧。
    // 每条两行（页码 + 折行的上下文），故 items_height 按两行给 —— Clipper 需要一个
    // 代表值来估算滚动范围，给成一行会让滚动条偏短。
    const float list_h = ImGui::GetContentRegionAvail().y;
    if (list_h <= 1.0f) return;
    ImGui::BeginChild("##search_list", ImVec2(0, list_h), false);
    ImGuiListClipper clipper;
    clipper.Begin(n, ImGui::GetTextLineHeightWithSpacing() * 2.0f);
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const lr::SearchHit& h = g_search_hits[static_cast<std::size_t>(i)];
            ImGui::PushID(i);
            char label[64];
            std::snprintf(label, sizeof label, "第 %d 页", h.page + 1);
            if (ImGui::Selectable(label, i == g_search_cur)) search_goto(i);
            if (!h.snippet.empty()) {
                ImGui::Indent(px(8));
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim));
                ImGui::TextWrapped("%s", h.snippet.c_str());
                ImGui::PopStyleColor();
                ImGui::Unindent(px(8));
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

// ---------------- 弹窗：密码 ----------------

void draw_password_popup() {
    const bool alive = g_pwd_anim.step(ImGui::GetIO().DeltaTime, g_open_password, g_prefs.motion);
    const bool is_open = ImGui::IsPopupOpen("需要密码");
    if (!is_open && !g_open_password) return;   // 无弹窗、也无打开请求：不参与
    if (!is_open) ImGui::OpenPopup("需要密码");
    // **始终**套用动画样式：收敛那一帧 value 已是 0，必须仍为全透明再销毁（否则闪一下）。
    popup_anim_apply(g_pwd_anim);
    if (ImGui::BeginPopupModal("需要密码", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("此文档已加密，请输入密码：");
        ImGui::TextDisabled("%s", g_doc.name_u8.c_str());
        ImGui::SetNextItemWidth(px(260));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        // 认证在途时禁用交互并提示"验证中…"，弹窗保持打开（不再关→开跳变）；
        // 淡出中（!g_open_password）同样禁用，避免重复提交/重复关闭文档。
        const bool locked = g_auth_pending || !g_open_password;
        ImGui::BeginDisabled(locked);
        const bool enter = ImGui::InputText("##pwd", g_password_buf, sizeof g_password_buf,
                                            ImGuiInputTextFlags_Password |
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::EndDisabled();
        if (g_auth_pending)
            ImGui::TextDisabled("验证中…");
        else if (!g_password_error.empty())
            ImGui::TextColored(tone_apply(ImVec4(0.9f, 0.35f, 0.35f, 1.0f)), "%s",
                               g_password_error.c_str());
        ImGui::BeginDisabled(locked);
        const bool ok = enter || ImGui::Button("解锁");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("取消");
        ImGui::EndDisabled();
        if (ok) {
            submit_password();   // 成功后由会话层置 g_open_password=false，动画再收尾
        } else if (cancel) {
            g_open_password = false;   // 只置逻辑关：动画收敛后再真正 CloseCurrentPopup
            close_document();          // 取消即关闭该文档，回到引导页
        }
        if (!alive) ImGui::CloseCurrentPopup();   // 动画收敛：本帧已全透明，安全销毁
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---------------- 界面外壳（顶栏 / 菜单 / 设置 / 帮助） ----------------

// 图标 + 文本（无图标字体时退化为纯文本）。
std::string with_icon(const char* icon, const char* text) {
    std::string s;
    if (g_icons_ok && icon && icon[0]) { s += icon; s += "  "; }
    s += text;
    return s;
}

// 菜单项：图标 + 文案 + 快捷键 + 勾选 / 可用状态 + 可选悬停说明。
//
// **文案有长度上限（12 个 CJK 列）**：弹出菜单的宽度由最长那一项决定，只要出现一项
// 特别长的，整张菜单就被撑宽、其余短项显得空荡（人工反馈：一条很长的内容影响感官）。
// 所以"括号里的补充说明"一律不进文案，改走 tip（悬停提示）—— 信息不丢，宽度可控。
// 该上限由 tests/check_menu_width.py 对所有菜单字面量持续断言，防止再次腐化。
bool menu_item(const char* icon, const char* label, const char* shortcut,
               bool checked = false, bool enabled = true, const char* tip = nullptr) {
    const std::string s = with_icon(icon, label);
    const bool clicked = ImGui::MenuItem(s.c_str(), shortcut, checked, enabled);
    if (tip && tip[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", tip);
    return clicked;
}

// 快捷键一栏取**当前绑定**（用户改键后菜单同步），而不是写死的字符串（ADR-054）。
bool menu_item_cmd(const char* icon, const char* label, Cmd shortcut_cmd,
                   bool checked = false, bool enabled = true, const char* tip = nullptr) {
    const std::string sc = chord_label(g_binds[static_cast<int>(shortcut_cmd)][0]);
    return menu_item(icon, label, sc.c_str(), checked, enabled, tip);
}

// 扁平按钮配色（顶栏/工具条，ADR-045）：
// 默认**无底色**——否则一排按钮就是一排灰块，工具栏显脏；悬停/按下才浮现柔和底色。
// active（如"侧栏已开/设置已开"）用低透明强调色底表示状态，而不是加粗边框。
void push_flat_button(bool active) {
    // 强调色也过一遍纸张方案的色调（ADR-068）：否则暖色方案下会出现"页面暖、按钮冷"。
    const ImVec4 accent = tone_apply(g_dark_theme ? ImVec4(0.42f, 0.65f, 0.94f, 1.0f)
                                                  : ImVec4(0.23f, 0.49f, 0.85f, 1.0f));
    const ImVec4 ink    = g_dark_theme ? ImVec4(1, 1, 1, 1) : ImVec4(0, 0, 0, 1);
    ImGui::PushStyleColor(ImGuiCol_Button,
        active ? ImVec4(accent.x, accent.y, accent.z, 0.18f) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
        active ? ImVec4(accent.x, accent.y, accent.z, 0.28f)
               : ImVec4(ink.x, ink.y, ink.z, g_dark_theme ? 0.12f : 0.06f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
        active ? ImVec4(accent.x, accent.y, accent.z, 0.36f)
               : ImVec4(ink.x, ink.y, ink.z, g_dark_theme ? 0.18f : 0.10f));
}
inline void pop_flat_button() { ImGui::PopStyleColor(3); }

// 图标按钮（顶栏用）。图标字体缺失时显示 text；active 表示"已开启"。
bool tool_button(const char* id, const char* icon, const char* text, const char* tip,
                 bool active = false, bool enabled = true) {
    ImGui::PushID(id);
    const bool use_icon = (g_icons_ok && icon && icon[0]);
    const char* label = use_icon ? icon : text;
    const float h = ImGui::GetFrameHeight();
    push_flat_button(active);
    if (!enabled) ImGui::BeginDisabled();
    const bool clicked = ImGui::Button(label, ImVec2(use_icon ? h : 0.0f, h));
    if (!enabled) ImGui::EndDisabled();
    pop_flat_button();
    if (tip && tip[0] && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", tip);
    ImGui::PopID();
    return clicked;
}

// 弹出菜单统一观感（ADR-045）：更宽松的内边距 + 更高的菜单项。
// 注意：Selectable/MenuItem 的高度 = 文字高（**不吃 FramePadding**），项高完全由
// ItemSpacing.y 决定 —— 悬停高亮向外扩半个间距，恰好铺满整个间距，故放大间距即放大项高，
// 且相邻项的高亮连续、无点击死区。
void push_popup_style() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(kPopupPadXY), px(kPopupPadXY)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), px(kMenuItemGapY)));
}
inline void pop_popup_style() { ImGui::PopStyleVar(2); }

int  zoom_percent() { return static_cast<int>(std::lround(g_canvas.effective_zoom() * 100.0f)); }
// 顶栏/菜单里的缩放按钮与档位：都走动效入口（ADR-047），锚点取视口中心
void zoom_at_center(float factor) {
    zoom_by_animated(factor, g_canvas.viewport_w() * 0.5f, g_canvas.viewport_h() * 0.5f);
}
void zoom_set(float z) {
    zoom_to_animated(z, g_canvas.viewport_w() * 0.5f, g_canvas.viewport_h() * 0.5f);
}

// 页面间距（设置项）变更后立即下发；会失效布局缓存，故只在真正变化时调用。
void apply_gap_pref() {
    g_canvas.set_margin_gap(px(kCanvasMarginPx), gap_ratio_pref());
    g_canvas_scale = ui_scale();
}

// ---- 菜单内容（顶栏主菜单与画布右键菜单共用） ----

void draw_zoom_menu_contents() {
    if (menu_item_cmd(kIcExpand, "适合宽度", Cmd::FitWidth, g_canvas.state().fit_width))
        fit_to_width_animated();
    if (menu_item(kIcDoc, "实际大小", nullptr)) zoom_set(1.0f);
    ImGui::Separator();
    const int pcts[] = { 50, 75, 100, 125, 150, 200, 300 };
    for (const int p : pcts) {
        char lab[16];
        std::snprintf(lab, sizeof lab, "%d%%", p);
        if (ImGui::MenuItem(lab)) zoom_set(static_cast<float>(p) / 100.0f);
    }
}

void draw_view_menu_contents() {
    const bool rd = (g_doc.kind == UiDoc::Kind::Reading);
    {
        const std::string sc = chord_label(g_binds[static_cast<int>(Cmd::ToggleSidebar)][0]);
        ImGui::MenuItem(with_icon(kIcList, "侧栏").c_str(), sc.c_str(), &g_show_sidebar, rd);
    }
    if (ImGui::BeginMenu(with_icon(kIcGrid, "列数").c_str(), rd)) {
        static const char* kNames[] = { "单页", "双页", "三页", "四页" };
        static const Cmd kCols[] = { Cmd::Col1, Cmd::Col2, Cmd::Col3, Cmd::Col4 };
        for (int c = 1; c <= 4; ++c) {
            const bool on = (!g_canvas.state().spread && g_canvas.state().columns == c);
            const std::string sc = chord_label(g_binds[static_cast<int>(kCols[c - 1])][0]);
            if (ImGui::MenuItem(kNames[c - 1], sc.c_str(), on)) g_canvas.set_columns(c);
        }
        ImGui::EndMenu();
    }
    const bool spread = g_canvas.state().spread;
    if (menu_item_cmd(kIcBook, "双页对开", Cmd::ToggleSpread, spread, rd,
                      "双页对开（书籍模式）：封面单独成页，其余两页并列"))
        g_canvas.set_spread(!spread);
    if (ImGui::BeginMenu(with_icon(kIcRotate, "旋转").c_str(), rd)) {
        const int degs[] = { 0, 90, 180, 270 };
        for (const int d : degs) {
            char lab[16];
            if (d == 0) std::snprintf(lab, sizeof lab, "不旋转");
            else        std::snprintf(lab, sizeof lab, "%d°", d);
            const std::string sc =
                (d == 90) ? chord_label(g_binds[static_cast<int>(Cmd::RotateCW)][0]) : std::string();
            if (ImGui::MenuItem(lab, sc.empty() ? nullptr : sc.c_str(), g_rotation == d))
                set_rotation(d);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(with_icon(kIcPalette, "配色").c_str(), rd)) {
        if (ImGui::MenuItem("原色", nullptr, g_scheme == 0)) set_scheme(0);
        const std::string si = chord_label(g_binds[static_cast<int>(Cmd::ToggleDark)][0]);
        const std::string se = chord_label(g_binds[static_cast<int>(Cmd::ToggleWarm)][0]);
        if (ImGui::MenuItem("深色", si.c_str(), g_scheme == 1)) set_scheme(1);
        if (ImGui::MenuItem("暖色", se.c_str(), g_scheme == 2)) set_scheme(2);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (menu_item_cmd(kIcFullscreen, "全屏", Cmd::ToggleFullscreen, g_fullscreen))
        g_request_fullscreen_toggle = true;   // 帧间执行（ADR-060）
}

void draw_nav_menu_contents() {
    const bool rd = (g_doc.kind == UiDoc::Kind::Reading);
    if (menu_item_cmd(kIcHome, "首页", Cmd::FirstPage, false, rd)) request_jump_scroll(0, 0.0f);
    if (menu_item_cmd(kIcPrev, "上一页", Cmd::PrevRow, false, rd, "上一页（多列或对开时按整行推进）"))
        scroll_by_rows(-1);
    if (menu_item_cmd(kIcNext, "下一页", Cmd::NextRow, false, rd, "下一页（多列或对开时按整行推进）"))
        scroll_by_rows(+1);
    if (menu_item_cmd(kIcArrowDown, "末页", Cmd::LastPage, false, rd))
        request_jump_scroll(g_canvas.page_count() - 1, 0.0f);
    ImGui::Separator();
    if (menu_item_cmd(kIcSearch, "跳转页码…", Cmd::JumpPage, false, rd)) open_jump_popup();
    if (menu_item_cmd(kIcStar, current_page_has_bookmark() ? "删除书签" : "添加书签",
                      Cmd::ToggleBookmark, current_page_has_bookmark(), rd,
                      "为当前页添加 / 移除书签"))
        toggle_bookmark_current();
}

void draw_main_menu_contents() {
    const bool has_doc = (g_doc.kind != UiDoc::Kind::None);
    const bool rd = (g_doc.kind == UiDoc::Kind::Reading);
    if (menu_item_cmd(kIcOpenFile, "打开文档…", Cmd::OpenFile)) g_request_open_dialog = true;
    if (menu_item(kIcClose, "关闭文档", nullptr, false, has_doc)) close_document();
    ImGui::Separator();
    if (ImGui::BeginMenu(with_icon(kIcDoc, "视图").c_str(), rd)) {
        draw_view_menu_contents(); ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(with_icon(kIcNext, "导航").c_str(), rd)) {
        draw_nav_menu_contents(); ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(with_icon(kIcExpand, "缩放").c_str(), rd)) {
        draw_zoom_menu_contents(); ImGui::EndMenu();
    }
    ImGui::Separator();
    if (menu_item_cmd(kIcSearch, "查找…", Cmd::OpenSearch, false, rd)) {
        set_sidebar(true, 3);
        g_search_focus = true;
    }
    if (menu_item_cmd(kIcSettings, "设置…", Cmd::OpenSettings)) g_show_settings = true;
    if (menu_item_cmd(kIcHelp, "按键设置…", Cmd::OpenKeys)) {
        g_show_settings = true;
        g_settings_open_tab = 3;   // 直接落到「按键」分栏
    }
    ImGui::Separator();
    if (menu_item(kIcClose, "退出", nullptr)) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

// 画布右键菜单：在画布子窗口的 ID 作用域内调用（BeginPopupContextWindow 依赖它）。
void draw_canvas_context_menu() {
    if (g_doc.kind != UiDoc::Kind::Reading) return;
    // 键盘入口（Shift+F10 / 菜单键）与右键**同一 ID**：在本窗口作用域内显式打开。
    if (g_open_canvas_ctx) { g_open_canvas_ctx = false; ImGui::OpenPopup("##canvas_ctx"); }
    push_popup_style();
    if (ImGui::BeginPopupContextWindow("##canvas_ctx", ImGuiPopupFlags_MouseButtonRight)) {
        // ---- 文本 / 图片 / 链接（Phase 8）----
        // 置灰规则：没有选区 → 「复制」不可用；右键位置不在页面上 → 「复制图片」不可用；
        // 该位置没有链接 → 链接两项不可用。宁可置灰也不隐藏：菜单长度稳定，用户能找到功能。
        const bool has_sel = g_sel.active && !g_sel.rects.empty();
        const bool has_link = g_ctx_link_valid;
        if (menu_item_cmd(kIcCopy, "复制", Cmd::Copy, false, has_sel,
                          "复制选中的文本（在文字上拖拽即可选择）"))
            selection_copy();
        if (menu_item(kIcImage, "复制图片", nullptr, false, g_ctx_image_valid,
                      "复制此处的嵌入图片（原始分辨率）"))
            copy_image_at_context();
        if (menu_item(kIcLink, "打开链接", nullptr, false, has_link))
            open_link_at_context();
        if (menu_item(kIcLink, "复制链接", nullptr, false, has_link && !g_ctx_link.uri.empty())) {
            if (set_clipboard_text(g_ctx_link.uri)) show_toast("已复制链接地址");
        }
        if (menu_item_cmd(kIcSearch, "查找…", Cmd::OpenSearch)) {
            set_sidebar(true, 3);
            g_search_focus = true;
        }
        ImGui::Separator();
        if (menu_item_cmd(kIcPrev, "上一页", Cmd::PrevRow, false, true, "上一页（对开时按整行推进）"))
            scroll_by_rows(-1);
        if (menu_item_cmd(kIcNext, "下一页", Cmd::NextRow, false, true, "下一页（对开时按整行推进）"))
            scroll_by_rows(+1);
        ImGui::Separator();
        if (ImGui::BeginMenu(with_icon(kIcExpand, "缩放").c_str())) {
            draw_zoom_menu_contents(); ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(with_icon(kIcDoc, "视图").c_str())) {
            draw_view_menu_contents(); ImGui::EndMenu();
        }
        ImGui::Separator();
        if (menu_item_cmd(kIcSearch, "跳转页码…", Cmd::JumpPage)) open_jump_popup();
        if (menu_item_cmd(kIcStar, current_page_has_bookmark() ? "删除书签" : "添加书签",
                          Cmd::ToggleBookmark, current_page_has_bookmark(), true,
                          "为当前页添加 / 移除书签"))
            toggle_bookmark_current();
        if (menu_item_cmd(kIcList, "侧栏", Cmd::ToggleSidebar, g_show_sidebar))
            set_sidebar(!g_show_sidebar, 0);
        ImGui::Separator();
        if (menu_item_cmd(kIcSettings, "设置…", Cmd::OpenSettings)) g_show_settings = true;
        if (menu_item_cmd(kIcHelp, "按键设置…", Cmd::OpenKeys)) {
            g_show_settings = true;
            g_settings_open_tab = 3;
        }
        ImGui::EndPopup();
    }
    pop_popup_style();
}

// ---- 顶栏自动隐藏 ----
// 阅读态下，鼠标离开窗口顶部一段时间就收起顶栏（沉浸阅读）；移到顶部即重现。
// 引导/失败/密码等状态、以及打开设置时始终显示（否则用户找不到入口）。
// **画布右键菜单、跳页/密码/确认弹窗刻意不唤醒顶栏**（人工反馈）：它们是屏幕中央/画布上的
// 操作，把顶栏一并滑出来只是无谓的视觉噪音。唯一例外是**顶栏自己的弹出菜单**
// （主菜单 / 缩放档位）—— 它们挂在顶栏按钮下方，顶栏滑走会让菜单悬空，故用 g_toolbar_pinned 钉住。
void update_toolbar_visibility() {
    if (g_doc.kind != UiDoc::Kind::Reading || !g_prefs.auto_hide_toolbar) {
        g_toolbar_visible = true;
        g_toolbar_idle_since = -1.0;
        return;
    }
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const ImVec2 vp = ImGui::GetMainViewport()->Pos;

    // **侧栏区不参与顶栏的唤起/收起。** 侧栏的分栏标题栏就在窗口最上方，鼠标必然要贴到
    // 顶端才点得到它；而"贴近顶端即唤出顶栏"会把顶栏滑出来，顶栏一出来整条侧栏（连同
    // 标题栏）就被推下去 40px —— 正要点的分栏跳走，反而点中刚滑出的顶栏（实测反馈：
    // "选择的时候容易唤出菜单栏，手感不好"）。故鼠标在侧栏区时**冻结**顶栏状态：
    // 既不因它而唤起，也不因离开顶端带而收起（后者会让侧栏在光标下又滑回去）。
    // 顶栏仍可从画布一侧的顶端带唤起（x 在侧栏右侧），或用固定显示/设置窗口。
    const bool over_sidebar = g_show_sidebar && g_sidebar_w > 1.0f &&
                              (mp.x - vp.x) < g_sidebar_w;
    const bool near_top = !over_sidebar &&
                          (mp.y - vp.y) <= px(kTopBarH + kToolbarRevealBandPx);
    if (near_top || g_show_settings || g_toolbar_pinned) {
        g_toolbar_visible = true;
        g_toolbar_idle_since = -1.0;
        return;
    }
    if (over_sidebar) return;   // 冻结：不推进"该收起了"的计时
    if (!g_toolbar_visible) return;
    const double now = ImGui::GetTime();
    if (g_toolbar_idle_since < 0.0) {
        g_toolbar_idle_since = now;
    } else if (now - g_toolbar_idle_since >= kToolbarHideDelaySec) {
        g_toolbar_visible = false;
        g_toolbar_idle_since = -1.0;
    }
}

bool top_bar_should_show() {
    if (g_doc.kind != UiDoc::Kind::Reading) return true;
    if (!g_prefs.auto_hide_toolbar) return true;
    if (g_show_settings || g_toolbar_pinned) return true;
    return g_toolbar_visible;
}

// 顶栏高度的滑入/滑出插值：自动隐藏不再"整块消失"，而是把顶栏推上去。
// 返回当前高度（0 ~ px(kTopBarH)）。调用方在高度为 0 时**不要**创建子窗口 ——
// ImGui 的 BeginChild 把 size.y == 0 当作"自动高度"，会吃掉整个客户区。
float update_top_bar_height(float dt) {
    const float full = px(kTopBarH);
    if (g_top_bar_h < 0.0f) g_top_bar_h = full;   // 首帧：未初始化即按展开态
    const float want = top_bar_should_show() ? full : 0.0f;
    if (!g_prefs.motion) {
        g_top_bar_h = want;
        return want;
    }
    g_top_bar_h = approach(g_top_bar_h, want, kTopBarAnimRate, dt);
    if (std::fabs(g_top_bar_h - want) < kMotionEpsPx) g_top_bar_h = want;
    return g_top_bar_h;
}

// 侧栏宽度的滑入/滑出插值：返回当前动画宽度（0 ~ px(kSidebarWidthPx)）。
// 与顶栏同一手法（一阶滞后、帧率无关）；关闭动效时直切。
float update_sidebar_width(float dt) {
    const float full = px(kSidebarWidthPx);
    const float want = g_show_sidebar ? full : 0.0f;
    if (!g_prefs.motion) {
        g_sidebar_w = want;
        return want;
    }
    g_sidebar_w = approach(g_sidebar_w, want, kSidebarAnimRate, dt);
    if (std::fabs(g_sidebar_w - want) < kMotionEpsPx) g_sidebar_w = want;
    return g_sidebar_w;
}

// ---- 顶栏 ----
// bar_h 由 draw_shell 传入（可小于整条高度：自动隐藏的滑出动效）。
void draw_top_bar(float bar_h) {
    const float full_h = px(kTopBarH);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(kChromePadX), 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(kChromeGapX), 0));
    ImGui::BeginChild("##topbar", ImVec2(0, bar_h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), g_pal.chrome);
    dl->AddLine(ImVec2(wp.x, wp.y + ws.y - 0.5f), ImVec2(wp.x + ws.x, wp.y + ws.y - 0.5f),
                g_pal.chrome_border);

    const bool rd = (g_doc.kind == UiDoc::Kind::Reading);
    const float h = ImGui::GetFrameHeight();
    // 内容始终按**整条高度**排布，再整体上移被收起的那部分：于是收起过程表现为"滑出"，
    // 而不是原地被裁掉（子窗口会裁掉超出部分，负偏移正好落到窗口上缘之外）。
    const float dy = -(full_h - bar_h);
    const float y = (full_h - h) * 0.5f + dy;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPos(ImVec2(px(kChromePadX), y));

    // 主菜单（☰）——所有命令的唯一入口，避免把按钮铺满工具栏
    bool pinned = false;   // 顶栏自己的弹出菜单是否开着（见 g_toolbar_pinned）
    if (tool_button("##mainmenu", kIcMenu, "菜单", nullptr))
        ImGui::OpenPopup("##mainmenu_pop");
    push_popup_style();
    const bool main_open = ImGui::BeginPopup("##mainmenu_pop");
    if (main_open) { draw_main_menu_contents(); ImGui::EndPopup(); }
    pop_popup_style();
    if (main_open) pinned = true;

    if (rd) {
        ImGui::SameLine();
        if (tool_button("##sidebar", kIcPane, "侧栏", "侧栏 (O)", g_show_sidebar))
            set_sidebar(!g_show_sidebar, 0);
    }

    // 右侧控件簇宽度（先算宽度，标题才能安全居中且不与它重叠）
    const std::string ztxt = std::to_string(zoom_percent()) + "%";
    const float zw = ImGui::CalcTextSize(ztxt.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float cluster = rd ? (h * 4.0f + zw + gap * 5.0f + px(8.0f))
                             : (h * 2.0f + gap * 3.0f + px(8.0f));

    const char* title = g_doc.name_u8.empty() ? "Lilith Reader" : g_doc.name_u8.c_str();
    const float tw = ImGui::CalcTextSize(title).x;
    const float left_end = ImGui::GetCursorPosX();
    const float right_start = ws.x - cluster - px(kChromePadX);
    if (right_start - tw - px(24.0f) > left_end) {   // 空间不足就省略标题（窄窗口/长文件名）
        ImGui::SameLine();
        ImGui::SetCursorPos(ImVec2((ws.x - tw) * 0.5f,
                                   (full_h - ImGui::GetTextLineHeight()) * 0.5f + dy));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim), "%s", title);
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(right_start);
    ImGui::SetCursorPosY(y);
    if (rd) {
        if (tool_button("##bm", kIcStar, "书签", "本页书签 (B)", current_page_has_bookmark()))
            toggle_bookmark_current();
        ImGui::SameLine();
        if (tool_button("##zout", kIcZoomOut, "-", "缩小"))
            zoom_at_center(1.0f / kZoomStep);
        ImGui::SameLine();
        push_flat_button(false);
        if (ImGui::Button(ztxt.c_str(), ImVec2(zw, h))) ImGui::OpenPopup("##zoompop");
        pop_flat_button();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("缩放");
        push_popup_style();
        const bool zoom_open = ImGui::BeginPopup("##zoompop");
        if (zoom_open) { draw_zoom_menu_contents(); ImGui::EndPopup(); }
        pop_popup_style();
        if (zoom_open) pinned = true;
        ImGui::SameLine();
        if (tool_button("##zin", kIcZoomIn, "+", "放大"))
            zoom_at_center(kZoomStep);
        ImGui::SameLine();
    } else {
        if (tool_button("##open", kIcOpenFile, "打开…", "打开文档"))
            g_request_open_dialog = true;
        ImGui::SameLine();
    }
    // 提示里带上**当前绑定**（用户改过键之后不能再说 Ctrl+,）
    const std::string tip_settings =
        "设置 (" + chord_label(g_binds[static_cast<int>(Cmd::OpenSettings)][0]) + ")";
    if (tool_button("##settings", kIcSettings, "设置", tip_settings.c_str(), g_show_settings))
        g_show_settings = true;

    g_toolbar_pinned = pinned;   // 供下一帧的 update_toolbar_visibility / top_bar_should_show 读
    ImGui::EndChild();
}

// ---------------- 通用确认弹窗（ADR-062）----------------
//
// 浮动窗口的默认尺寸与位置：期望值按视口上限钳制后居中，小屏/低分辨率下也不超出屏幕。
ImVec2 clamp_to_viewport(const ImGuiViewport* vp, const ImVec2& want) {
    return ImVec2(std::min(want.x, vp->Size.x * 0.92f),
                  std::min(want.y, vp->Size.y * 0.90f));
}
ImVec2 centered_on_viewport(const ImGuiViewport* vp, const ImVec2& size) {
    return ImVec2(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
                  vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
}

// 一处弹窗、多处复用：① 智能匹配的"要不要沿用这份阅读数据"询问（主=沿用 / 次=从头开始）；
// ② 阅读数据删除 / 清空的二次确认（主=删除 / 次=取消）。与跳页、密码弹窗同一套开合动效
// （ADR-059）：逻辑关闭只把目标降到 0，等动画收敛后再真正销毁 popup。
//
// 版式（不用 ImGui 的标题栏，自己排版）：标题（强调色）→ 说明（折行）→ 明细行
// （标签次要色 + 值折行）→ 分隔线 → 按钮**右对齐**（主按钮在最右，符合"确认在右"的惯例）。
// 不用标题栏的原因：标题栏会把内部名"确认"显示出来、还带一个关闭按钮，既重复又容易误点；
// 自绘版面后弹窗完全由内容决定尺寸，长路径折行也不会把窗口撑到屏幕外。
ToggleAnim g_confirm_anim;

void request_confirm(ConfirmKind kind, std::string title, std::string body,
                     std::vector<std::pair<std::string, std::string>> rows,
                     std::string ok_label, std::string alt_label, std::uint64_t target) {
    // 已有确认在挂起时**不覆盖**：先到的那个才是用户该先处理的。
    if (g_confirm_open) return;
    g_confirm_kind = kind;
    g_confirm_title = std::move(title);
    g_confirm_body = std::move(body);
    g_confirm_rows = std::move(rows);
    g_confirm_ok = std::move(ok_label);
    g_confirm_alt = std::move(alt_label);
    g_confirm_target = target;
    g_confirm_open = true;
}

void draw_confirm_popup() {
    constexpr const char* kConfirmId = "确认##confirm";
    const bool alive = g_confirm_anim.step(ImGui::GetIO().DeltaTime, g_confirm_open, g_prefs.motion);
    const bool is_open = ImGui::IsPopupOpen(kConfirmId);
    if (!is_open && !g_confirm_open) return;   // 无弹窗、也无打开请求：不参与
    if (!is_open) ImGui::OpenPopup(kConfirmId);
    popup_anim_apply(g_confirm_anim);

    if (ImGui::BeginPopupModal(kConfirmId, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize |
                               ImGuiWindowFlags_NoTitleBar |
                               ImGuiWindowFlags_NoMove)) {
        const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim);
        const float wrap = px(420.0f);

        // 标题（强调色）
        ImGui::PushTextWrapPos(wrap);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(g_pal.accent));
        ImGui::TextUnformatted(g_confirm_title.c_str());
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();
        ImGui::Spacing();

        // 说明
        if (!g_confirm_body.empty()) {
            ImGui::PushTextWrapPos(wrap);
            ImGui::TextUnformatted(g_confirm_body.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
        }

        // 明细行：标签固定的左列 + 折行的值，长路径不再和正文糊在一起
        if (!g_confirm_rows.empty()) {
            const float label_w = px(72.0f);   // 容得下 3 个汉字标签（基准字号 20px）
            for (const auto& kv : g_confirm_rows) {
                ImGui::TextColored(dim, "%s", kv.first.c_str());
                ImGui::SameLine(label_w);
                ImGui::PushTextWrapPos(wrap);
                ImGui::TextUnformatted(kv.second.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Spacing();
        }

        ImGui::Separator();
        ImGui::Spacing();

        ImGui::BeginDisabled(!g_confirm_open);   // 淡出中不再响应，避免重复触发
        const ImGuiStyle& st = ImGui::GetStyle();
        const float ok_w  = ImGui::CalcTextSize(g_confirm_ok.c_str()).x + st.FramePadding.x * 2.0f;
        const float alt_w = ImGui::CalcTextSize(g_confirm_alt.c_str()).x + st.FramePadding.x * 2.0f;
        const float min_w = px(88.0f);
        const float bw_ok = ok_w > min_w ? ok_w : min_w;
        const float bw_alt = alt_w > min_w ? alt_w : min_w;
        // 右对齐：末按钮右缘距窗口右内边 = 标准内边距（首帧窗口尚未按内容定宽，故夹到左边距）
        const float row_w = bw_ok + bw_alt + st.ItemSpacing.x;
        const float x = ImGui::GetWindowWidth() - st.WindowPadding.x - row_w;
        ImGui::SetCursorPosX(x > st.WindowPadding.x ? x : st.WindowPadding.x);
        const bool alt = ImGui::Button(g_confirm_alt.c_str(), ImVec2(bw_alt, 0));
        ImGui::SameLine();
        const bool ok = ImGui::Button(g_confirm_ok.c_str(), ImVec2(bw_ok, 0));
        ImGui::EndDisabled();

        if (ok || alt) {
            const ConfirmKind kind = g_confirm_kind;
            const std::uint64_t target = g_confirm_target;
            g_confirm_open = false;             // 逻辑关：动画收敛后再 CloseCurrentPopup
            g_confirm_kind = ConfirmKind::None;
            if (ok) {
                // 主按钮 = 确认动作。Relocate 的主按钮无需动作 —— 命中时已经沿用（adopt 过）了。
                if (kind == ConfirmKind::ClearOne)           clear_reading_data(target);
                else if (kind == ConfirmKind::ClearAll)      clear_all_reading_data();
                else if (kind == ConfirmKind::PruneUnknown)  clear_unknown_reading_data();
            } else if (kind == ConfirmKind::Relocate) {
                detach_current_progress();      // 次按钮 = 另起一份
            }
        }
        if (!alive) { ImGui::CloseCurrentPopup(); g_confirm_target = 0; g_confirm_rows.clear(); }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

// ---------------- 设置 · 阅读数据分栏（ADR-062/063/065）----------------
//
// 一条记录 = 一份**阅读数据**（读到哪 / 书签 / 视图参数）。同一份内容出现在多个位置时只占
// 一条 —— 位置在卡片里列成"路径树"，这正是"一份数据、若干路径"的表达。此前一条记录只留
// 最后一个路径，复制出来的第二份会把第一份顶掉，看不出这是同一份文档的两处副本。
//
// 版式：卡片（圆角底 + 悬停描边）= 标题行（文档名 + 行尾删除按钮）/ 统计行（第 N / M 页 ·
// K 个书签 · J 个位置）/ 缩进的路径行（左侧一条竖导引线，把它们挂在标题下面）。
//
// 三个刻意的取舍：
//   · **不用 TreeNode**：那会带一个三角与整行选中态，而路径是只读信息、不该看起来可点；
//     一条安静的竖导引线更像"附属关系"，也更省横向空间。
//   · **路径单行中间省略，不折行**：折行会让卡片高度参差，且长路径开头往往是同样的前缀，
//     堆在一起看着像糊成一团；中间省略同时保住父目录与文件名，完整路径留给悬停提示。
//   · **"位置未知"的旧记录单独成区**：v1 记录（升级前）没存过路径，硬摆在列表里就是一堆
//     "未知文档"。它们挪到末尾、附一句解释，要么等再次打开那个文件时按路径键被认领，
//     要么一键清掉。
namespace {

// 路径 → 文件名（UTF-8 进出）；空路径返回空串。
std::string file_name_of_u8(const std::string& path_u8) {
    if (path_u8.empty()) return {};
    return lr::wide_to_utf8(lr::file_name_of(lr::utf8_to_wide(path_u8)));
}

// UTF-8 每个字符的起始字节下标（末尾补 size）。截断必须切在字符边界上，
// 否则半个汉字会被画成豆腐块。
std::vector<std::size_t> utf8_char_offsets(const std::string& s) {
    std::vector<std::size_t> v;
    for (std::size_t i = 0; i < s.size();) {
        v.push_back(i);
        const unsigned char c = static_cast<unsigned char>(s[i]);
        i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    }
    v.push_back(s.size());
    return v;
}

// 放不下就加省略号（middle：掐中间，保留头 55% / 尾 45%；否则掐尾）。
// 二分"最多留几个字符"，每步两次 CalcTextSize，与串长无关地收敛到 log(n)。
std::string ellipsize(const std::string& s, float max_w, bool middle) {
    if (s.empty()) return s;
    if (ImGui::CalcTextSize(s.c_str()).x <= max_w) return s;
    const char* kEll = "…";
    const float budget = max_w - ImGui::CalcTextSize(kEll).x;
    if (budget <= 0.0f) return kEll;

    const std::vector<std::size_t> off = utf8_char_offsets(s);
    const std::size_t chars = off.size() - 1;
    const auto width_of = [&](std::size_t keep) {
        const std::size_t head = middle ? keep * 55 / 100 : keep;
        const std::size_t tail = middle ? keep - head : 0;
        return ImGui::CalcTextSize(s.substr(0, off[head]).c_str()).x +
               ImGui::CalcTextSize(s.substr(off[chars - tail]).c_str()).x;
    };
    std::size_t lo = 0, hi = chars;
    while (lo < hi) {
        const std::size_t mid = (lo + hi + 1) / 2;
        if (width_of(mid) <= budget) lo = mid; else hi = mid - 1;
    }
    if (lo == 0) return kEll;
    const std::size_t head = middle ? lo * 55 / 100 : lo;
    const std::size_t tail = middle ? lo - head : 0;
    return s.substr(0, off[head]) + kEll + s.substr(off[chars - tail]);
}

// 危险动作的按钮配色（无底色 → 悬停浮出红）：删除图标与"清空 / 清理"文字按钮共用。
// 红色属**语义色**，tone_apply 只把它的色相往方案色相拉近一个小比例，保住"红=危险"。
ImVec4 danger_ink() {
    return tone_apply(g_dark_theme ? ImVec4(0.95f, 0.47f, 0.45f, 1.0f)
                                   : ImVec4(0.76f, 0.22f, 0.19f, 1.0f));
}
void push_danger_button(bool hovered) {
    const ImVec4 red = danger_ink();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(red.x, red.y, red.z, 0.14f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(red.x, red.y, red.z, 0.26f));
    ImGui::PushStyleColor(ImGuiCol_Text, hovered ? red : col_dim());
}
inline void pop_danger_button() { ImGui::PopStyleColor(4); }

// 行尾的删除按钮：默认无底色，悬停才浮出危险色。
// 刻意不用 ImGui 的默认按钮——那在浅色主题下就是一块灰方块（人工反馈"按钮很丑"）。
bool danger_icon_button(float x, float y, const char* tip) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 b0(x, y), b1(x + h, y + h);
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                     ImGui::IsMouseHoveringRect(b0, b1);
    push_danger_button(hov);
    ImGui::SetCursorScreenPos(b0);
    const bool clicked = ImGui::Button(g_icons_ok ? kIcTrash : "×", ImVec2(h, h));
    pop_danger_button();
    if (tip != nullptr && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", tip);
    return clicked;
}

// 破坏性动作的文字按钮（清空全部 / 清理旧记录）：同一套"无底色、悬停变红"的观感。
bool danger_text_button(const char* label) {
    const ImVec2 sz = ImGui::CalcTextSize(label);
    const ImGuiStyle& st = ImGui::GetStyle();
    const float w = sz.x + st.FramePadding.x * 2.0f;
    const float h = sz.y + st.FramePadding.y * 2.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                     ImGui::IsMouseHoveringRect(p0, ImVec2(p0.x + w, p0.y + h));
    push_danger_button(hov);
    const bool clicked = ImGui::Button(label);
    pop_danger_button();
    return clicked;
}

// 一张文档卡：标题行 + 统计行 + 若干路径行。
void draw_reading_data_card(std::size_t i) {
    const std::uint64_t key = g_state.docs[i].first;
    const lr::DocRecord& r = g_state.docs[i].second;

    const float pad     = px(10.0f);
    const float row_gap = px(5.0f);
    const float line    = ImGui::GetTextLineHeight();
    const float row_h   = line + px(2.0f);
    const float indent  = px(16.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;

    constexpr std::size_t kMaxRows = 3;   // 最多铺 3 行路径，多出来的折成"还有 N 处…"
    const bool more = r.locations.size() > kMaxRows;
    const std::size_t shown = more ? kMaxRows : r.locations.size();
    const std::size_t rows = shown + (more ? 1u : 0u);
    const float card_h = pad * 2.0f + line * 2.0f + row_gap + px(6.0f) +
                         static_cast<float>(rows) * row_h;
    const ImVec2 p1(p0.x + w, p0.y + card_h);
    const bool hov = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                     ImGui::IsMouseHoveringRect(p0, p1);

    ImGui::PushID(static_cast<int>(i));
    ImGui::Dummy(ImVec2(w, card_h));   // 先占位撑开整张卡；后面的元素都按绝对坐标摆

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const bool dark = g_dark_theme;
    dl->AddRectFilled(p0, p1, dark ? IM_COL32(255, 255, 255, hov ? 16 : 8)
                                   : IM_COL32(0, 0, 0, hov ? 10 : 5), px(7.0f));
    dl->AddRect(p0, p1,
                hov ? g_pal.accent
                    : (dark ? IM_COL32(255, 255, 255, 26) : IM_COL32(0, 0, 0, 18)),
                px(7.0f), 0, px(1.0f));

    const std::string title = file_name_of_u8(r.locations.front());
    const float btn = ImGui::GetFrameHeight();

    // 标题行：文档名（左） + 删除按钮（右，垂直居中于标题行）
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad));
    ImGui::TextColored(col_text(), "%s",
                       ellipsize(title, w - pad * 2.0f - btn - px(8.0f), false).c_str());
    if (danger_icon_button(p1.x - pad - btn, p0.y + pad + (line - btn) * 0.5f,
                           "删除这份阅读数据")) {
        request_confirm(ConfirmKind::ClearOne, "删除这份阅读数据？",
                        "将删除它的阅读位置与书签，文档本身不受影响。",
                        { { "文档", title }, { "最近位置", r.locations.front() } },
                        "删除", "取消", key);
    }

    // 统计行
    char meta[192];
    std::size_t n = static_cast<std::size_t>(std::snprintf(
        meta, sizeof meta, "第 %d 页", r.page + 1));
    if (r.page_count > 0 && n < sizeof meta)
        n += static_cast<std::size_t>(std::snprintf(
            meta + n, sizeof meta - n, " / %d", r.page_count));
    if (n < sizeof meta)
        std::snprintf(meta + n, sizeof meta - n, " · %d 个书签 · %d 个位置",
                      static_cast<int>(r.bookmarks.size()),
                      static_cast<int>(r.locations.size()));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + pad + line + row_gap));
    ImGui::TextColored(col_dim(), "%s", meta);

    // 路径行：左侧一条竖导引线 + 每行一个短横，把若干位置挂在标题下面
    const float x_rule = p0.x + pad + px(6.0f);
    const float x_text = p0.x + pad + indent;
    const float y0 = p0.y + pad + line * 2.0f + row_gap + px(6.0f);
    const ImVec4 dim4 = col_dim();
    const ImU32 rule = ImGui::ColorConvertFloat4ToU32(
        ImVec4(dim4.x, dim4.y, dim4.z, dim4.w * 0.40f));
    if (rows >= 2)   // 只有一条位置时不画竖线，那一段零长度的线毫无意义
        dl->AddLine(ImVec2(x_rule, y0 + row_h * 0.5f),
                    ImVec2(x_rule, y0 + row_h * (static_cast<float>(rows) - 0.5f)),
                    rule, px(1.0f));
    for (std::size_t k = 0; k < shown; ++k) {
        const float y = y0 + row_h * static_cast<float>(k);
        dl->AddLine(ImVec2(x_rule, y + row_h * 0.5f),
                    ImVec2(x_rule + px(6.0f), y + row_h * 0.5f), rule, px(1.0f));
        ImGui::SetCursorScreenPos(ImVec2(x_text, y));
        const std::string cut = ellipsize(r.locations[k], p1.x - pad - x_text, true);
        ImGui::TextColored(col_dim(), "%s", cut.c_str());
        // 被省略的完整路径给悬停提示（用鼠标位置判定，不依赖文本项有没有 id）
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (cut != r.locations[k] && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseHoveringRect(p0, p1))
            ImGui::SetTooltip("%s", r.locations[k].c_str());
    }
    if (more) {
        ImGui::SetCursorScreenPos(ImVec2(x_text, y0 + row_h * static_cast<float>(shown)));
        ImGui::TextColored(col_dim(), "还有 %d 处…",
                           static_cast<int>(r.locations.size() - shown));
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(a, b) && ImGui::IsMouseHoveringRect(p0, p1)) {
            std::string rest;
            for (std::size_t k = shown; k < r.locations.size(); ++k) {
                rest += r.locations[k];
                rest += '\n';
            }
            if (!rest.empty()) rest.pop_back();
            ImGui::SetTooltip("%s", rest.c_str());
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + px(9.0f)));
    ImGui::Dummy(ImVec2(1.0f, 0.0f));   // 收尾：让下一张卡从这里开始
    ImGui::PopID();
}

// 位置未知的旧记录（升级前的 v1 记录没存过路径，永远显示不出文档名）单独成区：
// 说清它们是什么、什么时候会被认领，并给一个只清这一类的入口。
void draw_unknown_records(const std::vector<std::size_t>& idx) {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(col_text(), "位置未知的旧记录 · %d 条", static_cast<int>(idx.size()));
    ImGui::TextColored(col_dim(), "%s", "升级前的记录没有保存过文件路径，认不出是哪份文档；");
    ImGui::TextColored(col_dim(), "%s", "再次打开那个文件时，它会自动认领这里的一条。");
    ImGui::Spacing();

    const float line = ImGui::GetTextLineHeight();
    const float btn = ImGui::GetFrameHeight();
    const float h = line > btn ? line : btn;
    const float w = ImGui::GetContentRegionAvail().x;
    for (const std::size_t i : idx) {
        const std::uint64_t key = g_state.docs[i].first;
        const lr::DocRecord& r = g_state.docs[i].second;
        ImGui::PushID(static_cast<int>(i));

        char text[128];
        if (r.page_count > 0)
            std::snprintf(text, sizeof text, "第 %d / %d 页 · %d 个书签",
                          r.page + 1, r.page_count, static_cast<int>(r.bookmarks.size()));
        else
            std::snprintf(text, sizeof text, "第 %d 页 · %d 个书签",
                          r.page + 1, static_cast<int>(r.bookmarks.size()));

        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(p0.x + px(6.0f), p0.y + (h - line) * 0.5f));
        ImGui::TextColored(col_dim(), "%s", text);
        if (danger_icon_button(p0.x + w - btn, p0.y, "删除这条旧记录")) {
            request_confirm(ConfirmKind::ClearOne, "删除这条阅读数据？",
                            "将删除它的阅读位置与书签，文档本身不受影响。",
                            { { "记录", text } }, "删除", "取消", key);
        }
        ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + px(2.0f)));
        ImGui::Dummy(ImVec2(1.0f, 0.0f));
        ImGui::PopID();
    }

    ImGui::Spacing();
    char label[64];
    std::snprintf(label, sizeof label, "清理这 %d 条", static_cast<int>(idx.size()));
    if (danger_text_button(label)) {
        char body[224];
        std::snprintf(body, sizeof body,
                      "将删除这 %d 条认不出文档的记录（阅读位置与书签）。\n"
                      "文档本身不受影响，其余记录也不受影响。",
                      static_cast<int>(idx.size()));
        request_confirm(ConfirmKind::PruneUnknown, "清理位置未知的旧记录？", body, {},
                        "清理", "取消", 0);
    }
}

}  // namespace

// 设置项表格的辅助函数定义在下面（分栏共用）；这里只是前置声明。
bool settings_rows_begin(const char* id);
void settings_rows_end();
void settings_row(const char* label);
void settings_note(const char* text);

void draw_settings_reading_data_tab() {
    // 顶部一行：智能匹配三态。它决定"文件换位置后要不要沿用同一份阅读数据"，
    // 与这一栏是同一件事 —— 放这儿，免得「阅读」栏里再出现一句"见阅读数据分栏"。
    if (settings_rows_begin("##set_rd_head")) {
        settings_row("智能匹配");
        const char* smart[] = { "关（只认路径）", "询问", "自动沿用" };
        if (ImGui::Combo("##smart", &g_prefs.smart_match, smart, IM_ARRAYSIZE(smart)))
            save_prefs();
        settings_note("换位置后沿用同一份阅读数据");
        settings_rows_end();
    }
    ImGui::Spacing();

    // 底部操作条**不参与滚动**：列表用负高度，剩下的一行留给「清空全部」+ 计数
    const float footer_h = ImGui::GetFrameHeightWithSpacing();

    std::vector<std::size_t> known, unknown;
    for (std::size_t i = 0; i < g_state.docs.size(); ++i)
        (g_state.docs[i].second.locations.empty() ? unknown : known).push_back(i);

    ImGui::BeginChild("##rd_list", ImVec2(0, -footer_h),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoNav);
    if (g_state.docs.empty()) {
        ImGui::TextColored(col_dim(), "%s", "还没有记录任何阅读数据。");
        ImGui::TextColored(col_dim(), "%s", "读到哪、书签和视图参数会在关闭文档时自动记下。");
    }
    for (const std::size_t i : known) draw_reading_data_card(i);
    if (!unknown.empty()) draw_unknown_records(unknown);
    ImGui::EndChild();

    char count[64];
    std::snprintf(count, sizeof count, "共 %d 份文档", static_cast<int>(known.size()));
    ImGui::BeginDisabled(g_state.docs.empty());
    if (danger_text_button("清空全部")) {
        char body[176];
        std::snprintf(body, sizeof body,
                      "将删除全部 %d 条阅读数据（阅读位置与书签）。\n文档本身不受影响。",
                      static_cast<int>(g_state.docs.size()));
        request_confirm(ConfirmKind::ClearAll, "清空全部阅读数据？", body, {},
                        "清空", "取消", 0);
    }
    ImGui::EndDisabled();
    const float cw = ImGui::CalcTextSize(count).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - cw - ImGui::GetStyle().WindowPadding.x);
    ImGui::TextColored(col_dim(), "%s", count);
}

// ---- 设置窗口（分栏：界面 / 阅读 / 性能 / 按键） ----

void reset_prefs_to_default() {
    g_prefs = UiPrefs{};
    g_user_scale = g_prefs.ui_scale;
    g_apply_scale_pending = true;
    g_renderer->set_resource_tier(static_cast<lr::ResourceTier>(g_prefs.resource_tier));
    apply_gap_pref();
    save_prefs();
}

// ---- 设置项表格（左列标签 + 右列控件）----
// CellPadding 收窄：控件自身已有 FramePadding，行再留 6px 会显得空、且撑高窗口。
bool settings_rows_begin(const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(9.0f), px(3.0f)));
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable(id, 2, tf)) { ImGui::PopStyleVar(); return false; }
    ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, px(136.0f));
    ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
    return true;
}
void settings_rows_end() { ImGui::EndTable(); ImGui::PopStyleVar(); }
void settings_row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(px(190.0f));
}
void settings_note(const char* text) {
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim), "%s", text);
}

void draw_settings_interface_tab() {
    if (settings_rows_begin("##set_ui")) {
        settings_row("界面缩放");
        int pct = static_cast<int>(std::lround(g_prefs.ui_scale * 100.0f));
        if (ImGui::SliderInt("##uiscale", &pct, 80, 150, "%d%%")) {
            g_prefs.ui_scale = pct / 100.0f;
            g_user_scale = g_prefs.ui_scale;
            g_apply_scale_pending = true;   // 样式改动延到下一帧首（不在帧中途换样式）
            save_prefs();
        }
        settings_note("80% ~ 150%");

        settings_row("主题");
        const char* themes[] = { "跟随系统", "浅色", "深色" };
        if (ImGui::Combo("##theme", &g_prefs.theme, themes, IM_ARRAYSIZE(themes)))
            save_prefs();   // 下一帧 sync_theme 生效（不在帧中途改配色）

        settings_row("顶栏自动隐藏");
        if (ImGui::Checkbox("##autohide", &g_prefs.auto_hide_toolbar)) {
            save_prefs();
            g_toolbar_visible = true;
        }
        settings_note("阅读时收起，鼠标移到窗口顶部即重现");

        settings_row("界面动效");
        if (ImGui::Checkbox("##motion", &g_prefs.motion)) save_prefs();
        settings_note("页面淡入 / 滚动缩放平滑 / 顶栏与侧栏滑动 / 窗口淡入淡出");
        settings_rows_end();
    }
}

void draw_settings_reading_tab() {
    if (settings_rows_begin("##set_read")) {
        settings_row("页面间距");
        float gp = g_prefs.gap_percent;
        if (ImGui::SliderFloat("##gap", &gp, 0.0f, 6.0f, "%.1f%%")) {
            g_prefs.gap_percent = gp;
            apply_gap_pref();
            save_prefs();
        }
        settings_note("页与页之间的留白比例");

        settings_row("纸张方案");
        const char* schemes[] = { "原色", "深色", "暖色" };
        int cm = g_scheme;
        if (ImGui::Combo("##scheme", &cm, schemes, IM_ARRAYSIZE(schemes))) set_scheme(cm);
        settings_note("只改页面；照片与插图保持原色。快捷键见「按键」分栏");
        settings_rows_end();
    }
}

void draw_settings_performance_tab() {
    if (settings_rows_begin("##set_perf")) {
        settings_row("资源档位");
        const char* tiers[] = { "低", "中", "高" };
        if (ImGui::Combo("##resource_tier", &g_prefs.resource_tier, tiers, IM_ARRAYSIZE(tiers))) {
            g_renderer->set_resource_tier(static_cast<lr::ResourceTier>(g_prefs.resource_tier));
            save_prefs();
        }
        const lr::ResourceProfile profile = g_renderer->resource_profile();
        char note[96];
        std::snprintf(note, sizeof note, "缓存 %zu MB · tile %d px · worker %d",
                      profile.cache_bytes / (1024ull * 1024ull), profile.tile_size_px,
                      profile.render_workers);
        settings_note(note);
        settings_rows_end();
    }
}

// ---- 按键分栏（ADR-054）----
//
// 每条命令一行：命令名 + 两个按键框（主键 / 备键）。左键点框进入捕获（"按下按键…"），
// 按下的第一个键即写入该槽；右键清除（捕获中则取消）。同一键被多条命令使用会以警示色
// 标出并给出冲突对象——**只提示不阻止**：有人确实想让一个键在不同状态下做不同事。
//
// 修饰键型命令（kCmds[i].mod_only，"滚轮缩放"）**不是一条被按下的命令**，而是"按住不放的
// 键"，故不用捕获框，改用下拉框（无 / Ctrl / Alt / Shift / Ctrl+Alt）。
//
// 分栏末尾还有一节**鼠标与固定键**：这些操作改不了（不是命令），但用户必须能查到 ——
// 早期只列可自定义的命令，结果"Ctrl+滚轮能缩放""右键有菜单"这类事全靠猜（人工反馈）。

// 滚轮缩放修饰键的候选（值与 ini 里的存储串一一对应，见 chord_to_string）。
const char* const kModLabels[] = { "无", "Ctrl", "Alt", "Shift", "Ctrl+Alt" };
const ImGuiKeyChord kModValues[] = { 0, ImGuiMod_Ctrl, ImGuiMod_Alt, ImGuiMod_Shift,
                                     ImGuiMod_Ctrl | ImGuiMod_Alt };
constexpr int kModCount = static_cast<int>(IM_ARRAYSIZE(kModValues));

void draw_bind_button(int cmd, int slot) {
    const bool capturing = (g_capture_cmd == cmd && g_capture_slot == slot);
    const ImGuiKeyChord c = g_binds[cmd][slot];
    const std::string label = capturing ? std::string("按下按键…")
                                        : (c == ImGuiKey_None ? std::string("未设置")
                                                              : chord_label(c));
    ImGui::PushID(slot);
    if (capturing) {
        ImGui::PushStyleColor(ImGuiCol_Button, tone_apply(
            g_dark_theme ? ImVec4(0.42f, 0.65f, 0.94f, 0.35f)
                         : ImVec4(0.23f, 0.49f, 0.85f, 0.28f)));
    }
    const bool clicked = ImGui::Button(label.c_str(), ImVec2(-FLT_MIN, 0));
    if (capturing) ImGui::PopStyleColor();
    if (clicked) {
        g_capture_cmd = cmd;
        g_capture_slot = slot;
        g_capture_frame = ImGui::GetFrameCount();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        if (capturing) {
            g_capture_cmd = kCmdCount;          // 右键取消捕获
        } else if (c != ImGuiKey_None) {
            g_binds[cmd][slot] = ImGuiKey_None; // 右键清除
            save_binds();
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("左键点击后按下新键；右键清除 / 取消捕获");
    ImGui::PopID();
}

int conflict_of(int cmd) {
    const int c0 = find_bind_conflict(cmd, 0);
    return c0 >= 0 ? c0 : find_bind_conflict(cmd, 1);
}

void draw_settings_keys_tab() {
    const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim);
    if (ImGui::Button("恢复默认按键")) { reset_binds_to_default(); save_binds(); }
    ImGui::SameLine();
    if (g_capture_cmd < kCmdCount)
        ImGui::TextColored(col_warn(), "正在捕获按键…（点别处或右键取消）");
    else
        ImGui::TextColored(dim, "点按键框后按下新键；右键清除。下拉框直接选。");
    ImGui::Separator();

    ImGui::BeginChild("##keys_list", ImVec2(0, 0), false, ImGuiWindowFlags_NoNav);
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
    int i = 0;
    while (i < kCmdCount) {
        const char* group = kCmds[i].group;
        ImGui::SeparatorText(group);
        char tid[32];
        std::snprintf(tid, sizeof tid, "##keys_g%d", i);
        if (ImGui::BeginTable(tid, 3, tf)) {
            ImGui::TableSetupColumn("cmd", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("k0", ImGuiTableColumnFlags_WidthFixed, px(150.0f));
            ImGui::TableSetupColumn("k1", ImGuiTableColumnFlags_WidthFixed, px(150.0f));
            for (; i < kCmdCount && std::strcmp(kCmds[i].group, group) == 0; ++i) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                ImGui::AlignTextToFramePadding();
                const int conflict = conflict_of(i);
                if (conflict >= 0) ImGui::TextColored(col_warn(), "%s", kCmds[i].name);
                else               ImGui::TextUnformatted(kCmds[i].name);
                if (conflict >= 0 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("与「%s」使用了同一个键", kCmds[conflict].name);
                if (kCmds[i].mod_only) {
                    // 修饰键型：下拉框选"按住哪个键"，第二列留给说明文字（没有备键）
                    const ImGuiKeyChord cur = g_binds[i][0] & ImGuiMod_Mask_;
                    int sel = 0;
                    for (int o = 0; o < kModCount; ++o)
                        if ((kModValues[o] & ImGuiMod_Mask_) == cur) { sel = o; break; }
                    ImGui::TableSetColumnIndex(1);
                    ImGui::SetNextItemWidth(px(150.0f));
                    if (ImGui::Combo("##mod", &sel, kModLabels, kModCount)) {
                        g_binds[i][0] = kModValues[sel];
                        save_binds();
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                        ImGui::SetTooltip("按住此键滚动滚轮 = 缩放；不按则滚动页面");
                    ImGui::TableSetColumnIndex(2);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(dim, "%s", "按住 + 滚轮");
                } else {
                    for (int s = 0; s < kBindSlots; ++s) {
                        ImGui::TableSetColumnIndex(1 + s);
                        draw_bind_button(i, s);
                    }
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    // ---- 鼠标与固定键（**不可自定义**的操作清单）----
    // 用户查不到就会以为"没有这个功能"：滚轮缩放、右键菜单、滚条拖拽都属于这一类。
    ImGui::SeparatorText("鼠标与固定键");
    {
        const ImGuiTableFlags rf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
        if (ImGui::BeginTable("##keys_ref", 2, rf)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, px(190.0f));
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
            auto ref = [&](const char* key, const char* desc) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(key);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(dim, "%s", desc);
            };
            const std::string mod = chord_label(g_binds[static_cast<int>(Cmd::ZoomWheelMod)][0]);
            const std::string wheel_zoom =
                mod.empty() ? std::string("滚轮缩放：未设置修饰键（当前不可用）")
                            : ("按住 " + mod + " + 滚轮");
            ref("滚轮", "上下滚动");
            ref(wheel_zoom.c_str(), "以鼠标位置为中心缩放（修饰键可改，见上）");
            ref("左键拖拽", "在文字上 = 选择文本；在非文字处 = 平移页面");
            ref("中键拖拽", "平移页面（任何位置都可用）");
            ref("左键单击", "点链接即打开；点文字处取消选区");
            ref("右键", "打开画布菜单（Shift+F10 / 菜单键同效）");
            ref("拖拽右侧滚动条 / 点轨道", "定位到该处");
            ref("单击「渲染失败」占位", "重试渲染该页");
            ref("鼠标移到窗口顶部", "顶栏自动隐藏后重新显示");
            ref("Esc", "关闭当前对话框（设置 / 跳页 / 密码 / 确认）");
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

// 设置窗口本体：**底栏固定、正文装分栏**（内容多时只滚动正文）。
//
// 打开/关闭 = **缩放 + 淡入淡出**（开合对称，ADR-056），进度用**定时补间 + 三次缓出**
// （ToggleAnim，160ms，ADR-061）—— 比一阶滞后利落：时长确定、末端干脆，无长尾。
// 以窗口中心为不动点，尺寸 96% ↔ 100%，同时整体透明度 0 ↔ 1。**关闭不做"整窗幽灵化淡出"** —— 原实现只降 alpha，
// 结果是窗口原地保持原尺寸、内容逐渐透明，既不像常见窗口动画、又留一个半透明大窗占着画面。
// 现在边缩边淡，读起来是"收回去"。设置是普通 ImGui 窗口（非 popup），ImGui 自身不做动画。
//
// 几何：动画进行中每帧强制驱动（Cond_Always）；静止后不再覆盖，位置交给 ImGui 记忆，
// 这样用户仍可拖动窗口 —— 拖动后的位置由 g_settings_rest_pos 每帧实测记录，下次开合
// 即以该位置为中心缩放，不会"跳回屏幕中央"。
void draw_settings_window() {
    const float dt = ImGui::GetIO().DeltaTime;
    if (!g_settings_anim.step(dt, g_show_settings, g_prefs.motion))
        return;   // 完全关闭：不再创建窗口
    const float a = g_settings_anim.value;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 want = clamp_to_viewport(vp, ImVec2(px(600.0f), px(700.0f)));
    if (!g_settings_rest_valid) {
        g_settings_rest_pos = centered_on_viewport(vp, want);
        g_settings_rest_valid = true;
    }

    const bool animating = g_settings_anim.active;
    const float s = kWindowScaleFrom + (1.0f - kWindowScaleFrom) * a;
    const ImVec2 sz(want.x * s, want.y * s);
    const ImVec2 pos(g_settings_rest_pos.x + (want.x - sz.x) * 0.5f,
                     g_settings_rest_pos.y + (want.y - sz.y) * 0.5f);

    // 尺寸始终由我们驱动（NoResize，用户改不了）；位置仅在动画中强制，
    // 静止后用 Cond_Appearing —— 只在窗口出现那一帧落位，之后交给 ImGui（可拖动）。
    ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
    ImGui::SetNextWindowPos(pos, animating ? ImGuiCond_Always : ImGuiCond_Appearing);
    ImGui::SetNextWindowBgAlpha(a);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);

    bool keep = true;   // 每帧从 true 起算：真正的关闭由 g_show_settings 驱动，便于淡出
    if (ImGui::Begin("设置##settings", &keep,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings)) {
        if (!animating) {
            // 记住用户拖动后的位置。只在**真的移动过**（与基准差 > 1px）时更新，
            // 否则每次收尾那一帧的亚像素误差会被记成新基准、逐次累积漂移。
            const ImVec2 actual = ImGui::GetWindowPos();
            if (std::fabs(actual.x - g_settings_rest_pos.x) > 1.0f ||
                std::fabs(actual.y - g_settings_rest_pos.y) > 1.0f)
                g_settings_rest_pos = actual;
        }
        const int want_tab = g_settings_open_tab;   // F1 等入口指定的分栏，用完即复位
        g_settings_open_tab = -1;

        const float footer_h = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        ImGui::BeginChild("##settings_body", ImVec2(0, -footer_h), false, ImGuiWindowFlags_NoNav);
        if (ImGui::BeginTabBar("##settings_tabs")) {
            if (ImGui::BeginTabItem("界面", nullptr,
                                    want_tab == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
                draw_settings_interface_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("阅读", nullptr,
                                    want_tab == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                draw_settings_reading_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("性能", nullptr,
                                    want_tab == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
                draw_settings_performance_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("按键", nullptr,
                                    want_tab == 3 ? ImGuiTabItemFlags_SetSelected : 0)) {
                draw_settings_keys_tab();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("阅读数据", nullptr,
                                    want_tab == 4 ? ImGuiTabItemFlags_SetSelected : 0)) {
                draw_settings_reading_data_tab();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();

        // 底部操作条**固定在窗口底部**（不随内容滚动）：否则内容稍多时按钮会被挤出滚动区。
        ImGui::Separator();
        if (ImGui::Button("恢复默认设置")) reset_prefs_to_default();
        const float close_w = ImGui::CalcTextSize("关闭").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - close_w - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("关闭")) g_show_settings = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
    if (!keep) g_show_settings = false;
}

// ---------------- 顶层 UI ----------------

namespace {

// 是否有"对话框级"的界面开着（设置窗口 / 跳页 / 密码 / 任何弹出菜单）。
// 有则**不派发全局命令**：一来避免 Esc 之类"既关弹窗又触发命令"（Esc 现已是可绑定键），
// 二来对话框期间应用级快捷键本就不该抢输入。
bool any_dialog_open() {
    if (g_show_settings || g_open_jump || g_open_password) return true;
    return ImGui::IsPopupOpen(nullptr,
                              ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

}  // namespace

void draw_shell() {
    poll_document();
    g_renderer->drain_retired();  // 帧首：释放上一帧退役的纹理
    update_clipboard_results();   // 帧首：取走复制结果并写剪贴板（Phase 8）
    update_search();              // 帧首：取走检索的新命中 + 执行待办跳转（Phase 8）
    update_ime_association();     // 输入法关联随文本输入激活状态切换（ADR-028）
    if (g_apply_scale_pending) {  // 界面缩放改动：样式只在帧首换，绝不在一帧中途换
        g_apply_scale_pending = false;
        apply_ui_scale();
    }
    sync_theme(g_scheme);         // 主题 + 纸张方案 → chrome 明暗与暖化（含 ImGui 控件配色，帧首）
    update_toolbar_visibility();  // 顶栏自动隐藏

    // 全局命令：命令表驱动（ADR-054）。门 = 无文本输入 + 未在捕获按键 + 无对话框级界面。
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput && g_capture_cmd >= kCmdCount && !any_dialog_open())
            handle_global_commands();
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##shell", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);

    // ---- 三段外壳的显式布局（顶栏 / 画布[+侧栏] / 状态栏）----
    //
    // 为什么不让 ImGui 顺着排：三者是**同级子窗口**，`EndChild()` 会为每个子窗口记一个 item，
    // 于是相邻子窗口之间被插入一份 `ItemSpacing.y`。三段高度之和因此比客户区多出一个间距，
    // 状态栏被挤出窗口底部、文字下缘被裁 —— 实测截图里"第 95 / 361 页"的墨迹正好落在
    // 窗口最后一行（这才是"下方的字太靠近边缘"的真实原因，不是配色或内边距问题）。
    // 改为显式定位 + 显式高度后：三段**恰好铺满客户区**，中间不留缝，几何一眼可读（ADR-049）。
    const float dt = ImGui::GetIO().DeltaTime;
    const float client_h = ImGui::GetContentRegionAvail().y;
    const float status_h = px(kStatusBarH);
    const float top_h = update_top_bar_height(dt);
    if (top_h >= 1.0f) {   // 高度为 0 时不创建子窗口：BeginChild 把 0 当作"自动高度"
        ImGui::SetCursorPosY(0.0f);
        draw_top_bar(top_h);
    }
    const float body_h = std::max(px(60.0f), client_h - top_h - status_h);

    // 非阅读态把内容区铺成画布同色的底，与阅读态的视觉语言一致（否则是一大片窗口底色）。
    if (g_doc.kind != UiDoc::Kind::Reading) {
        ImGui::SetCursorPosY(top_h);
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p0, ImVec2(p0.x + avail.x, p0.y + avail.y), g_pal.backdrop);
    }

    switch (g_doc.kind) {
    case UiDoc::Kind::None:
        ImGui::SetCursorPosY(top_h);
        draw_drop_guide();
        break;
    case UiDoc::Kind::Opening:
        ImGui::SetCursorPosY(top_h);
        draw_opening();
        break;
    case UiDoc::Kind::Reading: {
        ImGui::SetCursorPosY(top_h);
        const float sidebar_w = update_sidebar_width(dt);   // 侧栏滑入/滑出（ADR-055）
        if (sidebar_w >= 1.0f) {
            draw_sidebar(body_h, sidebar_w);
            ImGui::SameLine(0.0f, 0.0f);
        }
        draw_canvas_area(body_h);
        ImGui::SetCursorPosY(client_h - status_h);
        draw_status_bar();
        break;
    }
    case UiDoc::Kind::Failed:
        ImGui::SetCursorPosY(top_h);
        draw_failed();
        break;
    case UiDoc::Kind::NeedsPassword:
        ImGui::SetCursorPosY(top_h);
        draw_failed();  // 背景铺失败页，密码框浮在其上
        break;
    case UiDoc::Kind::Rejected:
        ImGui::SetCursorPosY(top_h);
        draw_rejected();
        break;
    }
    ImGui::End();

    if (g_show_debug) draw_debug_overlay();
    draw_jump_popup();
    draw_password_popup();
    draw_confirm_popup();                                   // 智能匹配询问 / 删除二次确认
    draw_settings_window();
    update_key_capture();   // 在设置窗口绘制之后推进按键捕获（跳过"点按钮"那一帧的鼠标点击）

    // Esc：只关"对话框级"界面（密码 / 跳页 / 设置）。**不再关闭文档、不再退出** ——
    // 阅读中误按 Esc 就会丢掉当前阅读位置、得重新翻回去，代价太大（用户反馈）；
    // 关闭文档/退出一律走菜单（见 draw_main_menu_contents）。
    // 捕获按键时 Esc 交给捕获（Esc 已是可绑定键，默认与 F11 同为全屏）。
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && g_capture_cmd >= kCmdCount) {
        if (g_auth_pending) {
            // 认证请求在途，忽略 Esc，避免状态错乱
        } else if (g_open_password) {
            g_open_password = false;
            close_document();
        } else if (g_open_jump) {
            g_open_jump = false;
        } else if (g_show_settings) {
            g_show_settings = false;
        } else if (g_confirm_open) {
            // 确认弹窗上按 Esc = 次按钮（"取消"）；智能匹配的次按钮是"从头开始"，
            // 那不是 Esc 该替用户做的决定，故这里只按"沿用"（已经沿用过了）关掉弹窗。
            g_confirm_open = false;
            g_confirm_kind = ConfirmKind::None;
        }
    }
}

}  // namespace lr::app
