// main.cpp — Lilith Reader 应用外壳（Phase 1 外壳 + Phase 2 文档核心 + Phase 3 自研画布
// + Phase 4 渲染调度：失败重试 / 方向感知预加载 / 缓存预算）
// Win32 + D3D11 + ImGui：命令行/拖放打开、窗口状态持久化、画布阅读（滚动/缩放/多列网格）。
//
// 分层（架构文档 §1）：main 只做 UI 与输入；文档经 render 调度层访问（UI 线程零 fz_*、
// 零阻塞）；布局数学全在 lilithreader.canvas（纯函数，可单测）。
//
// 项目纪律：不在开发中实际运行本软件做测试（编译/自动化测试除外）；
// 人工运行验证项统一记录在 docs/04-人工验证.md。

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <imm.h>          // ImmAssociateContext：让阅读窗口脱离输入法（ADR-028）
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

import lilithreader.utils;
import lilithreader.document;
import lilithreader.canvas;
import lilithreader.render;

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr wchar_t kWindowClass[] = L"LilithReaderWnd";
constexpr wchar_t kWindowTitle[] = L"Lilith Reader";

// ---- 界面缩放（DPI）----
//
// 所有"屏幕像素"设计值都以 **100% 缩放（96dpi）** 为基准，运行时统一过 px()。
//   · g_dpi_scale  自动：窗口所在显示器的 DPI（WM_DPICHANGED 时刷新）
//   · g_user_scale 手动：用户字号/界面缩放（将来的"全局设置"驱动，现固定 1.0）
// 字体不在这里乘，而是交给 ImGui 的 style.FontScaleDpi / FontScaleMain —— 1.92 起的
// 动态字体系统会按新尺寸重新栅格化，比手工改字号更不容易漏掉某处文本。
float g_dpi_scale = 1.0f;
float g_user_scale = 1.0f;

inline float ui_scale() { return g_dpi_scale * g_user_scale; }
inline float px(float base) { return base * ui_scale(); }

HWND g_hwnd = nullptr;

// 未缩放的 ImGui 基准样式（CreateContext 后立即留底；每次缩放都从它重算，避免累积）
ImGuiStyle g_base_style;
bool g_base_style_ready = false;

void apply_ui_scale() {
    if (!g_base_style_ready) return;
    ImGuiStyle& st = ImGui::GetStyle();
    st = g_base_style;
    st.ScaleAllSizes(ui_scale());      // 内边距/间距/圆角/滚动条（不含字体）
    st.FontScaleMain = g_user_scale;   // 字体：主缩放（将来交给用户设置）
    st.FontScaleDpi = g_dpi_scale;     // 字体：DPI 缩放（自动）
}

// 读取窗口所在显示器的 DPI 缩放；变化时重算样式。仅在帧间调用（改样式不能跨帧）。
void refresh_dpi_scale() {
    if (!g_hwnd) return;
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd);
    if (dpi > 0.0f && std::fabs(dpi - g_dpi_scale) > 0.001f) {
        g_dpi_scale = dpi;
        apply_ui_scale();
    }
}

// ---- 画布交互参数（基准像素，用前过 px()）----
constexpr float  kUiFontBasePx = 20.0f;    // UI 基准字号（×FontScaleMain×FontScaleDpi 后为实际像素）
constexpr float  kScrollStepPx = 120.0f;   // 每格滚轮滚动的屏幕像素
constexpr float  kKeyScrollPx = 80.0f;     // 方向键每次滚动的屏幕像素
constexpr float  kZoomStep = 1.15f;        // 每格 Ctrl+滚轮 / 每次 +/- 的缩放倍率
constexpr double kZoomDebounceSec = 0.15;  // 缩放稳定后触发高清重渲染的等待时间
constexpr float  kStatusBarH = 34.0f;      // 底部状态栏高度
constexpr float  kCanvasMarginPx = 18.0f;   // 画布四周留白（**屏幕像素**，随 DPI 缩放）
// 页/列间距：占**列宽**的比例（文档空间，ADR-029）。屏幕间距 = 比例 × 列宽 × zoom，
// 故随缩放线性变化；**刻意不过 px()**：它要与页面同比例，不该随 DPI 单独放大。
constexpr float  kCanvasGapRatio = 0.013f;

