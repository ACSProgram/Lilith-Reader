// main.cpp — Lilith Reader 应用外壳（Phase 1 外壳 + Phase 2 文档核心 + Phase 3 自研画布
// + Phase 4 渲染调度 + Phase 5 阅读功能：阅读位置/目录/书签/缩略图/旋转/双页对开/反色护眼/密码）
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
#include <commdlg.h>      // GetOpenFileNameW：菜单「打开文档…」（Phase 6）
#include <imm.h>          // ImmAssociateContext：让阅读窗口脱离输入法（ADR-028）
#include <d3d11.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

import lilithreader.utils;
import lilithreader.document;
import lilithreader.canvas;
import lilithreader.render;
import lilithreader.reader_state;

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "comdlg32.lib")

#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr wchar_t kWindowClass[] = L"LilithReaderWnd";
constexpr wchar_t kWindowTitle[] = L"Lilith Reader";
constexpr int     kAppIconId = 101;          // 见 src/app/app.rc

// ---- 图标（Phase 6）----
//
// 图标字形来自系统字体 **Segoe MDL2 Assets**（Win10+ 内置），与微软雅黑合并成同一字体；
// 字形编码在 Unicode 私用区（PUA），故以 UTF-8 字面量写死，逐个标注码位便于核对。
// 该字体缺失时 g_icons_ok = false，所有按钮退化为文字标签（不出现豆腐块）。
// 码位对照表（本机 segmdl2.ttf 实测渲染确认，非凭记忆）见 docs/03-决策记录.md ADR-045。
constexpr const char* kIcMenu       = "\xEE\x9C\x80";  // U+E700 汉堡菜单
constexpr const char* kIcPane       = "\xEE\xA2\xA0";  // U+E8A0 侧栏面板（带竖分隔的窗格）
constexpr const char* kIcZoomIn     = "\xEE\xA2\xA3";  // U+E8A3 放大
constexpr const char* kIcZoomOut    = "\xEE\x9C\x9F";  // U+E71F 缩小
constexpr const char* kIcExpand     = "\xEE\x9D\x80";  // U+E740 适合宽度/展开
constexpr const char* kIcFullscreen = "\xEE\x87\x99";  // U+E1D9 全屏
constexpr const char* kIcStar       = "\xEE\x9C\xB4";  // U+E734 书签（空心）
constexpr const char* kIcStarFill   = "\xEE\x9C\xB5";  // U+E735 书签（实心）
constexpr const char* kIcSettings   = "\xEE\x9C\x93";  // U+E713 设置
constexpr const char* kIcHelp       = "\xEE\xA2\x97";  // U+E897 帮助
constexpr const char* kIcSun        = "\xEE\x9C\x86";  // U+E706 浅色/日间
constexpr const char* kIcMoon       = "\xEE\x9C\x88";  // U+E708 深色/夜间
constexpr const char* kIcRotate     = "\xEE\x9E\xAD";  // U+E7AD 旋转
constexpr const char* kIcMore       = "\xEE\x9C\x92";  // U+E712 更多
constexpr const char* kIcClose      = "\xEE\x9C\x91";  // U+E711 关闭
constexpr const char* kIcChevDown   = "\xEE\x9C\x8D";  // U+E70D 下箭头（细）
constexpr const char* kIcChevUp     = "\xEE\x9C\x8E";  // U+E70E 上箭头（细）
constexpr const char* kIcArrowUp    = "\xEE\x9D\x8A";  // U+E74A 上箭头
constexpr const char* kIcArrowDown  = "\xEE\x9D\x8B";  // U+E74B 下箭头
constexpr const char* kIcNext       = "\xEE\x9C\xAA";  // U+E72A 下一页
constexpr const char* kIcPrev       = "\xEE\x9C\xAB";  // U+E72B 上一页
constexpr const char* kIcHome       = "\xEE\xA0\x8F";  // U+E80F 首页
constexpr const char* kIcList       = "\xEE\xA3\xBD";  // U+E8FD 目录（列表）
constexpr const char* kIcGrid       = "\xEE\xA0\x8A";  // U+E80A 网格
constexpr const char* kIcPicture    = "\xEE\xA2\xB9";  // U+E8B9 缩略图
constexpr const char* kIcPalette    = "\xEE\x9E\x90";  // U+E790 配色
constexpr const char* kIcFontSize   = "\xEE\xA3\xA9";  // U+E8E9 字号
constexpr const char* kIcBook       = "\xEE\xA0\xAD";  // U+E82D 双页/书
constexpr const char* kIcDoc        = "\xEE\xA2\xA5";  // U+E8A5 单页
constexpr const char* kIcOpenFile   = "\xEE\xA3\xA5";  // U+E8E5 打开文件
constexpr const char* kIcRefresh    = "\xEE\x9C\xAC";  // U+E72C 刷新/重试
constexpr const char* kIcInfo       = "\xEE\xA5\x86";  // U+E946 信息
constexpr const char* kIcSearch     = "\xEE\x9C\xA1";  // U+E721 搜索

// 所有图标字形拼在一起，供 ImFontGlyphRangesBuilder 只把这几十个字形烘进图集
// （不整表烘 PUA，图集体积可控）。**必须与上面各常量逐一对应**——启动时会用 GDI
// 逐个校验字形是否真的存在于本机字体里（不同 Windows 版本的 Segoe MDL2 覆盖不同，
// 缺字形时 ImGui 会静默回退成 "?"，必须拦在启动阶段，见 verify_icon_glyphs）。
constexpr const char* kIconGlyphs =
    "\xEE\x9C\x80\xEE\xA2\xA0\xEE\xA2\xA3\xEE\x9C\x9F\xEE\x9D\x80\xEE\x87\x99"
    "\xEE\x9C\xB4\xEE\x9C\xB5\xEE\x9C\x93\xEE\xA2\x97\xEE\x9C\x86\xEE\x9C\x88"
    "\xEE\x9E\xAD\xEE\x9C\x92\xEE\x9C\x91\xEE\x9C\x8D\xEE\x9C\x8E\xEE\x9D\x8A"
    "\xEE\x9D\x8B\xEE\x9C\xAA\xEE\x9C\xAB\xEE\xA0\x8F\xEE\xA3\xBD\xEE\xA0\x8A"
    "\xEE\xA2\xB9\xEE\x9E\x90\xEE\xA3\xA9\xEE\xA0\xAD\xEE\xA2\xA5\xEE\xA3\xA5"
    "\xEE\x9C\xAC\xEE\xA5\x86\xEE\x9C\xA1";

