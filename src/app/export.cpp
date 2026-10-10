// export.cpp — Lilith Reader 应用层：导出为图片
//
// 自 ui.cpp 拆出的独立 TU（与 settings_ui.cpp 同一手法，ADR-094）：导出对话框、输出选择
// （另存为 / 选目录）、后台导出任务的编排与进度显示。只经 app_internal.h 的共享声明互调，
// 与 ui.cpp 互不 include。
//
// 线程纪律：本文件运行在 **UI 线程**，零 fz_*、零阻塞。真正的渲染与编码在 render 工作线程
// （Renderer::start_export → document::save_page_as_image），本文件只投递请求、轮询进度
// （lr::ExportStatus）。
//
// 模态对话框（另存为 / 选目录）自带消息循环，**绝不能**在 ImGui 一帧中途调用 —— 故「导出」
// 按钮只置 g_app.request_export_pick，由主循环在帧与帧之间调用 export_pick_output_now()。
//
// 编码格式：PNG（无损）/ JPEG（有损，质量可调）—— 均由 MuPDF 内置编码器提供，无新依赖。
// MuPDF 还能写 JP2/PNM/PSD 等，但面向用户无意义或过于专业，故不开放。

#include "app_internal.h"

#include "imgui_raii.h"

#include <algorithm>
#include <cfloat>      // FLT_MAX / FLT_MIN（尺寸约束、撑满宽度）
#include <cwchar>
#include <shlobj.h>    // SHBrowseForFolderW / SHGetPathFromIDListW

#pragma comment(lib, "shell32.lib")   // SHBrowseForFolderW（imgui.cpp 也已链接，此处显式声明意图）
#pragma comment(lib, "comdlg32.lib")  // GetSaveFileNameW