// ---- 主题色（浅色阅读器） ----
constexpr ImU32 kColBackdrop = IM_COL32(228, 231, 235, 255);  // 页面区背景
constexpr ImU32 kColChrome = IM_COL32(242, 244, 246, 255);    // 状态栏背景
constexpr ImU32 kColPageBorder = IM_COL32(0, 0, 0, 46);
constexpr ImU32 kColPlaceholder = IM_COL32(246, 247, 249, 255);
constexpr ImU32 kColPlaceholderBorder = IM_COL32(200, 204, 210, 255);
constexpr ImU32 kColPlaceholderText = IM_COL32(130, 134, 140, 255);
constexpr ImU32 kColFailed = IM_COL32(252, 238, 238, 255);
constexpr ImU32 kColFailedBorder = IM_COL32(220, 150, 150, 255);
constexpr ImU32 kColChromeText = IM_COL32(60, 64, 70, 255);
constexpr ImU32 kColChromeDim = IM_COL32(120, 126, 134, 255);

// ---------------- D3D11（RAII） ----------------
struct Graphics {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    DXGI_SWAP_CHAIN_DESC sc_desc{};

    bool initialize(HWND hwnd) {
        sc_desc.BufferCount = 2;
        sc_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc_desc.OutputWindow = hwnd;
        sc_desc.SampleDesc.Count = 1;
        sc_desc.Windowed = TRUE;
        sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        constexpr D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        };
        D3D_FEATURE_LEVEL got{};
        if (FAILED(D3D11CreateDeviceAndSwapChain(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                &sc_desc, &swap_chain, &device, &got, &context)))
            return false;
        return create_rtv();
    }

    bool create_rtv() {
        ID3D11Texture2D* back = nullptr;
        if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
        const bool ok = SUCCEEDED(device->CreateRenderTargetView(back, nullptr, &rtv));
        back->Release();
        return ok;
    }

    void resize(UINT w, UINT h) {
        if (!swap_chain || w == 0 || h == 0) return;
        if (rtv) { rtv->Release(); rtv = nullptr; }
        if (SUCCEEDED(swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
            create_rtv();
    }

    void render_frame() {
        if (!rtv) return;  // resize 失败瞬间可能无渲染目标
        const float clear[4] = { 0.10f, 0.14f, 0.18f, 1.0f };
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->ClearRenderTargetView(rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swap_chain->Present(1, 0);
    }

    void shutdown() {
        if (rtv) rtv->Release();
        if (swap_chain) swap_chain->Release();
        if (context) context->Release();
        if (device) device->Release();
    }
} g_gfx;

// ---------------- ImGui（RAII；Phase 3 起不再依赖 ImPlot） ----------------
struct ImGuiRaii {
    bool win32 = false, dx11 = false, ctx = false;

    void initialize(HWND hwnd) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ctx = true;
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // 关掉 ImGui 自己的 imgui.ini 持久化（详见 Phase 2 记录）：
        // 「把文件拖到 exe 上」时 Explorer 会把工作目录设为被拖文件目录，
        // 默认相对路径会在用户目录里凭空生成 imgui.ini。窗口/布局状态由
        // LilithReader.ini 自管，故直接禁用。
        io.IniFilename = nullptr;

        // 留底未缩放的基准样式：apply_ui_scale() 每次从它重算，避免多次缩放累积。
        g_base_style = ImGui::GetStyle();
        g_base_style_ready = true;

        // 中文字体：系统微软雅黑（Phase 6 换为 exe 内嵌子集字体）。
        // 这里给的是**基准字号**；实际渲染尺寸 = 基准 × FontScaleMain × FontScaleDpi。
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", kUiFontBasePx,
            nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());

        win32 = ImGui_ImplWin32_Init(hwnd);
        dx11 = ImGui_ImplDX11_Init(g_gfx.device, g_gfx.context);

        // DPI：按窗口所在显示器缩放字体与界面度量（100% 时为 1.0，等价于旧行为）
        refresh_dpi_scale();
        apply_ui_scale();
    }

    void new_frame() {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void shutdown() {
        if (dx11) ImGui_ImplDX11_Shutdown();
        if (win32) ImGui_ImplWin32_Shutdown();
        if (ctx) ImGui::DestroyContext();
    }
} g_ui;

// ---------------- 全局状态 ----------------
std::unique_ptr<lr::Renderer> g_renderer;  // 渲染调度层（Phase 3 取代 DocSession）
lr::Canvas g_canvas;

struct UiDoc {
    enum class Kind { None, Rejected, Opening, Reading, Failed };
    Kind         kind = Kind::None;
    std::wstring path_w;
    std::string  name_u8, ext_u8;
    lr::DocError error = lr::DocError::Ok;  // Rejected / Failed 时的具体原因
    std::string  detail_u8;                 // MuPDF 原始错误信息
    lr::DocumentInfo info{};
    std::uint64_t request_id = 0;
} g_doc;

std::wstring g_ini_path;
bool g_show_debug = false;
bool g_fullscreen = false;
WINDOWPLACEMENT g_prev_placement{ sizeof(WINDOWPLACEMENT) };

// 缩放防抖状态（详见 update_want_scale）
float  g_want_scale = -1.0f;         // 已投递给渲染层的倍率；<0 表示尚未初始化
float  g_last_target_scale = -1.0f;  // 上一帧的目标倍率
double g_zoom_dirty_since = -1.0;    // 目标倍率最后一次变化的时刻；<0 表示无待定

// 滚动方向（-1 上 / 0 未定 / +1 下）：只服务**方向感知预加载**（Phase 4）——
// 向下滚只预取下方一行，向上滚只预取上方，避免为回不去的方向白渲染。
// 刚打开/跳页后为 0（未定），此时两侧都预取，防止首个方向反转出现空档。
int    g_scroll_dir = 0;
float  g_prev_scroll_y = 0.0f;

// 缓存字节预算的 ini 默认值（MB）；实际取值读 [cache] BudgetMB，再由渲染层钳制到
// [kCacheBudgetMin, kCacheBudgetMax]。用户可手改 ini 调整，无需设置界面。
constexpr int kCacheBudgetDefaultMB = 512;

// 画布留白/间距当前所依据的界面缩放值；与 ui_scale() 不一致时才重新下发
// （set_margin_gap 会让布局缓存失效，不能每帧无条件调用）。
float g_canvas_scale = -1.0f;

// 画布悬停/焦点状态：**仅供 F3 诊断显示**，不参与任何逻辑分支。
// 键盘快捷键不再以焦点为门（见 handle_canvas_input 上方注释），保留这两个量是为了
// 将来再遇到"某个键没反应"时能一眼看出是焦点问题还是别的（第四轮就是靠它排除了焦点假设）。
bool g_canvas_hovered = false;
bool g_canvas_focused = false;

// 跳页弹窗
bool g_open_jump = false;
int  g_jump_page = 1;

void update_title() {
    if (!g_hwnd) return;
    std::wstring title = kWindowTitle;
    if (!g_doc.path_w.empty()) {
        title += L" — ";
        title += lr::file_name_of(g_doc.path_w);
    }
    SetWindowTextW(g_hwnd, title.c_str());
}

// ---------------- 文档状态机（UI 侧） ----------------

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
    update_title();
}