// 校验 kIconGlyphs 里每个码位在本机 "Segoe MDL2 Assets" 中确有字形。
// 用 GDI 的 GetGlyphIndicesW（GGI_MARK_NONEXISTING_GLYPHS 对缺失字形返回 0xFFFF）。
// 只处理本项目用到的 3 字节 UTF-8 序列（PUA 码位恒为 3 字节）。
bool verify_icon_glyphs() {
    const char* font_path = "C:\\Windows\\Fonts\\segmdl2.ttf";
    if (GetFileAttributesA(font_path) == INVALID_FILE_ATTRIBUTES) return false;

    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) return false;
    HFONT font = CreateFontW(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe MDL2 Assets");
    bool ok = false;
    if (font != nullptr) {
        HGDIOBJ old = SelectObject(dc, font);
        const std::size_t n = std::strlen(kIconGlyphs);
        ok = (n % 3 == 0);
        for (std::size_t i = 0; ok && i + 2 < n; i += 3) {
            const wchar_t cp = static_cast<wchar_t>(
                ((kIconGlyphs[i]     & 0x0Fu) << 12) |
                ((kIconGlyphs[i + 1] & 0x3Fu) << 6)  |
                 (kIconGlyphs[i + 2] & 0x3Fu));
            WORD idx = 0;
            ok = GetGlyphIndicesW(dc, &cp, 1, &idx, GGI_MARK_NONEXISTING_GLYPHS) != GDI_ERROR
                 && idx != 0xFFFF;
        }
        SelectObject(dc, old);
        DeleteObject(font);
    }
    DeleteDC(dc);
    return ok;
}

// ---- 界面缩放（DPI）----
//
// 所有"屏幕像素"设计值都以 **100% 缩放（96dpi）** 为基准，运行时统一过 px()。
//   · g_dpi_scale  自动：窗口所在显示器的 DPI（WM_DPICHANGED 时刷新）
//   · g_user_scale 手动：用户在「设置 → 界面缩放」里调的值（80%~150%）
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

// 主题由下方 apply_theme_colors() 定义；此处前置声明，供 apply_ui_scale 在重算样式后
// 重新套用当前主题（g_base_style 里存的是浅色默认值）。
void apply_theme_colors();
extern bool g_theme_applied;

void apply_ui_scale() {
    if (!g_base_style_ready) return;
    ImGuiStyle& st = ImGui::GetStyle();
    st = g_base_style;
    st.ScaleAllSizes(ui_scale());      // 内边距/间距/圆角/滚动条（不含字体）
    st.FontScaleMain = g_user_scale;   // 字体：主缩放（来自「设置 → 界面缩放」）
    st.FontScaleDpi = g_dpi_scale;     // 字体：DPI 缩放（自动）
    if (g_theme_applied) apply_theme_colors();  // 覆盖 g_base_style 里的浅色配色
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
constexpr float  kStatusBarH = 30.0f;      // 底部状态栏高度
constexpr float  kTopBarH = 40.0f;         // 顶部工具栏高度
constexpr float  kCanvasMarginPx = 18.0f;   // 画布四周留白（**屏幕像素**，随 DPI 缩放）
// 页/列间距：占**列宽**的比例（文档空间，ADR-029）。屏幕间距 = 比例 × 列宽 × zoom，
// 故随缩放线性变化；**刻意不过 px()**：它要与页面同比例，不该随 DPI 单独放大。
constexpr float  kCanvasGapRatio = 0.013f;

// ---- 界面间距基准（Phase 6；100% 缩放值，用前过 px()）----
// 集中命名，避免顶栏/状态栏/侧栏各处手调出的"看起来差不多的不同值"。
constexpr float  kChromePadX = 12.0f;   // 顶栏/状态栏左右内边距
constexpr float  kChromeGapX = 4.0f;    // 顶栏图标按钮之间的水平间距（扁平按钮不需要大间隔）
constexpr float  kPopupPadXY = 8.0f;    // 弹出菜单四周内边距
// 弹出菜单项之间的垂直间距。菜单项高 = 文字高（Selectable 不吃 FramePadding），
// 悬停高亮会**向外扩半个间距**、正好铺满整个间距，故该值同时决定"项高"与"点击区高"。
// 取 10 → 30px 的项高（20px 字号），比默认 7 更从容，且相邻高亮仍连续、无死区。
constexpr float  kMenuItemGapY = 10.0f;

// 图标字形与汉字的**基线差异**（实测，ADR-045）：Segoe MDL2 图标以基线为锚、向上长出，
// 光学中心比微软雅黑汉字高——在 150% DPI / 30px 有效字号下逐像素量得约 6px，折算到 20px
// 基准字号为 4px。正值 = 向下微调，使图标与相邻文字对齐。该值由 ImGui 随有效尺寸等比缩放。
constexpr float  kIconGlyphOffsetY = 4.0f;

// ---- 用户偏好（Phase 6；落盘 exe 同目录 LilithReader.ini）----
//
// 只有真正需要跨会话记忆的量才放这里；阅读位置/书签仍归 reader_state.bin（ADR-034）。
// 全部由「设置」窗口驱动，改动即时生效并即时落盘（写 ini 很小，不必攒到退出）。
struct UiPrefs {
    float ui_scale = 1.0f;             // [ui] UiScale      0.80~1.50
    int   theme = 0;                   // [ui] Theme        0 跟随系统 / 1 浅色 / 2 深色
    bool  auto_hide_toolbar = true;    // [ui] AutoHideToolbar  阅读时顶栏自动隐藏
    bool  motion = true;               // [ui] Motion       页面淡入 / 滚动指示条渐隐
    float gap_percent = kCanvasGapRatio * 100.0f;  // [ui] GapPercent   页面间距（列宽百分比）0~6
    int   cache_mb = 512;              // [cache] BudgetMB  128~2048
};
UiPrefs g_prefs;
// 「界面缩放」改动后需要重算 ImGui 样式；样式不能在一帧中途更换，故置位、下一帧首应用。
bool g_apply_scale_pending = false;

// ---- 主题色 ----
// 阅读器有三套可能的 chrome 表现：浅色、深色、跟随系统；另外**反色模式下强制深色**
// （否则页面变暗、四周仍亮，暗色阅读没有实际意义，ADR-043）。画布/顶栏/状态栏是自绘的，
// 故颜色放进可切换的调色板 g_pal；ImGui 控件配色由 apply_theme_colors 另行设置。
struct Palette {
    ImU32 backdrop;          // 画布（页面区）背景
    ImU32 chrome;            // 顶栏/状态栏背景
    ImU32 chrome_border;     // chrome 下缘分隔线
    ImU32 page_border;       // 页面描边
    ImU32 placeholder;       // 页占位填充
    ImU32 placeholder_border;
    ImU32 placeholder_text;
    ImU32 failed;
    ImU32 failed_border;
    ImU32 chrome_text;       // 状态栏主文字
    ImU32 chrome_dim;        // 状态栏次要文字
    ImU32 accent;            // 强调色（选中/高亮/滚动指示条）
    ImU32 shadow;            // 页面投影（半透明黑）
};

constexpr Palette kPalLight{
    IM_COL32(228, 231, 235, 255), IM_COL32(244, 245, 247, 255),
    IM_COL32(214, 218, 223, 255), IM_COL32(0, 0, 0, 40),
    IM_COL32(246, 247, 249, 255), IM_COL32(200, 204, 210, 255),
    IM_COL32(130, 134, 140, 255), IM_COL32(252, 238, 238, 255),
    IM_COL32(220, 150, 150, 255), IM_COL32(56, 60, 66, 255),
    IM_COL32(122, 128, 136, 255), IM_COL32(59, 125, 216, 255),
    IM_COL32(0, 0, 0, 26),
};

constexpr Palette kPalDark{
    IM_COL32(23, 24, 28, 255),    IM_COL32(32, 33, 38, 255),
    IM_COL32(52, 54, 60, 255),    IM_COL32(255, 255, 255, 34),
    IM_COL32(38, 39, 44, 255),    IM_COL32(78, 80, 88, 255),
    IM_COL32(150, 154, 162, 255), IM_COL32(58, 35, 37, 255),
    IM_COL32(150, 80, 80, 255),   IM_COL32(214, 217, 222, 255),
    IM_COL32(142, 147, 154, 255), IM_COL32(106, 166, 240, 255),
    IM_COL32(0, 0, 0, 90),
};

Palette g_pal = kPalLight;
bool    g_dark_theme = false;          // 当前是否为深色 chrome
bool    g_theme_applied = false;       // g_pal / ImGui 颜色是否已按 g_dark_theme 应用
bool    g_icons_ok = false;            // 图标字形是否已成功合并进字体（否则退化为文字标签）

// 系统是否处于深色模式（「设置 → 个性化 → 颜色」的应用模式）。读不到一律按浅色。
bool system_prefers_dark() {
    DWORD light = 1, size = sizeof light;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
                     &light, &size) != ERROR_SUCCESS)
        return false;
    return light == 0;
}