namespace lr::app {
namespace {

// 导出对话框的开合动效（与跳页 / 密码弹窗同一手法：淡入淡出 + 轻微滑落，ADR-059）。
ToggleAnim g_export_anim;

// 透明度下限（理由同 ui.cpp 的 kPopupMinAlpha）：ImGui 在 Alpha<=0 时把窗口判为 Hidden，
// Begin() 返回 false，"收敛后再关闭"那一步就永远执行不到 —— 弹窗会作为不可见模态永久残留、
// 挡住全部输入。留 1/255 肉眼不可见，却让收敛帧仍是真实帧。
constexpr float kExportPopupMinAlpha = 1.0f / 255.0f;

// DPI 快捷档位。72 = 原始尺寸（1 点 = 1 像素）。
constexpr int kDpiPresets[] = { 72, 96, 150, 300, 600 };

// 对话框**固定宽度**（px，100% 缩放值）。宽度钉死、高度随内容自适应：否则内容一多，
// AlwaysAutoResize 会按最宽一行去挤，窄控件被压扁、整窗忽宽忽窄（人工反馈的观感问题）。
constexpr float kExportDialogWidth = 440.0f;

// 模态弹窗的**稳定 ID**（"###" 之后才算 ID）。标题要随格式变化（「导出为 PNG 图片」/
// 「导出为 JPEG 图片」），但 ImGui 的弹窗 ID 绝不能跟着变 —— 否则 OpenPopup 与
// IsPopupOpen 对不上，淡入淡出期间弹窗状态就断了。故标题尾部固定挂这个 ID。
constexpr const char* kExportModalId = "###export_modal";

// Windows 文件名非法字符 → '_'；并去掉首尾空白与结尾的点（Windows 会吞掉结尾的点 / 空格）。
std::wstring sanitize_file_stem(std::wstring s) {
    for (wchar_t& c : s) {
        if (c < 0x20 || c == L'\\' || c == L'/' || c == L':' || c == L'*' ||
            c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|')
            c = L'_';
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    std::size_t i = 0;
    while (i < s.size() && s[i] == L' ') ++i;
    if (i > 0) s.erase(0, i);
    return s;
}

// 当前文档名（去目录、去扩展名）→ 安全文件名主干。
std::wstring doc_base_name() {
    std::wstring name = lr::file_name_of(g_app.session.doc.path_w);
    const std::size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name.resize(dot);
    name = sanitize_file_stem(std::move(name));
    if (name.empty()) name = L"page";
    return name;
}

const wchar_t* ext_of_format(int format) { return format == 1 ? L"jpg" : L"png"; }

const wchar_t* filter_of_format(int format) {
    return format == 1
               ? L"JPEG 图片\0*.jpg;*.jpeg\0所有文件\0*.*\0"
               : L"PNG 图片\0*.png\0所有文件\0*.*\0";
}

// ---- 对话框配色辅助（与顶栏扁平按钮同源，过一遍纸张色调，ADR-068）----
// 强调色 accent_color() 定义在 ui.cpp、声明在 app_internal.h（两处共用一份，避免漂移）。

// 主操作按钮（「导出」）：强调色实底 + 高对比文字，一眼看出这是默认动作。
[[nodiscard]] ig::StyleColor4 primary_button_style() {
    const ImVec4 a = accent_color();
    const ImVec4 ink = g_app.dark_theme ? ImVec4(0.05f, 0.07f, 0.10f, 1.0f)
                                        : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    return ig::StyleColor4(ImGuiCol_Button,        ImVec4(a.x, a.y, a.z, 0.90f),
                           ImGuiCol_ButtonHovered, ImVec4(a.x, a.y, a.z, 1.00f),
                           ImGuiCol_ButtonActive,  ImVec4(a.x, a.y, a.z, 0.74f),
                           ImGuiCol_Text,          ink);
}

// 档位按钮（DPI 快捷档）：选中 = 强调色浅底 + 强调色文字；未选中 = 极浅底色 + 常规文字。
// 未选中**留一点底色**（而不是全透明）：否则一排数字看着像普通文本、认不出是可点的档位。
bool preset_button(const char* label, bool active, float width) {
    const ImVec4 a = accent_color();
    const ImVec4 ink = g_app.dark_theme ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                        : ImVec4(0.0f, 0.0f, 0.0f, 1.0f);
    const float idle_a = g_app.dark_theme ? 0.07f : 0.05f;
    const float hov_a  = g_app.dark_theme ? 0.14f : 0.08f;
    const float act_a  = g_app.dark_theme ? 0.20f : 0.12f;
    const ig::StyleColor4 c(
        ImGuiCol_Button,        active ? ImVec4(a.x, a.y, a.z, 0.22f)
                                       : ImVec4(ink.x, ink.y, ink.z, idle_a),
        ImGuiCol_ButtonHovered, active ? ImVec4(a.x, a.y, a.z, 0.32f)
                                       : ImVec4(ink.x, ink.y, ink.z, hov_a),
        ImGuiCol_ButtonActive,  active ? ImVec4(a.x, a.y, a.z, 0.44f)
                                       : ImVec4(ink.x, ink.y, ink.z, act_a),
        ImGuiCol_Text,          active ? a : ink);
    return ImGui::Button(label, ImVec2(width, 0.0f));
}

// 一行小字提示（自动折行，避免窄窗里溢出）。
void hint_text(const char* text) {
    const ig::TextWrapPos wrap(0.0f);
    ImGui::TextDisabled("%s", text);
}

// 计算待导出页（0 基），按范围选项与总页数钳制。
std::vector<int> compute_pages() {
    std::vector<int> pages;
    const int total = g_app.canvas.page_count();
    if (total <= 0) return pages;
    switch (g_app.export_scope) {
    case 1: {   // 全部页
        pages.reserve(static_cast<std::size_t>(total));
        for (int i = 0; i < total; ++i) pages.push_back(i);
        break;
    }
    case 2: {   // 自定义范围（1 基，含两端）
        int a = g_app.export_from;
        int b = g_app.export_to;
        if (a > b) std::swap(a, b);
        if (a < 1) a = 1;
        if (b > total) b = total;
        for (int i = a - 1; i <= b - 1 && i < total; ++i) pages.push_back(i);
        break;
    }
    case 0:     // 当前页
    default: {
        int cur = g_app.canvas.current_page();
        if (cur < 0 || cur >= total) cur = 0;
        pages.push_back(cur);
        break;
    }
    }
    return pages;
}

}  // namespace

// 打开导出对话框：复位自定义范围的默认值（起 = 当前页、止 = 总页数）；范围档位
// （当前页 / 全部页 / 自定义）与格式、DPI、质量等选项**沿用上次选择**，首次使用即默认值。
void open_export_dialog() {
    if (g_app.session.doc.kind != UiDoc::Kind::Reading) return;
    const int total = std::max(1, g_app.canvas.page_count());
    const int cur = std::clamp(g_app.canvas.current_page() + 1, 1, total);
    g_app.export_from = cur;
    g_app.export_to = total;
    g_app.export_open = true;
}

// 帧间调用：输出选择（单页 → 另存为文件；多页 → 选目录）。
// 解析成功 → 置 export_out_ready，待导出页列表**保留**给下一帧的 update_export 投递；
// 取消 → 丢弃本次已算好的页列表（下次复用会重新计算），不留过期状态。
void export_pick_output_now() {
    const std::wstring stem = doc_base_name();
    const bool single = (g_app.export_pages.size() == 1);

    if (single) {
        const int page1 = g_app.export_pages.empty() ? 1 : g_app.export_pages[0] + 1;
        std::wstring buf(4096, L'\0');
        std::swprintf(buf.data(), buf.size(), L"%s_p%04d.%s", stem.c_str(), page1,
                      ext_of_format(g_app.export_format));

        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_app.hwnd;
        ofn.lpstrFilter = filter_of_format(g_app.export_format);
        ofn.lpstrFile = buf.data();
        ofn.nMaxFile = static_cast<DWORD>(buf.size());
        ofn.lpstrTitle = L"导出为图片";
        ofn.lpstrDefExt = ext_of_format(g_app.export_format);
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&ofn)) {
            g_app.export_out_path.assign(buf.c_str());
            g_app.export_out_single = true;
            g_app.export_out_ready = true;
        } else {
            g_app.export_pages.clear();
        }
        return;
    }