// 文档打开成功 → 初始化画布并进入阅读态
void enter_reading() {
    lr::PageSizePt sz;
    sz.w = g_doc.info.page_width_pt > 0.0f ? g_doc.info.page_width_pt : 595.0f;
    sz.h = g_doc.info.page_height_pt > 0.0f ? g_doc.info.page_height_pt : 842.0f;

    lr::CanvasState st;  // 默认：fit_width=true, columns=1
    st.margin_px = px(kCanvasMarginPx);   // 屏幕像素 → 随 DPI
    st.gap_ratio = kCanvasGapRatio;       // 列宽比例 → 随缩放（ADR-029），不随 DPI

    g_canvas = lr::Canvas{};
    g_canvas.set_default_size(sz);

    // 逐页真实尺寸：PDF 允许各页尺寸/纵横比不同（封面、插页、横向页、扫描裁切不一）。
    // 若只用首页尺寸统一布局，各页纹理会被拉伸进"首页纵横比"的矩形 → 异构 PDF 形变。
    // 尺寸探测失败的页为 {0,0}，此处按首页尺寸回退。
    std::vector<lr::PageSizePt> sizes;
    sizes.reserve(g_doc.info.page_sizes.size());
    for (const lr::PageSize& ps : g_doc.info.page_sizes) {
        lr::PageSizePt e;
        e.w = ps.width_pt > 0.0f ? ps.width_pt : sz.w;
        e.h = ps.height_pt > 0.0f ? ps.height_pt : sz.h;
        sizes.push_back(e);
    }
    if (sizes.empty())
        g_canvas.set_uniform(g_doc.info.page_count, sz);
    else
        g_canvas.set_page_sizes(std::move(sizes));

    g_canvas.set_state(st);
    g_canvas.clamp_scroll();

    g_want_scale = -1.0f;  // 首帧立即采用目标倍率，不走防抖
    g_last_target_scale = -1.0f;
    g_zoom_dirty_since = -1.0;
    g_canvas_scale = ui_scale();  // 留白已按当前缩放写入
    g_scroll_dir = 0;             // 方向未定：首帧两侧都预取
    g_prev_scroll_y = g_canvas.state().scroll_y;
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
    if (g_doc.kind == UiDoc::Kind::Opening || g_doc.kind == UiDoc::Kind::Reading)
        g_renderer->close();
    reset_doc_state();
}