// 缓存版：sync_theme 每帧都会问一次，"跟随系统"时不能每帧读注册表。2 秒一次足够灵敏。
bool g_sys_dark = false;
double g_sys_dark_checked = -1.0;
bool system_prefers_dark_cached() {
    const double now = ImGui::GetTime();
    if (g_sys_dark_checked < 0.0 || now - g_sys_dark_checked > 2.0) {
        g_sys_dark = system_prefers_dark();
        g_sys_dark_checked = now;
    }
    return g_sys_dark;
}

// ImGui 控件的配色（圆角/间距等几何量在 initialize 里设定，随 DPI 缩放）。
void apply_theme_colors() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec4* c = st.Colors;
    if (g_dark_theme) {
        c[ImGuiCol_Text]                  = ImVec4(0.85f, 0.86f, 0.88f, 1.00f);
        c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.53f, 0.57f, 1.00f);
        c[ImGuiCol_WindowBg]              = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
        c[ImGuiCol_ChildBg]               = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
        c[ImGuiCol_PopupBg]               = ImVec4(0.15f, 0.15f, 0.17f, 0.99f);
        c[ImGuiCol_Border]                = ImVec4(0.24f, 0.25f, 0.28f, 1.00f);
        c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_FrameBg]               = ImVec4(0.19f, 0.20f, 0.23f, 1.00f);
        c[ImGuiCol_FrameBgHovered]        = ImVec4(0.25f, 0.26f, 0.30f, 1.00f);
        c[ImGuiCol_FrameBgActive]         = ImVec4(0.29f, 0.31f, 0.35f, 1.00f);
        c[ImGuiCol_TitleBg]               = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
        c[ImGuiCol_TitleBgActive]         = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
        c[ImGuiCol_MenuBarBg]             = ImVec4(0.15f, 0.15f, 0.17f, 1.00f);
        c[ImGuiCol_Button]                = ImVec4(0.21f, 0.22f, 0.26f, 1.00f);
        c[ImGuiCol_ButtonHovered]         = ImVec4(0.29f, 0.31f, 0.36f, 1.00f);
        c[ImGuiCol_ButtonActive]          = ImVec4(0.35f, 0.37f, 0.43f, 1.00f);
        c[ImGuiCol_Header]                = ImVec4(0.22f, 0.24f, 0.29f, 1.00f);
        c[ImGuiCol_HeaderHovered]         = ImVec4(0.29f, 0.31f, 0.37f, 1.00f);
        c[ImGuiCol_HeaderActive]          = ImVec4(0.34f, 0.37f, 0.44f, 1.00f);
        c[ImGuiCol_Separator]             = ImVec4(0.24f, 0.25f, 0.28f, 1.00f);
        c[ImGuiCol_SeparatorHovered]      = ImVec4(0.40f, 0.55f, 0.75f, 1.00f);
        c[ImGuiCol_SeparatorActive]       = ImVec4(0.42f, 0.65f, 0.94f, 1.00f);
        c[ImGuiCol_Tab]                   = ImVec4(0.16f, 0.17f, 0.20f, 1.00f);
        c[ImGuiCol_TabHovered]            = ImVec4(0.28f, 0.30f, 0.35f, 1.00f);
        c[ImGuiCol_TabSelected]           = ImVec4(0.24f, 0.26f, 0.31f, 1.00f);
        c[ImGuiCol_TabSelectedOverline]   = ImVec4(0.42f, 0.65f, 0.94f, 1.00f);
        c[ImGuiCol_TabDimmed]             = ImVec4(0.14f, 0.15f, 0.17f, 1.00f);
        c[ImGuiCol_TabDimmedSelected]     = ImVec4(0.19f, 0.20f, 0.24f, 1.00f);
        c[ImGuiCol_ScrollbarBg]           = ImVec4(0.13f, 0.13f, 0.15f, 0.00f);
        c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.32f, 0.34f, 0.38f, 0.85f);
        c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.40f, 0.42f, 0.47f, 0.95f);
        c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.47f, 0.49f, 0.55f, 1.00f);
        c[ImGuiCol_CheckMark]             = ImVec4(0.42f, 0.65f, 0.94f, 1.00f);
        c[ImGuiCol_SliderGrab]            = ImVec4(0.42f, 0.65f, 0.94f, 1.00f);
        c[ImGuiCol_SliderGrabActive]      = ImVec4(0.52f, 0.73f, 0.98f, 1.00f);
    } else {
        ImGui::StyleColorsLight(&st);
        // 浅色主题微调：纯黑文字 + 纯白弹窗偏"硬"，尤其菜单里的子菜单箭头（用的是 Text 色）
        // 会显得很重。改用近黑文字与略带灰的白，整体更柔和（Phase 6，ADR-045）。
        c[ImGuiCol_Text]                  = ImVec4(0.13f, 0.15f, 0.18f, 1.00f);
        c[ImGuiCol_TextDisabled]          = ImVec4(0.55f, 0.58f, 0.62f, 1.00f);
        c[ImGuiCol_WindowBg]              = ImVec4(0.96f, 0.965f, 0.972f, 1.00f);
        c[ImGuiCol_ChildBg]               = ImVec4(0.96f, 0.965f, 0.972f, 1.00f);
        c[ImGuiCol_PopupBg]               = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
        c[ImGuiCol_Border]                = ImVec4(0.80f, 0.82f, 0.85f, 1.00f);
        c[ImGuiCol_FrameBg]               = ImVec4(0.90f, 0.91f, 0.925f, 1.00f);
        c[ImGuiCol_FrameBgHovered]        = ImVec4(0.86f, 0.88f, 0.90f, 1.00f);
        c[ImGuiCol_FrameBgActive]         = ImVec4(0.82f, 0.85f, 0.88f, 1.00f);
        c[ImGuiCol_Button]                = ImVec4(0.89f, 0.90f, 0.915f, 1.00f);
        c[ImGuiCol_ButtonHovered]         = ImVec4(0.83f, 0.855f, 0.885f, 1.00f);
        c[ImGuiCol_ButtonActive]          = ImVec4(0.77f, 0.80f, 0.84f, 1.00f);
        c[ImGuiCol_Header]                = ImVec4(0.86f, 0.885f, 0.915f, 1.00f);
        c[ImGuiCol_HeaderHovered]         = ImVec4(0.81f, 0.845f, 0.885f, 1.00f);
        c[ImGuiCol_HeaderActive]          = ImVec4(0.76f, 0.80f, 0.85f, 1.00f);
        c[ImGuiCol_ScrollbarBg]           = ImVec4(0.96f, 0.965f, 0.972f, 0.00f);
        c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.72f, 0.74f, 0.77f, 0.85f);
        c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.64f, 0.665f, 0.70f, 0.95f);
        c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.56f, 0.585f, 0.62f, 1.00f);
        c[ImGuiCol_CheckMark]             = ImVec4(0.23f, 0.49f, 0.85f, 1.00f);
        c[ImGuiCol_SliderGrab]            = ImVec4(0.23f, 0.49f, 0.85f, 1.00f);
        c[ImGuiCol_SliderGrabActive]      = ImVec4(0.16f, 0.41f, 0.76f, 1.00f);
        c[ImGuiCol_TabSelectedOverline]   = ImVec4(0.23f, 0.49f, 0.85f, 1.00f);
    }
}

