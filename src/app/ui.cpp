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
ImVec4 col_text()   { return g_dark_theme ? ImVec4(0.90f, 0.91f, 0.93f, 1.0f)
                                          : ImVec4(0.15f, 0.17f, 0.20f, 1.0f); }
ImVec4 col_dim()    { return g_dark_theme ? ImVec4(0.58f, 0.61f, 0.65f, 1.0f)
                                          : ImVec4(0.42f, 0.46f, 0.51f, 1.0f); }
ImVec4 col_warn()   { return g_dark_theme ? ImVec4(0.95f, 0.72f, 0.42f, 1.0f)
                                          : ImVec4(0.72f, 0.35f, 0.10f, 1.0f); }

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

    // 设置/帮助窗口打开时画布不再吞键盘（否则方向键会同时翻页与移动焦点）。
    if (!g_open_jump && !g_open_password && !g_show_settings && !g_show_help)
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
        if (s.texture != nullptr) {
            // 页面投影：右下偏移的半透明矩形，给纸面一点立体感
            const float sh = px(3.0f);
            dl->AddRectFilled(ImVec2(pmin.x + sh, pmin.y + sh),
                              ImVec2(pmax.x + sh, pmax.y + sh), g_pal.shadow, px(2.0f));
            // 淡入：纹理首次出现的那一帧从 0 渐显（kPageFadeSec），避免生硬跳出
            PageFade& pf = g_page_fade[static_cast<std::size_t>(i)];
            if (g_prefs.motion) {
                if (!pf.seen) { pf.seen = true; pf.alpha = 0.0f; }
                if (pf.alpha < 1.0f)
                    pf.alpha = std::min(1.0f, pf.alpha + dt / kPageFadeSec);
            } else {
                pf.alpha = 1.0f;
            }
            const int a = static_cast<int>(std::lround(pf.alpha * 255.0f));
            dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(s.texture)),
                         pmin, pmax, ImVec2(0, 0), ImVec2(1, 1),
                         IM_COL32(255, 255, 255, a));
            dl->AddRect(pmin, pmax, g_pal.page_border);
        } else {
            g_page_fade[static_cast<std::size_t>(i)].seen = false;
            g_page_fade[static_cast<std::size_t>(i)].alpha = 1.0f;
            draw_page_placeholder(dl, pmin, pmax, s);
        }
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
    if (g_color_mode == 1) chip("反色");
    else if (g_color_mode == 2) chip("护眼");
    if (current_page_has_bookmark()) chip("已加书签");

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
                        g_rotation, g_color_mode, g_canvas.state().spread ? 1 : 0);
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
        }
        if (g_doc.kind == UiDoc::Kind::Failed && !g_doc.detail_u8.empty())
            ImGui::TextDisabled("last_error: %.120s", g_doc.detail_u8.c_str());
    }
    ImGui::End();
}

// ---------------- 弹窗：跳页 ----------------