void poll_document() {
    if (g_doc.kind != UiDoc::Kind::Opening) return;

    const lr::DocState snap = g_renderer->doc_state();
    if (snap.id != g_doc.request_id) return;  // 已被更新的请求取代
    if (snap.phase == lr::DocPhase::Opening) return;

    switch (snap.phase) {
    case lr::DocPhase::Ready:
        g_doc.kind = UiDoc::Kind::Reading;
        g_doc.info = snap.info;
        g_doc.error = lr::DocError::Ok;
        enter_reading();
        break;
    case lr::DocPhase::Failed:
        g_doc.kind = UiDoc::Kind::Failed;
        g_doc.error = snap.error;
        g_doc.detail_u8 = snap.detail_u8;
        break;
    default:  // Idle：被显式关闭
        reset_doc_state();
        return;
    }
    update_title();
}

// ---------------- 窗口状态校验 ----------------
bool placement_usable(const lr::WindowState& s) {
    if (s.w < 400 || s.h < 300) return false;
    const LONG vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const LONG vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const LONG vr = vx + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const LONG vb = vy + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const LONG ix = s.x > vx ? s.x : vx;
    const LONG iy = s.y > vy ? s.y : vy;
    const LONG rx = (s.x + s.w) < vr ? (s.x + s.w) : vr;
    const LONG ry = (s.y + s.h) < vb ? (s.y + s.h) : vb;
    return (rx - ix) >= 100 && (ry - iy) >= 100;
}

void toggle_fullscreen() {
    if (!g_hwnd) return;
    if (!g_fullscreen) {
        GetWindowPlacement(g_hwnd, &g_prev_placement);
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
            SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(g_hwnd, HWND_TOP,
                         mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_FRAMECHANGED | SWP_NOZORDER);
            g_fullscreen = true;
        }
    } else {
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(g_hwnd, &g_prev_placement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

// ---------------- 画布输入 ----------------
// 翻行/翻页的几何计算全在画布层（纯函数、可单测，见 canvas_test 的 scroll_rows 用例）。
// UI 层只做转发：不再像早期版本那样用 visible_first() 自行推算目标行 —— 那正是
// "视口高于一行时末页反复卡住"的根因（已由阅读游标修掉，见 ADR-023 与 canvas.ixx）。
void scroll_by_rows(int dir) { g_canvas.scroll_rows(dir); }

// 列数快捷键：主键盘 1~4 与小键盘 1~4；未按返回 0。
// 两者在 ImGui 里是**不同的键**（主键盘 ImGuiKey_1..9、小键盘 ImGuiKey_Keypad1..9），
// 必须分别判断 —— 否则"小键盘按了没反应"（第四轮反馈点名了这一点）。
int pressed_column_key() {
    if (ImGui::IsKeyPressed(ImGuiKey_1, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Keypad1, false)) return 1;
    if (ImGui::IsKeyPressed(ImGuiKey_2, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Keypad2, false)) return 2;
    if (ImGui::IsKeyPressed(ImGuiKey_3, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Keypad3, false)) return 3;
    if (ImGui::IsKeyPressed(ImGuiKey_4, false) ||
        ImGui::IsKeyPressed(ImGuiKey_Keypad4, false)) return 4;
    return 0;
}

// 键盘快捷键**刻意不以 ImGui 窗口焦点为门**（第四轮调试修复，ADR-026）：
//   1. 窗口结构是「无边框 shell 根窗口 + ##canvas 子窗口」，两者都带
//      ImGuiWindowFlags_NoNav（= NoNavInputs | NoNavFocus）。NoNavFocus 使 ImGui
//      在窗口出现时**不**把它设为 g.NavWindow，于是**启动后、首次点进画布之前
//      ImGui::IsWindowFocused() 恒为 false** —— 这段时间里所有快捷键（含 1/2/3/4
//      切列）全是死的，必须先点一下画布。用户反馈"按 1234 不切列"即由此而来。
//   2. 快捷键是**应用级语义**（整个窗口只有一个阅读视图），本就不该由 ImGui 的窗口
//      焦点决定；跳页弹窗/调试浮层出现时焦点会移走，同样会让快捷键莫名失效。
// 故此处只以「无文本输入（io.WantTextInput）」为门；阅读态与跳页弹窗由调用方保证。
// 鼠标（滚轮/拖拽）仍需悬停在画布上，与键盘分开判断。
void handle_canvas_input(const ImVec2& origin, const ImVec2& size, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    const float cx = size.x * 0.5f;
    const float cy = size.y * 0.5f;

    // 滚轮：Ctrl 缩放（以鼠标为不动点），否则滚动
    if (hovered && io.MouseWheel != 0.0f) {
        if (io.KeyCtrl) {
            g_canvas.zoom_by(std::pow(kZoomStep, io.MouseWheel),
                             io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        } else {
            g_canvas.scroll_by(0.0f, -io.MouseWheel * px(kScrollStepPx));
        }
    }

    // 左键拖拽平移（位移直接取鼠标物理像素增量，不做缩放换算）
    if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
        g_canvas.scroll_by(-io.MouseDelta.x, -io.MouseDelta.y);
    }

    if (io.WantTextInput) return;  // 有文本输入在跑：键盘归它（正常情况被 g_open_jump 拦住）

    const float vh = size.y;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))  g_canvas.scroll_by(0.0f, px(kKeyScrollPx));
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true))    g_canvas.scroll_by(0.0f, -px(kKeyScrollPx));
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))   g_canvas.scroll_by(0.0f, vh * 0.9f);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))     g_canvas.scroll_by(0.0f, -vh * 0.9f);
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false))      g_canvas.scroll_to_page(0, 0.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false))
        g_canvas.scroll_to_page(g_canvas.page_count() - 1, 0.0f);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) scroll_by_rows(+1);
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  scroll_by_rows(-1);

    const int cols = pressed_column_key();
    if (cols != 0) g_canvas.set_columns(cols);

    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) g_canvas.fit_to_width();
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_0, false)) g_canvas.fit_to_width();
    if (ImGui::IsKeyPressed(ImGuiKey_Equal, true) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, true))
        g_canvas.zoom_by(kZoomStep, cx, cy);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, true) ||
        ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, true))
        g_canvas.zoom_by(1.0f / kZoomStep, cx, cy);

    if (ImGui::IsKeyPressed(ImGuiKey_G, false)) {
        g_open_jump = true;
        g_jump_page = g_canvas.current_page() + 1;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F11, false)) toggle_fullscreen();
}