// 依据「主题偏好 + 文档配色」决定 chrome 明暗；只在需要时真正改动。
void sync_theme(int color_mode) {
    const bool want_dark = (g_prefs.theme == 2) ||
                           (g_prefs.theme == 0 && system_prefers_dark_cached()) ||
                           (color_mode == 1);  // 反色强制深色 chrome（ADR-043）
    if (g_theme_applied && want_dark == g_dark_theme) return;
    g_dark_theme = want_dark;
    g_pal = want_dark ? kPalDark : kPalLight;
    apply_theme_colors();
    g_theme_applied = true;
}

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

        // 界面几何基准（100% 缩放值；apply_ui_scale 会在此基础上按 DPI/用户缩放重算）。
        // 必须设在留底之前，保证 ScaleAllSizes 每次都从一致的基准出发。
        {
            ImGuiStyle& st = ImGui::GetStyle();
            st.WindowRounding    = 10.0f;
            st.ChildRounding     = 8.0f;
            st.FrameRounding     = 6.0f;
            st.PopupRounding     = 10.0f;
            st.ScrollbarRounding = 9.0f;
            st.GrabRounding      = 6.0f;
            st.TabRounding       = 6.0f;
            st.MenuItemRounding  = 6.0f;   // 菜单项悬停高亮也走圆角（1.93 起），避免直角高亮块突兀
            st.WindowBorderSize  = 1.0f;
            st.ChildBorderSize   = 0.0f;
            st.FrameBorderSize   = 0.0f;
            st.PopupBorderSize   = 1.0f;
            st.WindowPadding     = ImVec2(14, 14);
            st.FramePadding      = ImVec2(10, 6);
            st.ItemSpacing       = ImVec2(10, 8);
            st.ItemInnerSpacing  = ImVec2(7, 6);
            st.CellPadding       = ImVec2(9, 6);
            st.ScrollbarSize     = 12.0f;
            st.ScrollbarPadding  = 2.0f;
            st.GrabMinSize       = 11.0f;
            // 分节标题（SeparatorText）：默认 3px 粗线偏装饰，改细线与更克制的内边距
            st.SeparatorTextBorderSize = 1.0f;
            st.SeparatorTextPadding    = ImVec2(16, 5);
        }
        // 留底未缩放的基准样式：apply_ui_scale() 每次从它重算，避免多次缩放累积。
        g_base_style = ImGui::GetStyle();
        g_base_style_ready = true;

        // 中文字体：系统微软雅黑（子集化内嵌属 Phase 6 未完成项，见迁移计划）。
        // 这里给的是**基准字号**；实际渲染尺寸 = 基准 × FontScaleMain × FontScaleDpi。
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", kUiFontBasePx,
            nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());

        // 图标字体：把 Segoe MDL2 Assets 的相关字形**合并**进同一字体（Phase 6）。
        // 只烘 kIconGlyphs 里用到的几十个 PUA 码位，图集不膨胀。字体缺失、或任一码位
        // 在本机字体里没有字形（不同 Windows 版本覆盖不同）时，一律不合并 ——
        // 所有图标按钮退化为文字标签，绝不出现静默回退的 "?" 豆腐块。
        if (verify_icon_glyphs()) {
            static ImVector<ImWchar> icon_ranges;  // 必须活到图集构建（首帧）之后
            ImFontGlyphRangesBuilder builder;
            builder.AddText(kIconGlyphs);
            builder.BuildRanges(&icon_ranges);
            ImFontConfig ic;
            ic.MergeMode = true;
            ic.PixelSnapH = true;
            // 度量校正（Phase 6，ADR-045）：
            //  · GlyphMinAdvanceX：每个图标至少占一个基准 em，宽度一致 → 方形按钮内水平居中、
            //    图标↔文字间距恒定（否则个别字形 advance 偏小会显得"挤"或偏左）。
            //  · GlyphOffset.y：消掉 MDL2 与微软雅黑的基线差异（实测图标光学中心偏高约 6px @30px 有效字号）。
            ic.GlyphMinAdvanceX = kUiFontBasePx;
            ic.GlyphOffset = ImVec2(0.0f, kIconGlyphOffsetY);
            g_icons_ok = io.Fonts->AddFontFromFileTTF(
                             "C:\\Windows\\Fonts\\segmdl2.ttf", kUiFontBasePx,
                             &ic, icon_ranges.Data) != nullptr;
        }

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
    enum class Kind { None, Rejected, Opening, Reading, Failed, NeedsPassword };
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
// 「键盘打开画布右键菜单」（Shift+F10 / 菜单键）：无鼠标场景的等价入口，也用作自动化验证。
// 由 draw_shell 置位，draw_canvas_context_menu 在画布窗口作用域内消费（与右键同一 ID 空间）。
bool g_open_canvas_ctx = false;
// 右键按下/抬起计数（F3 诊断用）：排查"右键菜单没反应"时，一眼区分是输入没到、还是被别的原因挡住。
int g_dbg_r_down = 0;
int g_dbg_r_up = 0;
bool g_fullscreen = false;
WINDOWPLACEMENT g_prev_placement{ sizeof(WINDOWPLACEMENT) };