void draw_jump_popup() {
    if (!g_open_jump) return;
    if (!ImGui::IsPopupOpen("跳转页码")) ImGui::OpenPopup("跳转页码");
    if (ImGui::BeginPopupModal("跳转页码", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const int total = std::max(1, g_canvas.page_count());
        ImGui::Text("页码 (1 - %d)", total);
        ImGui::SetNextItemWidth(px(140));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputInt("##page", &g_jump_page, 0, 0,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
        const bool do_jump = enter || ImGui::Button("跳转");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("取消");
        if (do_jump) {
            int p = g_jump_page - 1;
            if (p < 0) p = 0;
            if (p >= g_canvas.page_count()) p = g_canvas.page_count() - 1;
            request_jump_scroll(p, 0.0f);
            g_open_jump = false;
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            g_open_jump = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
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

// 侧栏：高度与画布同高（由 draw_shell 统一给出，避免"三段 + 间距"溢出客户区）
void draw_sidebar(float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(8)));
    ImGui::BeginChild("##sidebar", ImVec2(px(kSidebarWidthPx), height), false,
                      ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();
    if (ImGui::BeginTabBar("##sidebar_tabs")) {
        if (ImGui::BeginTabItem("目录")) { g_sidebar_tab = 0; draw_outline_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("书签")) { g_sidebar_tab = 1; draw_bookmarks_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("缩略图")) { g_sidebar_tab = 2; draw_thumbnails_tab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
}

// ---------------- 弹窗：密码 ----------------

void draw_password_popup() {
    if (!g_open_password) {
        // 认证成功后需把上一个模态真正关掉，否则它会继续吞输入/绘制
        if (ImGui::IsPopupOpen("需要密码") &&
            ImGui::BeginPopupModal("需要密码", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        return;
    }
    if (!ImGui::IsPopupOpen("需要密码")) ImGui::OpenPopup("需要密码");
    if (ImGui::BeginPopupModal("需要密码", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("此文档已加密，请输入密码：");
        ImGui::TextDisabled("%s", g_doc.name_u8.c_str());
        ImGui::SetNextItemWidth(px(260));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        // 认证在途时禁用交互并提示"验证中…"，弹窗保持打开（不再关→开跳变）。
        ImGui::BeginDisabled(g_auth_pending);
        const bool enter = ImGui::InputText("##pwd", g_password_buf, sizeof g_password_buf,
                                            ImGuiInputTextFlags_Password |
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::EndDisabled();
        if (g_auth_pending)
            ImGui::TextDisabled("验证中…");
        else if (!g_password_error.empty())
            ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.35f, 1.0f), "%s", g_password_error.c_str());
        ImGui::BeginDisabled(g_auth_pending);
        const bool ok = enter || ImGui::Button("解锁");
        ImGui::SameLine();
        const bool cancel = ImGui::Button("取消");
        ImGui::EndDisabled();
        if (ok) {
            submit_password();
        } else if (cancel) {
            g_open_password = false;
            ImGui::CloseCurrentPopup();
            close_document();  // 取消即关闭该文档，回到引导页
        }
        ImGui::EndPopup();
    }
}

// ---------------- 界面外壳（顶栏 / 菜单 / 设置 / 帮助） ----------------

// 图标 + 文本（无图标字体时退化为纯文本）。
std::string with_icon(const char* icon, const char* text) {
    std::string s;
    if (g_icons_ok && icon && icon[0]) { s += icon; s += "  "; }
    s += text;
    return s;
}

// 菜单项：图标 + 文案 + 快捷键 + 勾选 / 可用状态。
bool menu_item(const char* icon, const char* label, const char* shortcut,
               bool checked = false, bool enabled = true) {
    const std::string s = with_icon(icon, label);
    return ImGui::MenuItem(s.c_str(), shortcut, checked, enabled);
}

// 扁平按钮配色（顶栏/工具条，ADR-045）：
// 默认**无底色**——否则一排按钮就是一排灰块，工具栏显脏；悬停/按下才浮现柔和底色。
// active（如"侧栏已开/设置已开"）用低透明强调色底表示状态，而不是加粗边框。
void push_flat_button(bool active) {
    const ImVec4 accent = g_dark_theme ? ImVec4(0.42f, 0.65f, 0.94f, 1.0f)
                                       : ImVec4(0.23f, 0.49f, 0.85f, 1.0f);
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
    if (menu_item(kIcExpand, "适合宽度", "F", g_canvas.state().fit_width))
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
    ImGui::MenuItem(with_icon(kIcList, "侧栏").c_str(), "O", &g_show_sidebar, rd);
    if (ImGui::BeginMenu(with_icon(kIcGrid, "列数").c_str(), rd)) {
        static const char* kNames[] = { "单页", "双页", "三页", "四页" };
        static const char* kKeys[]  = { "1", "2", "3", "4" };
        for (int c = 1; c <= 4; ++c) {
            const bool on = (!g_canvas.state().spread && g_canvas.state().columns == c);
            if (ImGui::MenuItem(kNames[c - 1], kKeys[c - 1], on)) g_canvas.set_columns(c);
        }
        ImGui::EndMenu();
    }
    const bool spread = g_canvas.state().spread;
    if (menu_item(kIcBook, "双页对开（书籍模式）", "D", spread, rd))
        g_canvas.set_spread(!spread);
    if (ImGui::BeginMenu(with_icon(kIcRotate, "旋转").c_str(), rd)) {
        const int degs[] = { 0, 90, 180, 270 };
        for (const int d : degs) {
            char lab[16];
            if (d == 0) std::snprintf(lab, sizeof lab, "不旋转");
            else        std::snprintf(lab, sizeof lab, "%d°", d);
            if (ImGui::MenuItem(lab, d == 90 ? "R" : nullptr, g_rotation == d))
                set_rotation(d);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(with_icon(kIcPalette, "配色").c_str(), rd)) {
        if (ImGui::MenuItem("正常", nullptr, g_color_mode == 0)) set_color_mode(0);
        if (ImGui::MenuItem("反色（深色）", "I", g_color_mode == 1)) set_color_mode(1);
        if (ImGui::MenuItem("护眼（暖色）", "E", g_color_mode == 2)) set_color_mode(2);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (menu_item(kIcFullscreen, "全屏", "F11", g_fullscreen)) toggle_fullscreen();
}

void draw_nav_menu_contents() {
    const bool rd = (g_doc.kind == UiDoc::Kind::Reading);
    if (menu_item(kIcHome, "首页", "Home", false, rd)) request_jump_scroll(0, 0.0f);
    if (menu_item(kIcPrev, "上一页 / 上一行", "←", false, rd)) scroll_by_rows(-1);
    if (menu_item(kIcNext, "下一页 / 下一行", "→", false, rd)) scroll_by_rows(+1);
    if (menu_item(kIcArrowDown, "末页", "End", false, rd))
        request_jump_scroll(g_canvas.page_count() - 1, 0.0f);
    ImGui::Separator();
    if (menu_item(kIcSearch, "跳转页码…", "G", false, rd)) open_jump_popup();
    if (menu_item(kIcStar, current_page_has_bookmark() ? "删除本页书签" : "添加本页书签", "B",
                  current_page_has_bookmark(), rd))
        toggle_bookmark_current();
}

void draw_main_menu_contents() {
    const bool has_doc = (g_doc.kind != UiDoc::Kind::None);
    const bool rd = (g_doc.kind == UiDoc::Kind::Reading);
    if (menu_item(kIcOpenFile, "打开文档…", "Ctrl+O")) g_request_open_dialog = true;
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
    if (menu_item(kIcSettings, "设置…", "Ctrl+,")) g_show_settings = true;
    if (menu_item(kIcHelp, "快捷键与帮助", "F1")) g_show_help = true;
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
        if (menu_item(kIcPrev, "上一页", "←")) scroll_by_rows(-1);
        if (menu_item(kIcNext, "下一页", "→")) scroll_by_rows(+1);
        ImGui::Separator();
        if (ImGui::BeginMenu(with_icon(kIcExpand, "缩放").c_str())) {
            draw_zoom_menu_contents(); ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(with_icon(kIcDoc, "视图").c_str())) {
            draw_view_menu_contents(); ImGui::EndMenu();
        }
        ImGui::Separator();
        if (menu_item(kIcSearch, "跳转页码…", "G")) open_jump_popup();
        if (menu_item(kIcStar, current_page_has_bookmark() ? "删除本页书签" : "添加本页书签", "B",
                      current_page_has_bookmark()))
            toggle_bookmark_current();
        if (menu_item(kIcList, "侧栏", "O", g_show_sidebar)) set_sidebar(!g_show_sidebar, 0);
        ImGui::Separator();
        if (menu_item(kIcSettings, "设置…", "Ctrl+,")) g_show_settings = true;
        if (menu_item(kIcHelp, "快捷键与帮助", "F1")) g_show_help = true;
        ImGui::EndPopup();
    }
    pop_popup_style();
}

// ---- 顶栏自动隐藏 ----
// 阅读态下，鼠标离开窗口顶部一段时间就收起顶栏（沉浸阅读）；移到顶部即重现。
// 引导/失败/密码等状态、以及打开菜单/设置/帮助时始终显示（否则用户找不到入口）。
void update_toolbar_visibility() {
    if (g_doc.kind != UiDoc::Kind::Reading || !g_prefs.auto_hide_toolbar) {
        g_toolbar_visible = true;
        g_toolbar_idle_since = -1.0;
        return;
    }
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const ImVec2 vp = ImGui::GetMainViewport()->Pos;
    const bool near_top = (mp.y - vp.y) <= px(kTopBarH + kToolbarRevealBandPx);
    if (near_top || g_show_settings || g_show_help) {
        g_toolbar_visible = true;
        g_toolbar_idle_since = -1.0;
        return;
    }
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
    if (g_show_settings || g_show_help || g_open_jump || g_open_password) return true;
    if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return true;
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
    if (tool_button("##mainmenu", kIcMenu, "菜单", nullptr))
        ImGui::OpenPopup("##mainmenu_pop");
    push_popup_style();
    if (ImGui::BeginPopup("##mainmenu_pop")) { draw_main_menu_contents(); ImGui::EndPopup(); }
    pop_popup_style();

    if (rd) {
        ImGui::SameLine();
        if (tool_button("##sidebar", kIcPane, "侧栏", "侧栏 (O)", g_show_sidebar))
            set_sidebar(!g_show_sidebar, 0);
    }

    // 右侧控件簇宽度（先算宽度，标题才能安全居中且不与它重叠）
    const std::string ztxt = std::to_string(zoom_percent()) + "%";
    const float zw = ImGui::CalcTextSize(ztxt.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    const float cluster = rd ? (h * 5.0f + zw + gap * 6.0f + px(8.0f))
                             : (h * 3.0f + gap * 3.0f + px(8.0f));

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
        if (ImGui::BeginPopup("##zoompop")) { draw_zoom_menu_contents(); ImGui::EndPopup(); }
        pop_popup_style();
        ImGui::SameLine();
        if (tool_button("##zin", kIcZoomIn, "+", "放大"))
            zoom_at_center(kZoomStep);
        ImGui::SameLine();
    } else {
        if (tool_button("##open", kIcOpenFile, "打开…", "打开文档 (Ctrl+O)"))
            g_request_open_dialog = true;
        ImGui::SameLine();
    }
    if (tool_button("##settings", kIcSettings, "设置", "设置 (Ctrl+,)", g_show_settings))
        g_show_settings = true;
    ImGui::SameLine();
    if (tool_button("##help", kIcHelp, "帮助", "快捷键与帮助 (F1)", g_show_help))
        g_show_help = true;

    ImGui::EndChild();
}

// ---- 设置窗口 ----

void reset_prefs_to_default() {
    g_prefs = UiPrefs{};
    g_user_scale = g_prefs.ui_scale;
    g_apply_scale_pending = true;
    g_renderer->set_cache_budget(static_cast<std::size_t>(g_prefs.cache_mb) * 1024ull * 1024ull);
    apply_gap_pref();
    save_prefs();
}

// 浮动窗口（设置/帮助/调试）的默认尺寸与位置：期望值按视口上限钳制后居中，
// 小屏/低分辨率下也不会超出屏幕。
ImVec2 clamp_to_viewport(const ImGuiViewport* vp, const ImVec2& want) {
    return ImVec2(std::min(want.x, vp->Size.x * 0.92f),
                  std::min(want.y, vp->Size.y * 0.90f));
}
ImVec2 centered_on_viewport(const ImGuiViewport* vp, const ImVec2& size) {
    return ImVec2(vp->Pos.x + (vp->Size.x - size.x) * 0.5f,
                  vp->Pos.y + (vp->Size.y - size.y) * 0.5f);
}

void draw_settings_window() {
    if (!g_show_settings) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 want = clamp_to_viewport(vp, ImVec2(px(560.0f), px(690.0f)));
    ImGui::SetNextWindowSize(want, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(centered_on_viewport(vp, want), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("设置##settings", &open, ImGuiWindowFlags_NoCollapse)) {
        const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim);

        // 正文放进可视区（高度 = 窗口高 − 底栏），底栏固定，内容多时只滚动正文。
        const float footer_h = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        ImGui::BeginChild("##settings_body", ImVec2(0, -footer_h), false,
                          ImGuiWindowFlags_NoNav);
        // 设置项表格的纵向内边距收窄：控件本身已有 FramePadding，行再留 6px 会显得空、且撑高窗口。
        ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(9.0f), px(3.0f)));

        const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
        auto begin_rows = [&](const char* id) {
            if (!ImGui::BeginTable(id, 2, tf)) return false;
            ImGui::TableSetupColumn("l", ImGuiTableColumnFlags_WidthFixed, px(136.0f));
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
            return true;
        };
        auto row = [](const char* label) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(px(190.0f));
        };
        auto note = [&](const char* text) {
            ImGui::SameLine();
            ImGui::TextColored(dim, "%s", text);
        };

        ImGui::SeparatorText("界面");
        if (begin_rows("##set_ui")) {
            row("界面缩放");
            int pct = static_cast<int>(std::lround(g_prefs.ui_scale * 100.0f));
            if (ImGui::SliderInt("##uiscale", &pct, 80, 150, "%d%%")) {
                g_prefs.ui_scale = pct / 100.0f;
                g_user_scale = g_prefs.ui_scale;
                g_apply_scale_pending = true;   // 样式改动延到下一帧首（不在帧中途换样式）
                save_prefs();
            }
            note("80% ~ 150%");

            row("主题");
            const char* themes[] = { "跟随系统", "浅色", "深色" };
            if (ImGui::Combo("##theme", &g_prefs.theme, themes, IM_ARRAYSIZE(themes)))
                save_prefs();   // 下一帧 sync_theme 生效（不在帧中途改配色）

            row("顶栏自动隐藏");
            if (ImGui::Checkbox("##autohide", &g_prefs.auto_hide_toolbar)) {
                save_prefs();
                g_toolbar_visible = true;
            }
            note("阅读时收起，鼠标移到窗口顶部即重现");

            row("界面动效");
            if (ImGui::Checkbox("##motion", &g_prefs.motion)) save_prefs();
            note("页面淡入、滚动条渐隐");
            ImGui::EndTable();
        }

        ImGui::SeparatorText("阅读");
        if (begin_rows("##set_read")) {
            row("页面间距");
            float gp = g_prefs.gap_percent;
            if (ImGui::SliderFloat("##gap", &gp, 0.0f, 6.0f, "%.1f%%")) {
                g_prefs.gap_percent = gp;
                apply_gap_pref();
                save_prefs();
            }
            note("页与页之间的留白比例");

            row("当前配色");
            const char* colors[] = { "正常", "反色", "护眼" };
            int cm = g_color_mode;
            if (ImGui::Combo("##color", &cm, colors, IM_ARRAYSIZE(colors))) set_color_mode(cm);
            note("快捷键 I / E");
            ImGui::EndTable();
        }

        ImGui::SeparatorText("性能");
        if (begin_rows("##set_perf")) {
            row("页缓存预算");
            if (ImGui::SliderInt("##cache", &g_prefs.cache_mb, 128, 2048, "%d MB")) {
                g_renderer->set_cache_budget(
                    static_cast<std::size_t>(g_prefs.cache_mb) * 1024ull * 1024ull);
                save_prefs();
            }
            note("128 ~ 2048 MB");
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::TextColored(dim, "设置即时保存到 LilithReader.ini（exe 同目录）。");
        ImGui::PopStyleVar();   // CellPadding
        ImGui::EndChild();

        // 底部操作条**固定在窗口底部**（不随内容滚动）：否则内容稍多时「恢复默认值/关闭」
        // 会被挤到滚动区外，用户得先滚动才能关闭窗口。
        ImGui::Separator();
        if (ImGui::Button("恢复默认值")) reset_prefs_to_default();
        const float close_w = ImGui::CalcTextSize("关闭").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(ImGui::GetWindowWidth() - close_w - ImGui::GetStyle().WindowPadding.x);
        if (ImGui::Button("关闭")) g_show_settings = false;
    }
    ImGui::End();
    if (!open) g_show_settings = false;
}

// ---- 帮助 / 快捷键窗口 ----
struct ShortcutRow { const char* desc; const char* keys; };

void shortcut_table(const char* id, const ShortcutRow* rows, int count) {
    const ImGuiTableFlags tf = ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_PadOuterX;
    if (!ImGui::BeginTable(id, 2, tf)) return;
    ImGui::TableSetupColumn("功能", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("按键", ImGuiTableColumnFlags_WidthFixed, px(140.0f));
    for (int i = 0; i < count; ++i) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(rows[i].desc);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim), "%s", rows[i].keys);
    }
    ImGui::EndTable();
}

void draw_help_window() {
    if (!g_show_help) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImVec2 want = clamp_to_viewport(vp, ImVec2(px(600.0f), px(720.0f)));
    ImGui::SetNextWindowSize(want, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(centered_on_viewport(vp, want), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("快捷键与帮助##help", &open, ImGuiWindowFlags_NoCollapse)) {
        const ImVec4 accent = ImGui::ColorConvertU32ToFloat4(g_pal.accent);
        const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(g_pal.chrome_dim);
        ImGui::TextColored(accent, "Lilith Reader");
        ImGui::TextColored(dim, "单文件 · 零外部依赖 · 专注阅读");
        ImGui::Spacing();

        ImGui::SeparatorText("导航");
        static const ShortcutRow kNav[] = {
            { "下一行 / 下一页",   "→" },
            { "上一行 / 上一页",   "←" },
            { "小步滚动",          "↑ / ↓" },
            { "整屏翻动",          "PgUp / PgDn" },
            { "首页 / 末页",       "Home / End" },
            { "跳转到指定页",      "G" },
        };
        shortcut_table("##sc_nav", kNav, IM_ARRAYSIZE(kNav));

        ImGui::SeparatorText("缩放");
        static const ShortcutRow kZoom[] = {
            { "放大 / 缩小",       "+ / −" },
            { "以鼠标为中心缩放",  "Ctrl + 滚轮" },
            { "适合宽度",          "F / Ctrl+0" },
        };
        shortcut_table("##sc_zoom", kZoom, IM_ARRAYSIZE(kZoom));

        ImGui::SeparatorText("显示");
        static const ShortcutRow kView[] = {
            { "列数（单 / 双 / 三 / 四）", "1 / 2 / 3 / 4" },
            { "双页对开（书籍模式）",      "D" },
            { "旋转 90°",                  "R" },
            { "反色 / 护眼",               "I / E" },
            { "全屏",                      "F11" },
        };
        shortcut_table("##sc_view", kView, IM_ARRAYSIZE(kView));

        ImGui::SeparatorText("界面");
        static const ShortcutRow kUi[] = {
            { "侧栏（目录 / 书签 / 缩略图）", "O" },
            { "当前页书签增删",               "B" },
            { "设置",                         "Ctrl+," },
            { "本帮助",                       "F1" },
            { "调试浮层",                     "F3" },
            { "关闭文档 / 退出",              "Esc" },
        };
        shortcut_table("##sc_ui", kUi, IM_ARRAYSIZE(kUi));

        ImGui::SeparatorText("鼠标");
        static const ShortcutRow kMouse[] = {
            { "滚动页面",           "滚轮" },
            { "以鼠标为中心缩放",   "Ctrl + 滚轮" },
            { "平移页面",           "左键拖拽" },
            { "全部命令（右键菜单）", "在页面上右键 / Shift+F10" },
        };
        shortcut_table("##sc_mouse", kMouse, IM_ARRAYSIZE(kMouse));

        ImGui::SeparatorText("支持格式");
        ImGui::TextWrapped("%s", kFormatsLine);
        ImGui::TextColored(dim, "识别以内容为准；扩展名不符会如实标注。");
        ImGui::Spacing();
        ImGui::TextColored(dim, "按 F1 或 Esc 关闭本窗口。");
    }
    ImGui::End();
    if (!open) g_show_help = false;
}

// ---------------- 顶层 UI ----------------

void draw_shell() {
    poll_document();
    g_renderer->drain_retired();  // 帧首：释放上一帧退役的纹理
    update_ime_association();     // 输入法关联随文本输入激活状态切换（ADR-028）
    if (g_apply_scale_pending) {  // 界面缩放改动：样式只在帧首换，绝不在一帧中途换
        g_apply_scale_pending = false;
        apply_ui_scale();
    }
    sync_theme(g_color_mode);     // 主题/反色 → chrome 明暗（含 ImGui 控件配色，也在帧首）
    update_toolbar_visibility();  // 顶栏自动隐藏

    // 全局快捷键：任何状态下都可用；有文本输入时让位。
    {
        ImGuiIO& io = ImGui::GetIO();
        if (!io.WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) g_show_debug ^= 1;
            if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) g_show_help ^= 1;
            if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) toggle_fullscreen();
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_O, false)) g_request_open_dialog = true;
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Comma, false)) g_show_settings ^= 1;
            // 画布右键菜单的键盘等价入口（Shift+F10 或「菜单」键），标准 Windows 习惯
            if (g_doc.kind == UiDoc::Kind::Reading &&
                ((io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_F10, false)) ||
                 ImGui::IsKeyPressed(ImGuiKey_Menu, false)))
                g_open_canvas_ctx = true;
        }
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
    case UiDoc::Kind::Reading:
        ImGui::SetCursorPosY(top_h);
        if (g_show_sidebar) {
            draw_sidebar(body_h);
            ImGui::SameLine(0.0f, 0.0f);
        }
        draw_canvas_area(body_h);
        ImGui::SetCursorPosY(client_h - status_h);
        draw_status_bar();
        break;
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
    draw_settings_window();
    draw_help_window();

    // Esc：密码框 → 关闭并放弃文档；跳页弹窗/设置/帮助 → 关窗；有文档 → 关闭返回引导页；
    //      无文档 → 退出
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g_auth_pending) {
            // 认证请求在途，忽略 Esc，避免状态错乱
        } else if (g_open_password) {
            g_open_password = false;
            close_document();
        } else if (g_open_jump) {
            g_open_jump = false;
        } else if (g_show_settings) {
            g_show_settings = false;
        } else if (g_show_help) {
            g_show_help = false;
        } else if (g_doc.kind != UiDoc::Kind::None) {
            close_document();
        } else {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
    }
}

}  // namespace lr::app