// 缩放防抖：目标倍率稳定 150ms 后，才按新倍率请求高清重渲染；
// 期间 g_want_scale 保持旧值，页面用现有纹理显示（双线性放大，不闪白）。
void update_want_scale() {
    const float target = g_canvas.effective_zoom();
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
// 预加载策略（Phase 4）：可见行 ±1 行，且**方向感知** —— 向下滚只预取下方一行、
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
        pf = std::min(pf, g_canvas.row_page_begin(r0 - 1));    // 上方一行

    std::vector<lr::RenderWant> wants;
    wants.reserve(static_cast<std::size_t>(pl - pf + 1));
    for (int i = first; i <= last && i < n; ++i) wants.push_back({ i, scale });
    for (int i = pf; i <= pl; ++i)
        if (i >= 0 && i < n && (i < first || i > last)) wants.push_back({ i, scale });

    g_renderer->set_wanted(std::move(wants));
}

// ---------------- 画布绘制 ----------------
void draw_page_placeholder(ImDrawList* dl, const ImVec2& pmin, const ImVec2& pmax,
                           const lr::PageSlot& s) {
    const bool failed = (s.status == lr::PageStatus::Failed);
    dl->AddRectFilled(pmin, pmax, failed ? kColFailed : kColPlaceholder);
    dl->AddRect(pmin, pmax, failed ? kColFailedBorder : kColPlaceholderBorder);
    // 失败占位提示"点击重试"（Phase 4）：命中测试在 draw_canvas_area 里做（见 retry 注释）
    const char* txt = failed ? "渲染失败 · 点击重试"
                             : (s.status == lr::PageStatus::Loading ? "载入中…" : "");
    if (txt[0] != '\0') {
        const ImVec2 ts = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2((pmin.x + pmax.x - ts.x) * 0.5f, (pmin.y + pmax.y - ts.y) * 0.5f),
                    kColPlaceholderText, txt);
    }
}