// ---- 阅读状态持久化（Phase 5）----
lr::ReaderState g_state;          // exe 同目录 reader_state.bin 的全部记录
std::wstring    g_state_path;
std::uint64_t   g_doc_key = 0;    // 当前文档键（0 = 无效，不参与存取）

// ---- 侧栏（Phase 5）：目录 / 书签 / 缩略图 ----
bool  g_show_sidebar = false;
int   g_sidebar_tab = 0;          // 0 目录 / 1 书签 / 2 缩略图
constexpr float kSidebarWidthPx = 300.0f;   // 基准像素，用前过 px()
constexpr int   kThumbTargetPx = 150;       // 缩略图最长边目标像素

// ---- 视图变换（Phase 5）----
int g_rotation = 0;               // 0/90/180/270
int g_color_mode = 0;             // 0 正常 / 1 反色 / 2 护眼
// 未旋转的逐页尺寸（点）；旋转 90/270 时交换宽高后再交给画布。
std::vector<lr::PageSizePt> g_raw_sizes;
lr::PageSizePt g_raw_default{ 595.0f, 842.0f };

// 打开后待恢复的阅读位置（首帧视口就绪后再应用，否则 fit-width 派生 zoom 尚未成立）
bool g_restore_pending = false;
int  g_restore_page = 0;

// ---- 目录（Phase 5）----
std::vector<lr::OutlineItem> g_outline;

// ---- 加密密码对话框（Phase 5）----
bool        g_open_password = false;
bool        g_auth_pending = false;  // 认证请求在途：弹窗保持打开、背景不变，避免闪烁
char        g_password_buf[256] = {};
std::string g_password_error;

// ---- 输入法关联（Phase 5）----
// ADR-028 要求：整窗脱离输入法的代价是"将来加入中文输入时必须改回仅在文本输入激活时关联"。
// Phase 5 引入了密码框/书签标签等文本输入，故在此落实：WantTextInput 变化时切换关联状态。
HIMC g_saved_ime = nullptr;       // 启动时保存的默认输入法上下文
bool g_ime_attached = false;      // 当前是否已关联（文本输入激活）

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

// ---- 设置 / 帮助窗口（Phase 6）----
bool g_show_settings = false;
bool g_show_help = false;

// ---- 顶栏自动隐藏（Phase 6）----
// 阅读态下鼠标离开顶部区域一段时间后收起顶栏（沉浸阅读）；鼠标靠近窗口顶端即重现。
bool   g_toolbar_visible = true;
double g_toolbar_idle_since = -1.0;   // 鼠标离开顶部区域后的计时起点；<0 表示未计时
constexpr double kToolbarHideDelaySec = 1.6;
constexpr float  kToolbarRevealBandPx = 4.0f;  // 距窗口顶端多少像素内即判定"要显示"

// ---- 微动效（Phase 6）----
// 页面淡入：仅在纹理"首次出现"的那一帧从 0 渐显，避免新渲染的页生硬跳出。
struct PageFade { bool seen = false; float alpha = 1.0f; };
std::vector<PageFade> g_page_fade;
constexpr float kPageFadeSec = 0.18f;

// 自绘滚动指示条：滚动时浮现、静止 1.2s 后渐隐（画布子窗口是 NoScrollbar）。
float  g_scroll_ind_alpha = 0.0f;
float  g_scroll_ind_last_y = 0.0f;
double g_last_scroll_time = -1.0;
constexpr double kScrollIndHoldSec = 1.2;

// ---- 打开文件对话框（Phase 6）----
// 真正的 GetOpenFileNameW 调用放在**帧与帧之间**执行（见主循环）：它自带模态消息循环，
// 在 ImGui 一帧中途调用会让后端在同一帧里处理窗口消息，可能打乱 ImGui 内部状态。
bool    g_request_open_dialog = false;
wchar_t g_open_path_buf[32768] = {};