    // 多页：选输出目录。
    //
    // 用经典的 SHBrowseForFolderW（不要求 COM 初始化，本项目未调用 CoInitialize；
    // 新版 IFileDialog/BIF_NEWDIALOGSTYLE 需要 OLE，故刻意不用）。
    BROWSEINFOW bi{};
    bi.hwndOwner = g_app.hwnd;
    bi.lpszTitle = L"选择导出图片的输出文件夹（每页一个文件）";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_EDITBOX;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl == nullptr) { g_app.export_pages.clear(); return; }
    wchar_t dir[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListW(pidl, dir) != FALSE;
    ILFree(pidl);   // shell32 分配，用 ILFree 释放（避免额外依赖 ole32）
    if (!ok || dir[0] == L'\0') { g_app.export_pages.clear(); return; }
    g_app.export_out_dir.assign(dir);
    g_app.export_out_single = false;
    g_app.export_out_ready = true;
}

// 帧首调用：投递已解析的输出目标；轮询导出进度并提示完成。
void update_export() {
    if (!g_app.renderer) return;

    // 1) 有已解析的输出目标 → 组装并投递导出作业。
    if (g_app.export_out_ready) {
        g_app.export_out_ready = false;
        if (!g_app.export_pages.empty()) {
            lr::ExportRequest req;
            req.pages = g_app.export_pages;
            req.scale = static_cast<float>(g_app.export_dpi) / 72.0f;
            req.format = g_app.export_format;
            req.quality = g_app.export_quality;
            req.rotation_deg = g_app.export_apply_view ? g_app.session.rotation : 0;
            req.scheme = g_app.export_apply_view ? g_app.session.scheme : 0;
            // 单边像素上限沿用 ExportRequest 的默认值（唯一出处在其字段注释），此处不重复设。
            req.single_file = g_app.export_out_single;
            req.out_dir = g_app.export_out_single ? g_app.export_out_path : g_app.export_out_dir;
            req.base_name = lr::wide_to_utf8(doc_base_name());
            g_app.renderer->start_export(std::move(req));
            show_toast("开始导出…");
        }
        g_app.export_pages.clear();
    }

    // 2) 轮询进度：进行中时用状态栏提示持续刷新进度（toast 每次刷新即续期）。
    const lr::ExportStatus st = g_app.renderer->export_status();
    g_app.export_busy = st.active;
    g_app.export_done = st.done;
    g_app.export_total = st.total;
    if (st.active) {
        char buf[128] = {};
        std::snprintf(buf, sizeof buf, "正在导出 %d / %d 页…", st.done, st.total);
        show_toast(buf);
        return;
    }

    // 3) 完成（成功 / 失败 / 取消）：只提示一次，然后清除结果。
    if (st.finished && st.id != 0 && st.id != g_app.export_seen_id) {
        g_app.export_seen_id = st.id;
        char buf[256] = {};
        if (st.cancelled) {
            std::snprintf(buf, sizeof buf, "已取消导出（完成 %d 页）", st.done - st.failed);
        } else if (st.failed > 0) {
            std::snprintf(buf, sizeof buf, "导出完成：成功 %d 页，失败 %d 页（%s）",
                          st.done - st.failed, st.failed,
                          st.error_text.empty() ? "见日志" : st.error_text.c_str());
        } else {
            std::snprintf(buf, sizeof buf, "已导出 %d 页", st.done);
        }
        show_toast(buf);
        g_app.renderer->clear_export_result();
    }
}

// 绘制导出对话框（draw_shell 调用）。
void draw_export_popup() {
    const float dt = ImGui::GetIO().DeltaTime;
    const bool alive = g_export_anim.step(dt, g_app.export_open, g_app.prefs.motion);
    const bool is_open = ImGui::IsPopupOpen(kExportModalId);
    if (!is_open && !g_app.export_open) return;
    if (!is_open) ImGui::OpenPopup(kExportModalId);

    // 始终套用动画样式：收敛那一帧 value 已是 0，必须仍为全透明再销毁（否则关闭时闪一下）。
    const float alpha =
        g_export_anim.value < kExportPopupMinAlpha ? kExportPopupMinAlpha : g_export_anim.value;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f,
                                   vp->Pos.y + vp->Size.y * 0.5f +
                                       (1.0f - g_export_anim.value) * px(kPopupSlidePx)),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(alpha);
    // 宽度钉死、高度自适应（AlwaysAutoResize 会把约束套用到自动尺寸上，见 imgui.cpp）。
    ImGui::SetNextWindowSizeConstraints(ImVec2(px(kExportDialogWidth), 0.0f),
                                        ImVec2(px(kExportDialogWidth), FLT_MAX));