void draw_canvas_area() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##canvas", ImVec2(0, -px(kStatusBarH)), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 size = ImGui::GetContentRegionAvail();

    // DPI/界面缩放变化时同步画布留白（只在不一致时下发：set_margin_gap 会让布局缓存失效，
    // 每帧无条件调用会毁掉"滚动不重算布局"的 O(1) 性质）。间距是列宽比例，与 DPI 无关。
    if (g_canvas_scale != ui_scale()) {
        g_canvas.set_margin_gap(px(kCanvasMarginPx), kCanvasGapRatio);
        g_canvas_scale = ui_scale();
    }
    g_canvas.set_viewport(size.x, size.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), kColBackdrop);

    const bool hovered = ImGui::IsWindowHovered();
    g_canvas_hovered = hovered;
    g_canvas_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // 失败占位点击重试（Phase 4）：在输入处理**之前**做命中测试，只针对
    // "Failed 且尚无纹理"的页（曾成功渲染过、因重渲染失败而保留旧图的页不显示占位，
    // 也无从点击）。单击不会触发拖拽平移（平移需要移动阈值），故两者不冲突。
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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

    if (!g_open_jump) handle_canvas_input(origin, size, hovered);

    // 滚动方向（供方向感知预加载）：以内容坐标 scroll_y 的变化判定。
    // 阈值 0.5px 抑制浮点抖动导致的假翻转。
    {
        const float sy = g_canvas.state().scroll_y;
        if (sy > g_prev_scroll_y + 0.5f) g_scroll_dir = +1;
        else if (sy < g_prev_scroll_y - 0.5f) g_scroll_dir = -1;
        g_prev_scroll_y = sy;
    }

    update_want_scale();
    emit_wants();

    const int n = g_canvas.page_count();
    const int first = g_canvas.visible_first();
    const int last = g_canvas.visible_last();
    const ImVec2 clip_max(origin.x + size.x, origin.y + size.y);
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
            dl->AddImage(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(s.texture)),
                         pmin, pmax);
            dl->AddRect(pmin, pmax, kColPageBorder);
        } else {
            draw_page_placeholder(dl, pmin, pmax, s);
        }
    }
    dl->PopClipRect();

    ImGui::EndChild();
}

void draw_status_bar() {
    const float bar_h = px(kStatusBarH);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(10), px(4)));
    ImGui::BeginChild("##status", ImVec2(0, bar_h), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                      ImGuiWindowFlags_NoNav);
    ImGui::PopStyleVar();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    dl->AddRectFilled(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), kColChrome);

    const int total = g_canvas.page_count();
    // 页码用画布游标（到底时即末行首页），而不是 visible_first()：后者在
    // 视口高于一行时会停在末行前一行，页码会与所见不符（详见 canvas.ixx）。
    const int cur = std::min(total, std::max(1, g_canvas.current_page() + 1));

    std::string left = g_doc.name_u8.empty() ? "(未命名)" : g_doc.name_u8;
    if (!g_doc.info.format.empty() &&
        !lr::format_matches_extension(g_doc.info.format, g_doc.ext_u8)) {
        left += "  ·  实际格式 ";
        left += g_doc.info.format;
        left += "（扩展名 ";
        left += g_doc.ext_u8;
        left += " 不符）";
    }

    char right[160];
    std::snprintf(right, sizeof right, "%d / %d   ·   %d%%   ·   %d 列",
                  cur, total,
                  static_cast<int>(std::lround(g_canvas.effective_zoom() * 100.0f)),
                  g_canvas.state().columns);

    ImGui::SetCursorPos(ImVec2(px(10), (bar_h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kColChromeText), "%s", left.c_str());

    const float rw = ImGui::CalcTextSize(right).x;
    ImGui::SetCursorPos(ImVec2(ws.x - rw - px(10), (bar_h - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kColChromeDim), "%s", right);

    ImGui::EndChild();
}

// ---------------- 引导 / 状态页（Phase 1/2 保留） ----------------
constexpr const char* kFormatsLine = "支持 PDF · EPUB · MOBI · FB2 · CBZ · XPS · 图片(PNG/JPG/GIF/BMP/TIFF)";
constexpr ImVec4 kColBright{ 0.92f, 0.93f, 0.95f, 1.0f };
constexpr ImVec4 kColDim{ 0.58f, 0.62f, 0.66f, 1.0f };
constexpr ImVec4 kColWarn{ 0.95f, 0.72f, 0.42f, 1.0f };

void centered_text(const char* text, float dy, const ImVec4& col) {
    const ImVec2 ws = ImGui::GetWindowSize();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    // dy 是基准像素偏移，随界面缩放（否则高 DPI 下多行文本会挤在一起）
    ImGui::SetCursorPos(ImVec2((ws.x - ts.x) * 0.5f, (ws.y - ts.y) * 0.5f + dy * ui_scale()));
    ImGui::TextColored(col, "%s", text);
}

void draw_drop_guide() {
    centered_text("将文档拖入窗口打开", -56.0f, kColBright);
    centered_text(kFormatsLine, -12.0f, kColDim);
    centered_text("也可以用命令行：LilithReader.exe <文件路径>", 12.0f, kColDim);
    centered_text("按 Esc 退出", 56.0f, kColDim);
}

void draw_opening() {
    centered_text(g_doc.name_u8.c_str(), -44.0f, kColBright);
    static const char* kDots[] = { "正在打开 ．", "正在打开 ．．", "正在打开 ．．．" };
    const int frame = static_cast<int>(ImGui::GetTime() * 3.0) % 3;
    centered_text(kDots[frame], 0.0f, kColDim);
    centered_text("按 Esc 取消", 44.0f, kColDim);
}

void draw_failed() {
    centered_text(lr::describe(g_doc.error).data(), -84.0f, kColWarn);
    centered_text(g_doc.name_u8.c_str(), -42.0f, kColBright);

    if (g_doc.error == lr::DocError::Unsupported) {
        centered_text(kFormatsLine, 0.0f, kColDim);
    } else if (g_doc.error == lr::DocError::Mismatched) {
        centered_text("实际内容是一个压缩包（zip/tar）", 0.0f, kColDim);
        centered_text("若是图片集，请把扩展名改回 .cbz；否则请先解压", 36.0f, kColDim);
    } else if (!g_doc.detail_u8.empty()) {
        std::string detail = g_doc.detail_u8;
        if (detail.size() > 160) detail = detail.substr(0, 160) + "…";
        centered_text(detail.c_str(), 0.0f, kColDim);
    }
    centered_text("按 Esc 返回", 84.0f, kColDim);
}

void draw_rejected() {
    if (g_doc.error == lr::DocError::NotFound) {
        centered_text(lr::describe(g_doc.error).data(), -42.0f, kColBright);
        centered_text(lr::wide_to_utf8(g_doc.path_w).c_str(), 0.0f, kColDim);
    } else {  // Unsupported
        centered_text(lr::describe(g_doc.error).data(), -84.0f, kColBright);
        centered_text(g_doc.name_u8.c_str(), -42.0f, kColDim);
        centered_text(kFormatsLine, 0.0f, kColDim);
    }
    centered_text("按 Esc 返回", 44.0f, kColDim);
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

        static const char* kKindNames[] = { "none", "rejected", "opening", "reading", "failed" };
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
            // 缓存统计（Phase 4）：驻留字节/预算、驻留页数、累计逐出页数
            const lr::CacheStats cs = g_renderer->cache_stats();
            ImGui::Text("cache: %.1f / %.0f MB  pages %d  evict %d",
                        (double)cs.used_bytes / 1048576.0,
                        (double)cs.budget_bytes / 1048576.0,
                        cs.resident_pages, cs.evictions);
            ImGui::TextDisabled("preload dir %d  (+1 下 / -1 上 / 0 两侧)",
                                g_scroll_dir);
            // 快捷键**不**看这两个量（ADR-026），列出仅为排查"某个键没反应"时定位用
            ImGui::TextDisabled("canvas hover %d  focus %d  text-input %d",
                                g_canvas_hovered ? 1 : 0, g_canvas_focused ? 1 : 0,
                                ImGui::GetIO().WantTextInput ? 1 : 0);
        }
        if (g_doc.kind == UiDoc::Kind::Failed && !g_doc.detail_u8.empty())
            ImGui::TextDisabled("last_error: %.120s", g_doc.detail_u8.c_str());
    }
    ImGui::End();
}