// ---------------- 用户偏好持久化（Phase 6） ----------------
// 读：缺键/非法值走默认，读后一律钳制到合法区间（手改 ini 也不致于把界面弄坏）。
void load_prefs() {
    g_prefs.ui_scale = std::clamp(
        lr::read_ini_float_ex(g_ini_path, L"ui", L"UiScale", 1.0f), 0.8f, 1.5f);
    g_prefs.theme = std::clamp(lr::read_ini_int_ex(g_ini_path, L"ui", L"Theme", 0), 0, 2);
    g_prefs.auto_hide_toolbar =
        lr::read_ini_int_ex(g_ini_path, L"ui", L"AutoHideToolbar", 1) != 0;
    g_prefs.motion = lr::read_ini_int_ex(g_ini_path, L"ui", L"Motion", 1) != 0;
    g_prefs.gap_percent = std::clamp(
        lr::read_ini_float_ex(g_ini_path, L"ui", L"GapPercent", 1.3f), 0.0f, 6.0f);
    g_prefs.cache_mb = std::clamp(
        lr::read_ini_int_ex(g_ini_path, L"cache", L"BudgetMB", kCacheBudgetDefaultMB), 128, 2048);
    g_user_scale = g_prefs.ui_scale;
}

void save_prefs() {
    lr::write_ini_float(g_ini_path, L"ui", L"UiScale", g_prefs.ui_scale);
    lr::write_ini_int(g_ini_path, L"ui", L"Theme", g_prefs.theme);
    lr::write_ini_int(g_ini_path, L"ui", L"AutoHideToolbar", g_prefs.auto_hide_toolbar ? 1 : 0);
    lr::write_ini_int(g_ini_path, L"ui", L"Motion", g_prefs.motion ? 1 : 0);
    lr::write_ini_float(g_ini_path, L"ui", L"GapPercent", g_prefs.gap_percent);
    lr::write_ini_int(g_ini_path, L"cache", L"BudgetMB", g_prefs.cache_mb);
}

inline float gap_ratio_pref() { return g_prefs.gap_percent / 100.0f; }

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

// ---------------- 视图变换 / 页尺寸（Phase 5） ----------------

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

// ---------------- 阅读位置与书签（Phase 5） ----------------

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
    (void)lr::save_state(g_state_path, g_state);  // 书签在增删时已写入 g_state，这里不覆盖
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
    g_doc_key = 0;
    g_outline.clear();
    g_raw_sizes.clear();
    g_page_fade.clear();
    g_restore_pending = false;
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
    g_doc_key = lr::document_key(path);  // 0 = 取不到属性（不存在等）

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
    // F11/F1/Ctrl+O/Ctrl+, 等全局快捷键统一在 draw_shell 处理（任何状态下都可用）

    // ---- Phase 5 快捷键 ----
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) rotate_view(90);           // 旋转 +90°
    if (ImGui::IsKeyPressed(ImGuiKey_D, false))                            // 双页对开（书籍模式）
        g_canvas.set_spread(!g_canvas.state().spread);
    if (ImGui::IsKeyPressed(ImGuiKey_I, false)) toggle_color_mode(1);      // 反色
    if (ImGui::IsKeyPressed(ImGuiKey_E, false)) toggle_color_mode(2);      // 护眼
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {                          // 侧栏（目录/书签/缩略图）
        g_show_sidebar = !g_show_sidebar;
        if (g_show_sidebar) g_sidebar_tab = 0;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_B, false)) toggle_bookmark_current(); // 当前页书签增删
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
    dl->AddRectFilled(pmin, pmax, failed ? g_pal.failed : g_pal.placeholder);
    dl->AddRect(pmin, pmax, failed ? g_pal.failed_border : g_pal.placeholder_border);
    // 失败占位提示"点击重试"（Phase 4）：命中测试在 draw_canvas_area 里做（见 retry 注释）
    const char* txt = failed ? "渲染失败 · 点击重试"
                             : (s.status == lr::PageStatus::Loading ? "载入中…" : "");
    if (txt[0] != '\0') {
        const ImVec2 ts = ImGui::CalcTextSize(txt);
        dl->AddText(ImVec2((pmin.x + pmax.x - ts.x) * 0.5f, (pmin.y + pmax.y - ts.y) * 0.5f),
                    g_pal.placeholder_text, txt);
    }
}

// 画布右键菜单在 draw_canvas_area 内触发（点击命中区属于画布），内容见下方定义。
void draw_canvas_context_menu();
// 自绘滚动指示条（画布子窗口是 NoScrollbar，滚动反馈自己画）。
void draw_scroll_indicator(ImDrawList* dl, const ImVec2& origin, const ImVec2& size);

void draw_canvas_area() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##canvas", ImVec2(0, -px(kStatusBarH)), false,
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
    // 右键按下/抬起计数（F3 诊断用，Phase 6）：排查"右键菜单没反应"时，
    // 一眼区分是输入根本没到、还是被别的条件挡住（配合 anyItem/hover 读数）。
    if (ImGui::GetIO().MouseClicked[ImGuiMouseButton_Right]) ++g_dbg_r_down;
    if (ImGui::GetIO().MouseReleased[ImGuiMouseButton_Right]) ++g_dbg_r_up;

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

    // 设置/帮助窗口打开时画布不再吞键盘（否则方向键会同时翻页与移动焦点）。
    if (!g_open_jump && !g_open_password && !g_show_settings && !g_show_help)
        handle_canvas_input(origin, size, hovered);

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
            // 页面投影：右下偏移的半透明矩形，给纸面一点立体感（微动效/美观，Phase 6）
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

    draw_scroll_indicator(dl, origin, size);

    // 右键菜单（Phase 6）：命中区是画布，命令集中在上下文菜单里，不往界面上堆按钮。
    draw_canvas_context_menu();

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

// ---------------- 引导 / 状态页（Phase 1/2 保留；Phase 6 配色随主题，排版重排） ----------------
constexpr const char* kFormatsLine = "支持 PDF · EPUB · MOBI · FB2 · CBZ · XPS · 图片(PNG/JPG/GIF/BMP/TIFF)";