    const ig::StyleVar anim_style(ImGuiStyleVar_Alpha, alpha);
    // 更宽松的内边距 + 项距：默认的紧凑间距正是"看着挤"的根源之一。
    const ig::StyleVar2 dlg_spacing(ImGuiStyleVar_WindowPadding, ImVec2(px(16.0f), px(14.0f)),
                                    ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), px(9.0f)));

    const int total = std::max(1, g_app.canvas.page_count());
    // 标题写明格式（菜单入口已定），弹窗 ID 用尾部 ### 固定住。
    const std::string modal_title =
        std::string("导出为 ") + (g_app.export_format == 1 ? "JPEG 图片（有损）" : "PNG 图片（无损）") +
        kExportModalId;
    if (const ig::PopupModal modal =
            ig::PopupModal(modal_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        // 淡出中不再响应，避免重复触发（与跳页弹窗同一手法）。
        const ig::Disabled anim_lock(!g_app.export_open);

        const ImGuiStyle& st = ImGui::GetStyle();
        const float right_edge = ImGui::GetWindowWidth() - st.WindowPadding.x;   // 右对齐基准

        // ---- 范围 ----
        ImGui::SeparatorText("范围");
        ImGui::RadioButton("当前页", &g_app.export_scope, 0);
        ImGui::SameLine();
        ImGui::RadioButton("全部页", &g_app.export_scope, 1);
        ImGui::SameLine();
        ImGui::RadioButton("自定义", &g_app.export_scope, 2);
        {
            const ig::Disabled dis(g_app.export_scope != 2);
            // 去掉数字框的 +/- 微调按钮：它们是窄窗里被压扁的元凶，且此处打字更直接。
            ImGui::SetNextItemWidth(px(96.0f));
            ImGui::InputInt("##export_from", &g_app.export_from, 0, 0);
            ImGui::SameLine(0.0f, px(6.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("至");
            ImGui::SameLine(0.0f, px(6.0f));
            ImGui::SetNextItemWidth(px(96.0f));
            ImGui::InputInt("##export_to", &g_app.export_to, 0, 0);
            ImGui::SameLine(0.0f, px(12.0f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("共 %d 页", total);
        }
        g_app.export_from = std::clamp(g_app.export_from, 1, total);
        g_app.export_to = std::clamp(g_app.export_to, 1, total);

        // ---- 清晰度（DPI）----
        ImGui::SeparatorText("清晰度");
        {
            // 档位按钮等宽平铺：一眼看清有哪些档、当前选中哪一档。
            const float avail = ImGui::GetContentRegionAvail().x;
            const float gap = st.ItemSpacing.x;
            const int n = IM_ARRAYSIZE(kDpiPresets);
            const float bw = (avail - gap * (n - 1)) / n;
            for (int i = 0; i < n; ++i) {
                if (i > 0) ImGui::SameLine();
                char lab[16] = {};
                std::snprintf(lab, sizeof lab, "%d", kDpiPresets[i]);
                if (preset_button(lab, g_app.export_dpi == kDpiPresets[i], bw))
                    g_app.export_dpi = kDpiPresets[i];
            }
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("自定义");
        ImGui::SameLine(0.0f, px(6.0f));
        ImGui::SetNextItemWidth(px(110.0f));
        ImGui::InputInt("##export_dpi", &g_app.export_dpi, 0, 0);
        ImGui::SameLine(0.0f, px(6.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("DPI");
        g_app.export_dpi = std::clamp(g_app.export_dpi, 36, 1200);
        hint_text("DPI 越高越清晰、文件越大（72 = 原始尺寸）");

        // ---- 质量（仅 JPEG）----
        // 格式在菜单里已经定死（标题写明），这里不再重复问。PNG 无损，直接不出现质量栏 ——
        // 留一个"滑了也没用"的滑杆比没有更糟（人工反馈）。
        if (g_app.export_format == 1) {
            ImGui::SeparatorText("质量");
            ImGui::SetNextItemWidth(-FLT_MIN);   // 撑满宽度
            ImGui::SliderInt("##export_quality", &g_app.export_quality, 1, 100);
        }

        // ---- 内容 ----
        ImGui::SeparatorText("内容");
        ImGui::Checkbox("应用当前视图（旋转与纸张配色）", &g_app.export_apply_view);
        hint_text("默认导出文档本来的样子；勾选后导出你现在看到的样子");

        ImGui::Separator();

        const float btn_h = ImGui::GetFrameHeight();
        if (g_app.export_busy) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("正在导出 %d / %d 页…", g_app.export_done, g_app.export_total);
            const float cw = ImGui::CalcTextSize("取消导出").x + st.FramePadding.x * 2.0f;
            ImGui::SameLine(right_edge - cw);
            if (ImGui::Button("取消导出", ImVec2(cw, btn_h))) g_app.renderer->cancel_export();
        } else {
            const float cw = ImGui::CalcTextSize("取消").x + st.FramePadding.x * 2.0f;
            const float ew = ImGui::CalcTextSize("导出").x + st.FramePadding.x * 2.0f;
            // 右对齐两个按钮。**必须**用 SetCursorPosX 另起一行：Separator 自身也算作一行，
            // 紧接 SameLine 会把按钮拉回分隔线那一行、与线重叠（人工反馈的 bug）。
            ImGui::SetCursorPosX(right_edge - (cw + st.ItemSpacing.x + ew));
            if (ImGui::Button("取消", ImVec2(cw, btn_h))) g_app.export_open = false;
            ImGui::SameLine();
            const ig::StyleColor4 primary = primary_button_style();
            if (ImGui::Button("导出", ImVec2(ew, btn_h))) {
                g_app.export_pages = compute_pages();
                if (g_app.export_pages.empty()) {
                    show_toast("没有可导出的页");
                } else {
                    g_app.request_export_pick = true;   // 帧间弹输出选择（另存为 / 选目录）
                    g_app.export_open = false;          // 关闭对话框（动画收敛后真正销毁）
                }
            }
        }

        anim_lock.dismiss();
        if (!alive) ImGui::CloseCurrentPopup();   // 动画收敛：本帧已全透明，安全销毁
    }
}

}  // namespace lr::app