// ---------------- 跳页弹窗 ----------------
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
            g_canvas.scroll_to_page(p, 0.0f);
            g_open_jump = false;
            ImGui::CloseCurrentPopup();
        } else if (cancel) {
            g_open_jump = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// ---------------- 顶层 UI ----------------
void draw_shell() {
    poll_document();
    g_renderer->drain_retired();  // 帧首：释放上一帧退役的纹理

    if (ImGui::IsKeyPressed(ImGuiKey_F3, false)) g_show_debug ^= 1;

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

    switch (g_doc.kind) {
    case UiDoc::Kind::None:
        draw_drop_guide();
        break;
    case UiDoc::Kind::Opening:
        draw_opening();
        break;
    case UiDoc::Kind::Reading:
        draw_canvas_area();
        draw_status_bar();
        break;
    case UiDoc::Kind::Failed:
        draw_failed();
        break;
    case UiDoc::Kind::Rejected:
        draw_rejected();
        break;
    }
    ImGui::End();

    if (g_show_debug) draw_debug_overlay();
    draw_jump_popup();

    // Esc：跳页弹窗 → 关闭弹窗；有文档 → 关闭返回引导页；无文档 → 退出
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g_open_jump) {
            g_open_jump = false;
        } else if (g_doc.kind != UiDoc::Kind::None) {
            close_document();
        } else {
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        }
    }
}