// 引导页文字颜色**必须随主题**：早期写死浅色，浅色主题下几乎看不见（Phase 6 修正）。
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
            // Phase 5：视图变换与侧栏
            ImGui::Text("rot: %d  color: %d  spread: %d",
                        g_rotation, g_color_mode, g_canvas.state().spread ? 1 : 0);
            const lr::DocRecord* rec = g_state.find(g_doc_key);
            ImGui::TextDisabled("outline: %d  bookmarks: %d  sidebar: %d tab %d",
                                static_cast<int>(g_outline.size()),
                                rec ? static_cast<int>(rec->bookmarks.size()) : 0,
                                g_show_sidebar ? 1 : 0, g_sidebar_tab);
            // 缓存统计（Phase 4）：驻留字节/预算、驻留页数、累计逐出页数
            const lr::CacheStats cs = g_renderer->cache_stats();
            ImGui::Text("cache: %.1f / %.0f MB  pages %d  evict %d",
                        (double)cs.used_bytes / 1048576.0,
                        (double)cs.budget_bytes / 1048576.0,
                        cs.resident_pages, cs.evictions);
            ImGui::TextDisabled("preload dir %d  (+1 下 / -1 上 / 0 两侧)",
                                g_scroll_dir);
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

// ---------------- 侧栏（Phase 5）：目录 / 书签 / 缩略图 ----------------

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
            g_canvas.scroll_to_page(it.page, 0.0f);
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
            g_canvas.scroll_to_page(b.page, 0.0f);
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

    // 只请求当前页附近一段（±40 页）：既够滚动浏览，又不至于一次性渲染整本书。
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
        if (ImGui::Selectable(label, i == cur)) g_canvas.scroll_to_page(i, 0.0f);
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

void draw_sidebar() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8), px(8)));
    ImGui::BeginChild("##sidebar", ImVec2(px(kSidebarWidthPx), -px(kStatusBarH)), false,
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

// ---------------- 密码对话框（Phase 5） ----------------
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

// 输入法关联随文本输入激活状态切换（落实 ADR-028 的后续要求）：
// 阅读窗口平时脱离输入法（让字母/数字快捷键生效）；密码框/文本输入激活时临时关联回来，
// 否则中文打不进去。
void update_ime_association() {
    const bool want = ImGui::GetIO().WantTextInput;
    if (want == g_ime_attached) return;
    g_ime_attached = want;
    ImmAssociateContext(g_hwnd, want ? g_saved_ime : nullptr);
}

// ================= Phase 6：界面外壳（顶栏 / 菜单 / 右键菜单 / 设置 / 帮助 / 微动效） =================
//
// 设计原则（对应用户要求，也写进 ADR-045）：
//   · **命令集中、按钮克制**：可点控件只出现在顶栏一簇与各种菜单里；状态栏只放只读信息。
//   · **右键即全部**：画布右键菜单覆盖缩放/视图/导航/书签/侧栏/设置/帮助，不靠记快捷键。
//   · **图标 + 文字双轨**：图标取自系统 Segoe MDL2；图标字体缺失时自动退化为文字标签。
//   · 视图类命令（列/对开/旋转/配色）都是**带勾选的语义项**，而不是一排看不出状态的按钮。

// ---- 小工具 ----

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

// 扁平按钮配色（顶栏/工具条，Phase 6 ADR-045）：
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

// 弹出菜单统一观感（Phase 6，ADR-045）：更宽松的内边距 + 更高的菜单项。
// 注意：Selectable/MenuItem 的高度 = 文字高（**不吃 FramePadding**），项高完全由
// ItemSpacing.y 决定 —— 悬停高亮向外扩半个间距，恰好铺满整个间距，故放大间距即放大项高，
// 且相邻项的高亮连续、无点击死区。
void push_popup_style() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(kPopupPadXY), px(kPopupPadXY)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), px(kMenuItemGapY)));
}
inline void pop_popup_style() { ImGui::PopStyleVar(2); }

void set_sidebar(bool on, int tab) {
    g_show_sidebar = on;
    if (on && tab >= 0) g_sidebar_tab = tab;
}

int  zoom_percent() { return static_cast<int>(std::lround(g_canvas.effective_zoom() * 100.0f)); }
void zoom_at_center(float factor) {
    g_canvas.zoom_by(factor, g_canvas.viewport_w() * 0.5f, g_canvas.viewport_h() * 0.5f);
}
void zoom_set(float z) {
    g_canvas.set_zoom(z, g_canvas.viewport_w() * 0.5f, g_canvas.viewport_h() * 0.5f);
}
void open_jump_popup() {
    g_open_jump = true;
    g_jump_page = g_canvas.current_page() + 1;
}

// 页面间距（设置项）变更后立即下发；会失效布局缓存，故只在真正变化时调用。
void apply_gap_pref() {
    g_canvas.set_margin_gap(px(kCanvasMarginPx), gap_ratio_pref());
    g_canvas_scale = ui_scale();
}

// ---- 菜单内容（顶栏主菜单与画布右键菜单共用） ----

void draw_zoom_menu_contents() {
    if (menu_item(kIcExpand, "适合宽度", "F", g_canvas.state().fit_width))
        g_canvas.fit_to_width();
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
    if (menu_item(kIcHome, "首页", "Home", false, rd)) g_canvas.scroll_to_page(0, 0.0f);
    if (menu_item(kIcPrev, "上一页 / 上一行", "←", false, rd)) scroll_by_rows(-1);
    if (menu_item(kIcNext, "下一页 / 下一行", "→", false, rd)) scroll_by_rows(+1);
    if (menu_item(kIcArrowDown, "末页", "End", false, rd))
        g_canvas.scroll_to_page(g_canvas.page_count() - 1, 0.0f);
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

// ---- 自绘滚动指示条 ----
void draw_scroll_indicator(ImDrawList* dl, const ImVec2& origin, const ImVec2& size) {
    const float max_sy = g_canvas.max_scroll_y();
    float target = 0.0f;
    if (max_sy > 1.0f) {
        const float sy = g_canvas.state().scroll_y;
        if (std::fabs(sy - g_scroll_ind_last_y) > 0.5f) {
            g_scroll_ind_last_y = sy;
            g_last_scroll_time = ImGui::GetTime();
        }
        if (g_last_scroll_time > 0.0 &&
            (ImGui::GetTime() - g_last_scroll_time) < kScrollIndHoldSec)
            target = 1.0f;
    }
    if (!g_prefs.motion) {
        g_scroll_ind_alpha = target;
    } else {
        const float dt = ImGui::GetIO().DeltaTime;
        g_scroll_ind_alpha += (target - g_scroll_ind_alpha) * std::min(1.0f, dt * 12.0f);
    }
    if (max_sy <= 1.0f || g_scroll_ind_alpha < 0.02f) return;

    const float track_h = std::max(1.0f, size.y - px(20));
    const float content = std::max(1.0f, g_canvas.content_height_px());
    const float frac = std::clamp(size.y / content, 0.05f, 1.0f);
    const float h = std::max(px(26.0f), track_h * frac);
    const float t = std::clamp(g_canvas.state().scroll_y / max_sy, 0.0f, 1.0f);
    const float y = origin.y + px(10.0f) + t * (track_h - h);
    const float w = px(4.0f);
    const float x = origin.x + size.x - px(9.0f) - w;
    const int a = static_cast<int>(g_scroll_ind_alpha * 170.0f);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h),
                      (g_pal.accent & 0x00FFFFFFu) | (static_cast<ImU32>(a) << 24), w * 0.5f);
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