// ---------------- 窗口过程 ----------------
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 切换输入语言（Alt+Shift / Win+Space）时，系统会给窗口**重新关联**输入法，
    // 把 ImmAssociateContext(hwnd, nullptr) 的脱离顶掉 —— 这里再脱一次（ADR-028）。
    // 必须放在 ImGui 后端处理器**之前**：后端会消费 WM_INPUTLANGCHANGE 并 return 1。
    if (msg == WM_INPUTLANGCHANGE) ImmAssociateContext(hwnd, nullptr);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        g_gfx.resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        // 最小窗口尺寸也随界面缩放，否则高 DPI 下会小到放不下内容
        mmi->ptMinTrackSize = { static_cast<LONG>(px(480)), static_cast<LONG>(px(320)) };
        return 0;
    }
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
        if (len > 0 && len < 4096) {
            std::wstring path(len + 1, L'\0');  // 含终止符
            DragQueryFileW(drop, 0, path.data(), len + 1);
            path.resize(len);
            request_open_document(std::move(path));
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DPICHANGED:
        // 移到不同 DPI 的显示器：先按新 DPI 重算字体/样式，再按系统建议矩形调整窗口。
        // 本消息在帧间（消息泵）到达，改样式安全。
        refresh_dpi_scale();
        if (const RECT* r = reinterpret_cast<const RECT*>(lp))
            SetWindowPos(hwnd, nullptr, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_DESTROY: {
        if (!g_fullscreen) {  // 全屏态不覆盖保存的正常态矩形
            WINDOWPLACEMENT placement{ sizeof(placement) };
            if (GetWindowPlacement(hwnd, &placement)) {
                lr::WindowState s;
                s.x = placement.rcNormalPosition.left;
                s.y = placement.rcNormalPosition.top;
                s.w = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
                s.h = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
                s.maximized = (placement.showCmd == SW_SHOWMAXIMIZED);
                s.valid = true;
                lr::save_window_state(g_ini_path, s);
            }
        }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE,
                    _In_ LPWSTR cmd_line, _In_ int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 命令行解析：LilithReader.exe <文档路径>（wWinMain 的 cmd_line 不含程序名）
    std::wstring doc_path;
    if (cmd_line && *cmd_line) {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(cmd_line, &argc)) {
            if (argc >= 1) doc_path = argv[0];
            LocalFree(argv);
        }
    }

    g_ini_path = lr::exe_dir() + L"LilithReader.ini";

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    const lr::WindowState saved = lr::load_window_state(g_ini_path);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1280, h = 800;
    if (saved.valid && placement_usable(saved)) {
        x = saved.x; y = saved.y; w = saved.w; h = saved.h;
    }

    g_hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                             x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd || !g_gfx.initialize(g_hwnd)) return 1;

    g_renderer = std::make_unique<lr::Renderer>(g_gfx.device);

    // 缓存字节预算（Phase 4）：读 ini [cache] BudgetMB，缺省 512MB；
    // 渲染层会钳制到 [kCacheBudgetMin, kCacheBudgetMax]（128MB~2GB）。
    // 非法/非正值回落到默认值。
    {
        const int mb = lr::read_ini_int_ex(g_ini_path, L"cache", L"BudgetMB",
                                           kCacheBudgetDefaultMB);
        const std::size_t bytes = static_cast<std::size_t>(mb > 0 ? mb : kCacheBudgetDefaultMB)
                                  * 1024ull * 1024ull;
        g_renderer->set_cache_budget(bytes);
    }

    g_ui.initialize(g_hwnd);
    DragAcceptFiles(g_hwnd, TRUE);

    // 让阅读窗口脱离输入法（ADR-028）。
    // 输入法启用时，Windows 会把字母/数字键的 WM_KEYDOWN 换成 VK_PROCESSKEY(0xE5)，
    // 而 ImGui 的 win32 后端不映射这个键码 → 这些键在 ImGui 里**完全不置位**，
    // 所有字母/数字快捷键（1~4 切列、F 回 fit-width、G 跳页…）静默失效。
    // 第四轮实测证据：WM_KEYDOWN 收到 E5 E5 E5 E5、WM_CHAR 收到正常的 '2'。
    // 本阶段没有任何需要输入法的文本输入（唯一的文本框是纯数字页码），故整窗脱离输入法，
    // 让键盘回归原生语义。将来加入中文输入（搜索/批注）时，改为"仅在文本输入激活时关联输入法"。
    ImmAssociateContext(g_hwnd, nullptr);

    if (!doc_path.empty()) request_open_document(std::move(doc_path));

    ShowWindow(g_hwnd, (saved.valid && saved.maximized) ? SW_MAXIMIZE : show);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto quit;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        g_ui.new_frame();
        draw_shell();
        ImGui::Render();
        g_gfx.render_frame();
    }
quit:
    g_renderer.reset();  // 必须先于 gfx 释放：纹理依赖 D3D11 设备
    g_ui.shutdown();
    g_gfx.shutdown();
    return 0;
}