// ---- 顶栏 ----
void draw_top_bar() {
    const float bar_h = px(kTopBarH);
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
    const float y = (bar_h - h) * 0.5f;
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
                                   (bar_h - ImGui::GetTextLineHeight()) * 0.5f));
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
// 小屏/低分辨率下也不会超出屏幕（Phase 6）。
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
        // 会被挤到滚动区外，用户得先滚动才能关闭窗口（Phase 6）。
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

// ---- 打开文件对话框（帧间调用） ----
void open_file_dialog_now() {
    g_open_path_buf[0] = L'\0';
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter =
        L"支持的文档\0*.pdf;*.epub;*.mobi;*.fb2;*.cbz;*.xps;*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0"
        L"PDF\0*.pdf\0电子书\0*.epub;*.mobi;*.fb2\0压缩图集\0*.cbz\0图片\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0"
        L"所有文件\0*.*\0";
    ofn.lpstrFile = g_open_path_buf;
    ofn.nMaxFile = ARRAYSIZE(g_open_path_buf);
    ofn.lpstrTitle = L"打开文档";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) request_open_document(g_open_path_buf);
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
    update_toolbar_visibility();  // 顶栏自动隐藏（Phase 6）

    // 全局快捷键：任何状态下都可用（设置/帮助/帮助窗口都能开）；有文本输入时让位。
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

    // 顶栏（阅读态可自动隐藏；其余状态恒显示——菜单/设置/帮助是唯一入口）
    if (top_bar_should_show()) draw_top_bar();

    // 非阅读态把内容区铺成画布同色的底，与阅读态的视觉语言一致（否则是一大片窗口底色）。
    if (g_doc.kind != UiDoc::Kind::Reading) {
        const ImVec2 p0 = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p0, ImVec2(p0.x + avail.x, p0.y + avail.y), g_pal.backdrop);
    }

    switch (g_doc.kind) {
    case UiDoc::Kind::None:
        draw_drop_guide();
        break;
    case UiDoc::Kind::Opening:
        draw_opening();
        break;
    case UiDoc::Kind::Reading:
        if (g_show_sidebar) {
            draw_sidebar();
            ImGui::SameLine(0.0f, 0.0f);
        }
        draw_canvas_area();
        draw_status_bar();
        break;
    case UiDoc::Kind::Failed:
        draw_failed();
        break;
    case UiDoc::Kind::NeedsPassword:
        draw_failed();  // 背景铺失败页，密码框浮在其上
        break;
    case UiDoc::Kind::Rejected:
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

// ---------------- 窗口过程 ----------------
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 切换输入语言（Alt+Shift / Win+Space）时，系统会给窗口**重新关联**输入法，
    // 把 ImmAssociateContext(hwnd, nullptr) 的脱离顶掉 —— 这里按当前状态再脱/再关联一次。
    // 必须放在 ImGui 后端处理器**之前**：后端会消费 WM_INPUTLANGCHANGE 并 return 1。
    if (msg == WM_INPUTLANGCHANGE)
        ImmAssociateContext(hwnd, g_ime_attached ? g_saved_ime : nullptr);
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
        save_reading_state();  // 退出时落盘阅读位置（书签已实时落盘）
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
    g_state_path = lr::exe_dir() + L"reader_state.bin";
    g_state = lr::load_state(g_state_path);  // 阅读位置/书签（损坏则安全忽略为空）
    load_prefs();  // 用户偏好（界面缩放/主题/动效/间距/缓存预算）

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // 应用图标（Phase 6）：资源里同时提供了多尺寸，窗口与任务栏各取所需。
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kAppIconId),
                                             IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kAppIconId),
                                               IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON),
                                               LR_SHARED));
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

    // 页缓存字节预算（Phase 4）：来自 [cache] BudgetMB（设置界面可调）；渲染层再钳制。
    g_renderer->set_cache_budget(
        static_cast<std::size_t>(g_prefs.cache_mb) * 1024ull * 1024ull);

    g_ui.initialize(g_hwnd);
    DragAcceptFiles(g_hwnd, TRUE);

    // 让阅读窗口脱离输入法（ADR-028）。
    // 输入法启用时，Windows 会把字母/数字键的 WM_KEYDOWN 换成 VK_PROCESSKEY(0xE5)，
    // 而 ImGui 的 win32 后端不映射这个键码 → 这些键在 ImGui 里**完全不置位**，
    // 所有字母/数字快捷键（1~4 切列、F 回 fit-width、G 跳页…）静默失效。
    // Phase 5 起：保存默认输入法上下文，平时脱离；仅在文本输入激活（密码框等）时关联回来，
    // 使中文可输入（见 update_ime_association，落实 ADR-028 的后续要求）。
    g_saved_ime = ImmAssociateContext(g_hwnd, nullptr);
    g_ime_attached = false;

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

        // 「打开文档…」：模态文件对话框自带消息循环，放在**帧与帧之间**执行，
        // 绝不在 ImGui 一帧中途调用（否则窗口消息会在半帧状态下被后端处理）。
        if (g_request_open_dialog) {
            g_request_open_dialog = false;
            open_file_dialog_now();
        }
    }
quit:
    g_renderer.reset();  // 必须先于 gfx 释放：纹理依赖 D3D11 设备
    g_ui.shutdown();
    g_gfx.shutdown();
    return 0;
}
