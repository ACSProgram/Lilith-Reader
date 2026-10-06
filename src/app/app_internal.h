// app_internal.h — Lilith Reader 应用层内部共享声明
//
// 为什么是普通头而不是 C++20 模块：app 层是 Win32 / D3D11 / ImGui 直连代码，
// 它的价值在于"把外部系统接进来"，不在于被复用或单测；真正需要隔离与可测的
// 是纯逻辑层（utils / document / canvas / render / state），那些已经是模块。
// 因此 app 层用「普通 TU + 本内部头」组织，模块边界一条也没放松。
//
// 文件划分（每个 TU 单一职责，见各 .cpp 文件头）：
//   main.cpp      入口：wWinMain、窗口类/创建、消息循环、窗口过程、窗口状态校验
//   platform.cpp  平台地基：D3D11、ImGui 引导、DPI/界面缩放、主题、图标与字体、
//                 输入法关联、用户偏好 ini、全屏
//   session.cpp   会话与状态：文档状态机、阅读位置/书签、视图变换、画布输入与渲染请求
//   ui.cpp        全部绘制：外壳/顶栏/状态栏/画布/侧栏/菜单/弹窗/设置/帮助/调试浮层
//
// 本头只放**跨 TU 共享**的状态与函数声明；只被单个 TU 用到的常量/类型留在各 .cpp。
// 依赖方向：main → { platform, session, ui }；ui → { platform, session }；session → platform。

#pragma once

#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commdlg.h>      // GetOpenFileNameW：菜单「打开文档…」
#include <imm.h>          // ImmAssociateContext：让阅读窗口脱离输入法（ADR-028）
#include <d3d11.h>
#include <dxgi.h>

// 标准库头**必须全部排在下面的 import 之前**：MSVC 下在 import 声明之后再文本包含 STL
// 头，会让 STL 被"文本包含"与"std 模块"两条路各展开一次，报 C2572（重定义默认参数）。
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

import lilithreader.utils;
import lilithreader.document;
import lilithreader.canvas;
import lilithreader.render;
import lilithreader.reader_state;
import lilithreader.persist;  // 异步持久化服务（ADR-082）
import lilithreader.log;      // Phase 7：轻量日志（自实现，不引入 spdlog，ADR-077）

#include "imgui.h"
#include "tone.h"      // 纸张方案 → chrome 色调（纯函数，自身不依赖 ImGui/Win32）
#include "page_map.h"  // 页面 pt ↔ 屏幕点（纯函数，含旋转折算；可单测）

// 链接依赖（app 层 TUs 共用；放在头里避免每个 .cpp 重复声明）
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "comdlg32.lib")

namespace lr::app {

// ============================================================================
// 标识与界面度量常量（100% 缩放值，屏幕像素；用前过 px()）
// ============================================================================

inline constexpr wchar_t kWindowClass[] = L"LilithReaderWnd";
inline constexpr wchar_t kWindowTitle[] = L"Lilith Reader";
inline constexpr int     kAppIconId = 101;   // 见 src/app/app.rc
inline constexpr int     kUiFontResId = 102; // UI 字体子集（RCDATA），见 src/app/app.rc

inline constexpr float  kUiFontBasePx = 20.0f;    // UI 基准字号（×FontScaleMain×FontScaleDpi 后为实际像素）
inline constexpr float  kScrollStepPx = 120.0f;   // 每格滚轮滚动的屏幕像素
inline constexpr float  kKeyScrollPx = 80.0f;     // 方向键每次滚动的屏幕像素
inline constexpr float  kZoomStep = 1.15f;        // 每格 Ctrl+滚轮 / 每次 +/- 的缩放倍率
inline constexpr double kZoomDebounceSec = 0.15;  // 缩放稳定后触发高清重渲染的等待时间
// 状态栏高度：**必须容得下 CJK 字体的墨迹盒**，而不是只装名义行高。
// ImGui 的 `GetTextLineHeight()` = 字号（1.0 em），而微软雅黑/思源黑体的 ascent+descent
// ≈ 1.32 em（含汉字上下的气口），故 20px 基准字号的实际墨迹约 26px。取 34 是为了让
// 墨迹上下各留 ~4px 基准像素的呼吸空间（早期取 30 时下缘只剩 1~2px，实测反馈"字太贴边"）。
inline constexpr float  kStatusBarH = 34.0f;      // 底部状态栏高度
inline constexpr float  kTopBarH = 40.0f;         // 顶部工具栏高度
inline constexpr float  kCanvasMarginPx = 18.0f;  // 画布四周留白（屏幕像素，随 DPI 缩放）
// 页/列间距：占**列宽**的比例（文档空间，ADR-029），**刻意不过 px()**（与页面同比例缩放）。
inline constexpr float  kCanvasGapRatio = 0.013f;

// 界面间距基准：集中命名，避免各处手调出的"看起来差不多的不同值"（ADR-045）。
inline constexpr float  kChromePadX = 12.0f;     // 顶栏/状态栏左右内边距
inline constexpr float  kChromeGapX = 4.0f;      // 顶栏图标按钮之间的水平间距
inline constexpr float  kPopupPadXY = 8.0f;      // 弹出菜单四周内边距
inline constexpr float  kMenuItemGapY = 10.0f;   // 弹出菜单项之间/项高（Selectable 不吃 FramePadding）
// 图标字形与汉字的基线差异（实测，ADR-045）：正值 = 向下微调，使图标与文字对齐。
inline constexpr float  kIconGlyphOffsetY = 4.0f;

// ---- 文本交互（Phase 8）----
// 判定"点击"还是"拖拽"的位移阈值（100% 缩放值）：超过它才算拖拽。
// 4px 是桌面惯例量级（小于它人手几乎不可能"有意拖动"）。
inline constexpr float  kClickSlopPx = 4.0f;
// 选区高亮与搜索命中高亮的透明度（0~1）。
inline constexpr float  kSelectAlpha = 0.34f;
inline constexpr float  kFindAlpha   = 0.30f;
// 状态栏提示（toast）的显示时长（秒）。
inline constexpr double kToastSec = 2.6;
// 搜索的**输入防抖**：停止打字多久后自动发起检索（秒）。
// 为什么不逐键检索、也不无限等回车：逐键检索在千页文档上会把渲染线程持续占满；
// 只认回车则中文输入法下"第一次回车是上屏"会让人以为搜索没反应（用户实测）。
// 0.4s 是"打完一个词到想按回车"之间的自然停顿量级，短于它不算打完，长于它用户已在等结果。
inline constexpr double kSearchDebounceSec = 0.4;

// ---- 视图动效（Phase 6 收尾，ADR-047）----
// 统一用**一阶滞后**（帧率无关的指数趋近）而不是补间：无过冲、必然收敛、不需要维护速度状态，
// 且连续输入时自然叠加（补间则每次输入都要重排时间轴）。速率单位 1/s，时间常数 = 1/rate：
// 22/s ≈ 45ms，95% 收敛约 135ms —— 这个量级"跟得上手"又把台阶抹平了。
inline constexpr float  kScrollSmoothRate = 22.0f;  // 滚轮 / 方向键 / 整屏滚动
inline constexpr float  kZoomSmoothRate   = 26.0f;  // 缩放插值
inline constexpr float  kTopBarAnimRate   = 20.0f;  // 顶栏自动隐藏的滑入/滑出
inline constexpr float  kSidebarAnimRate  = 18.0f;  // 侧栏的滑入/滑出
inline constexpr float  kWindowScaleFrom  = 0.96f;  // 设置窗口出现/消失时的起始缩放（0.96 → 1.0）
inline constexpr float  kPopupSlidePx     = 10.0f;  // 弹出对话框出现/消失时的滑落距离（px，100% 缩放值）
inline constexpr float  kMotionEpsPx      = 0.5f;   // 动画收敛阈值（像素/倍率），到阈值即吸附到目标

// 一阶趋近（帧率无关的指数趋近）：无过冲、必然收敛，不需要维护速度状态。
inline float approach(float cur, float target, float rate, float dt) {
    return target + (cur - target) * std::exp(-rate * dt);
}

// ---- 一次性开合动画：定时补间 + 三次缓出（ADR-061）----
// 与上面那类"连续输入驱动"的动效（滚动/缩放，一阶滞后）**刻意不同**：开合是**单向、有明确
// 终点**的一次性过渡，用定时补间更"利落" —— 时长确定、末端干脆；一阶滞后收敛到 99% 之后
// 还要再走一个时间常数，观感上"软绵绵"（人工反馈"再利落一点"）。
inline constexpr float  kOpenCloseDur = 0.16f;   // 开合过渡时长（秒）

// 三次缓出：起步快、末端缓，读起来是"干脆地停住"。
inline float ease_out_cubic(float x) {
    const float u = 1.0f - x;
    return 1.0f - u * u * u;
}

// 一次性开合动画的进度。value ∈ [0,1]：0 = 完全收起（可销毁），1 = 静止展开。
struct ToggleAnim {
    float value = 0.0f;        // 当前（已缓出）进度
    float from = 0.0f;         // 本次过渡的起点
    float elapsed = 0.0f;
    bool  active = false;
    bool  target_open = false;

    // 推进一帧；返回 true = 本帧仍需绘制（value > 0）。
    bool step(float dt, bool want_open, bool motion) {
        const float target = want_open ? 1.0f : 0.0f;
        if (!motion) { value = target; active = false; target_open = want_open; return value > 0.0f; }
        if (active && target_open != want_open) { from = value; elapsed = 0.0f; }  // 中途反向：从当前值重起
        if (!active) {
            if (value == target) return value > 0.0f;   // 已静止在目标
            from = value; elapsed = 0.0f; active = true;
        }
        target_open = want_open;
        elapsed += dt;
        float x = elapsed / kOpenCloseDur;
        if (x >= 1.0f) { x = 1.0f; active = false; }
        value = from + (target - from) * ease_out_cubic(x);
        return value > 0.0f;
    }
};

// ---- 图标字形（Segoe MDL2 Assets，PUA 码位；缺失时按钮退化为文字标签）----
// **只列当前实际用到的字形**：多列一个就多一份图集体积与一处启动校验点。
// 新增菜单/按钮若需新图标，在此加常量并同步 platform.cpp 的 kIconGlyphs。
inline constexpr const char* kIcMenu       = "\xEE\x9C\x80";  // U+E700 汉堡菜单
inline constexpr const char* kIcPane       = "\xEE\xA2\xA0";  // U+E8A0 侧栏面板
inline constexpr const char* kIcZoomIn     = "\xEE\xA2\xA3";  // U+E8A3 放大
inline constexpr const char* kIcZoomOut    = "\xEE\x9C\x9F";  // U+E71F 缩小
inline constexpr const char* kIcExpand     = "\xEE\x9D\x80";  // U+E740 适合宽度/展开
inline constexpr const char* kIcFullscreen = "\xEE\x87\x99";  // U+E1D9 全屏
inline constexpr const char* kIcStar       = "\xEE\x9C\xB4";  // U+E734 书签
inline constexpr const char* kIcSettings   = "\xEE\x9C\x93";  // U+E713 设置
inline constexpr const char* kIcHelp       = "\xEE\xA2\x97";  // U+E897 帮助
inline constexpr const char* kIcRotate     = "\xEE\x9E\xAD";  // U+E7AD 旋转
inline constexpr const char* kIcClose      = "\xEE\x9C\x91";  // U+E711 关闭
inline constexpr const char* kIcArrowDown  = "\xEE\x9D\x8B";  // U+E74B 下箭头
inline constexpr const char* kIcNext       = "\xEE\x9C\xAA";  // U+E72A 下一页
inline constexpr const char* kIcPrev       = "\xEE\x9C\xAB";  // U+E72B 上一页
inline constexpr const char* kIcHome       = "\xEE\xA0\x8F";  // U+E80F 首页
inline constexpr const char* kIcList       = "\xEE\xA3\xBD";  // U+E8FD 目录（列表）
inline constexpr const char* kIcGrid       = "\xEE\xA0\x8A";  // U+E80A 网格
inline constexpr const char* kIcPalette    = "\xEE\x9E\x90";  // U+E790 配色
inline constexpr const char* kIcBook       = "\xEE\xA0\xAD";  // U+E82D 双页/书
inline constexpr const char* kIcDoc        = "\xEE\xA2\xA5";  // U+E8A5 单页
inline constexpr const char* kIcOpenFile   = "\xEE\xA3\xA5";  // U+E8E5 打开文件
inline constexpr const char* kIcSearch     = "\xEE\x9C\xA1";  // U+E721 搜索
inline constexpr const char* kIcFolder     = "\xEE\xA2\xB7";  // U+E8B7 文件夹（阅读数据的位置行）
inline constexpr const char* kIcTrash      = "\xEE\x9D\x8D";  // U+E74D 删除（阅读数据行尾）
// Phase 8：文本/图片/链接
inline constexpr const char* kIcCopy       = "\xEE\xA3\x88";  // U+E8C8 复制
inline constexpr const char* kIcImage      = "\xEE\xA2\xB9";  // U+E8B9 图片
inline constexpr const char* kIcLink       = "\xEE\x9C\x9B";  // U+E71B 链接

// ============================================================================
// 平台层（platform.cpp）
// ============================================================================

// ---- 界面缩放（DPI × 用户缩放）----
// 所有"屏幕像素"设计值都以 100%（96dpi）为基准，运行时统一过 px()。字体不在这里乘，
// 而是交给 ImGui 的 style.FontScaleDpi / FontScaleMain（1.92+ 动态字体重栅格化）。
inline float g_dpi_scale = 1.0f;    // 自动：窗口所在显示器 DPI（WM_DPICHANGED 时刷新）
inline float g_user_scale = 1.0f;   // 手动：「设置 → 界面缩放」（80%~150%）
inline float ui_scale() { return g_dpi_scale * g_user_scale; }
inline float px(float base) { return base * ui_scale(); }

inline HWND g_hwnd = nullptr;

// ---- 主题调色板 ----
// chrome 表现由两个正交维度决定（ADR-067/068）：
//   · **界面主题**（浅/深/跟随系统）—— 用户偏好，管顶栏/菜单/设置窗的明暗；
//   · **纸张方案**（原色/深色纸张/暖色）—— 管页面，同时通过 g_tone 决定 chrome 的色调
//     （跟随系统时深色纸张还会令 chrome 变暗；暖色方案旋到米黄纸面色相）。
// 这里只定义**两套中性底**（kPalLight / kPalDark）；实际用到的颜色由 g_tone 现场派生 ——
// 手写 4~5 套相近调色板既难保持一致、又容易漏改某几个色，派生则天然同步，
// 且"保持亮度/换强调色/保语义色"等规则集中在一处（见 tone.h）。
// 画布/顶栏/状态栏是自绘的，故颜色放进可切换的调色板 g_pal；ImGui 控件配色另行设置。
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

extern const Palette kPalLight;
extern const Palette kPalDark;

inline Palette g_pal = kPalLight;
inline bool g_dark_theme = false;      // 当前是否为深色 chrome
inline Tone g_tone{};                  // 当前纸张方案的色调（inactive = 原色，见 tone.h）
inline bool g_theme_applied = false;   // g_pal / ImGui 颜色是否已按当前主题应用
inline bool g_icons_ok = false;        // 图标字形是否已成功合并进字体（否则退化为文字标签）

// 按当前纸张方案的色调派生一个颜色（保持 alpha）。ui.cpp 里自绘的文字色/强调色也走它，
// 否则会出现"调色板暖了、这几处还是冷灰"这种局部漏改（ADR-068）。
[[nodiscard]] ImU32 tone_apply(ImU32 c);
[[nodiscard]] ImVec4 tone_apply(ImVec4 c);

void apply_theme_colors();
void sync_theme(int scheme);           // 主题偏好 + 纸张方案 → chrome 明暗与色调（帧首调用）

// ---- D3D11（RAII） ----
struct Graphics {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    DXGI_SWAP_CHAIN_DESC sc_desc{};
    UINT cur_w = 0, cur_h = 0;   // 当前后备缓冲尺寸：尺寸未变时跳过重建（避免无谓闪烁）
    // 显示设备已丢失（Present 返回 DXGI_ERROR_DEVICE_REMOVED/RESET）：此后不再呈现。
    // 只置标记、不做设备重建 —— 半途重建 device/swapchain 还必须让 ImGui 后端重新初始化并
    // 重建字体纹理，失败面比收益大。置标记后由界面如实告知用户，日志里保留 HRESULT 供追溯。
    bool device_lost = false;

    // D3D 资源是**独占所有**：拷贝会得到两个"所有者"，析构时双重释放。
    Graphics() = default;
    Graphics(const Graphics&) = delete;
    Graphics& operator=(const Graphics&) = delete;

    bool initialize(HWND hwnd);
    bool create_rtv();
    void resize(UINT w, UINT h);
    void render_frame();
    void shutdown();   // 幂等：可重复调用
};
inline Graphics g_gfx;

// 待处理的客户区尺寸（WM_SIZE 只记录、不立即重建 swapchain）。
// 为什么延到帧首：WM_SIZE 常在 SetWindowPos / SetWindowPlacement 过程中**同步**到达，
// 而这类调用可能发生在 ImGui 一帧中途（命令派发时）。若当场 ResizeBuffers，本帧的绘制数据
// 仍是按旧尺寸排布的，却被画进新尺寸的后备缓冲 → 呈现为一帧拉伸/闪烁；全屏切换时尤其明显。
// 改为帧首统一应用后：每帧至多重建一次、与 ImGui 视口尺寸严格一致，且多次 WM_SIZE 自动合并。
inline bool g_resize_pending = false;
inline UINT g_resize_w = 0, g_resize_h = 0;

// ---- ImGui 引导（RAII） ----
struct ImGuiRaii {
    bool win32 = false, dx11 = false, ctx = false;

    void initialize(HWND hwnd);
    void new_frame();
    void shutdown();
};
inline ImGuiRaii g_ui;

void apply_ui_scale();      // 从留底的基准样式重算界面度量与字体缩放
void refresh_dpi_scale();   // 读取窗口所在显示器 DPI；变化时重算样式（帧间调用）

// ---- 用户偏好（落盘 exe 同目录 LilithReader.ini）----
// 只有真正需要跨会话记忆的量放这里；阅读位置/书签仍归 reader_state.bin（ADR-034）。

// 「智能匹配」三态（ADR-062）：文件内容指纹命中但路径变了时的处置方式。
// 关 = 只认路径（旧行为）；询问 = 弹确认框（默认沿用，可选"从头开始"）；自动 = 静默沿用。
inline constexpr int kSmartMatchOff  = 0;
inline constexpr int kSmartMatchAsk  = 1;
inline constexpr int kSmartMatchAuto = 2;
struct UiPrefs {
    float ui_scale = 1.0f;             // [ui] UiScale      0.80~1.50
    int   theme = 0;                   // [ui] Theme        0 跟随系统 / 1 浅色 / 2 深色
    bool  auto_hide_toolbar = true;    // [ui] AutoHideToolbar
    bool  motion = true;               // [ui] Motion       页面淡入 / 滚动指示条渐隐
    float gap_percent = kCanvasGapRatio * 100.0f;  // [ui] GapPercent   页面间距（列宽百分比）0~6
    int   resource_tier = 1;           // [cache] ResourceTier 0低 / 1中 / 2高
    int   smart_match = kSmartMatchAsk;// [reading] SmartMatch  0 关 / 1 询问 / 2 自动（ADR-062）
};
inline UiPrefs g_prefs;
inline bool g_apply_scale_pending = false;  // 界面缩放改动：样式留到下一帧首应用
inline std::wstring g_ini_path;
inline std::wstring g_state_path;           // reader_state.bin（会话层使用，入口处赋值）

void load_prefs();
void save_prefs();
inline float gap_ratio_pref() { return g_prefs.gap_percent / 100.0f; }

// ---- 输入法关联（ADR-028/040）----
// 平时脱离输入法（让字母/数字快捷键生效）；文本输入激活时临时关联回来（中文可输入）。
inline HIMC g_saved_ime = nullptr;
inline bool g_ime_attached = false;
void update_ime_association();

// ---- 剪贴板（Phase 8）----
// 文本走 CF_UNICODETEXT（内部 UTF-8 → UTF-16，避免中文变乱码）；
// 图片走 CF_DIB（32bpp BGRA，自下而上）。返回 false = 写剪贴板失败（被别的进程占用）。
bool set_clipboard_text(const std::string& utf8);
bool set_clipboard_image_rgba(int w, int h, const std::uint8_t* rgba);

// ---- 全屏 ----
inline bool g_fullscreen = false;
inline WINDOWPLACEMENT g_prev_placement{ sizeof(WINDOWPLACEMENT) };
void toggle_fullscreen();

// ============================================================================
// 会话与状态层（session.cpp）
// ============================================================================

inline std::unique_ptr<lr::Renderer> g_renderer;  // 渲染调度层（ADR-020）
inline lr::Canvas g_canvas;

// UI 侧的文档状态机。Rejected（本地即时拒绝）与 Failed（后台打开失败）分开，
// 两者的引导页文案不同。
struct UiDoc {
    enum class Kind { None, Rejected, Opening, Reading, Failed, NeedsPassword };
    Kind         kind = Kind::None;
    std::wstring path_w;
    std::string  name_u8, ext_u8;
    lr::DocError error = lr::DocError::Ok;  // Rejected / Failed 时的具体原因
    std::string  detail_u8;                 // MuPDF 原始错误信息
    lr::DocumentInfo info{};
    std::uint64_t request_id = 0;
};
inline UiDoc g_doc;

// ---- 阅读状态持久化（ADR-034；身份分层 ADR-062；异步写盘 ADR-082）----
inline lr::ReaderState  g_state;      // exe 同目录 reader_state.bin 的全部记录
inline lr::DocIdentity  g_identity;   // 当前文档身份（内容指纹 / 路径键 / 页数 / 路径）
inline std::uint64_t    g_doc_key = 0;  // 当前文档在库里的主键（0 = 无效，不参与存取）
// 异步持久化服务（ADR-082）：写盘在工作线程完成，UI 线程只产快照（不碰磁盘）。
// 由入口在载入 g_state 之后创建；退出路径经 flush() 保证写入（见 main.cpp 的 WM_DESTROY）。
inline std::unique_ptr<lr::PersistService> g_persist;
// 请求把 g_state 落盘：编码在调用线程（纯序列化），写盘在服务的工作线程。g_persist 为空时无操作。
void request_state_save();

// ---- 侧栏（目录 / 书签 / 缩略图 / 搜索）----
inline bool g_show_sidebar = false;
inline int  g_sidebar_tab = 0;      // 0 目录 / 1 书签 / 2 缩略图 / 3 搜索
// **一次性**的"强制切到某分栏"请求（-1 = 无）。ImGui 的 TabBar 有自己的选中态，
// 只改 g_sidebar_tab 并不会切换分栏 —— 必须在下一次绘制时给该分栏带 SetSelected 标志。
// 只保留一帧：每帧都带会让该分栏被锁死，用户再也点不到别的分栏。
inline int  g_sidebar_tab_want = -1;

// ---- 视图变换（0/90/180/270 与纸张方案）----
inline int g_rotation = 0;
inline int g_scheme = 0;            // 0 原色 / 1 深色纸张 / 2 暖色（与 lr::PageScheme 同值）
// 未旋转的逐页尺寸（点）；旋转 90/270 时交换宽高后再交给画布。
inline std::vector<lr::PageSizePt> g_raw_sizes;
inline lr::PageSizePt g_raw_default{ 595.0f, 842.0f };

// 打开后待恢复的阅读位置（首帧视口就绪后再应用，否则 fit-width 派生 zoom 尚未成立）
inline bool g_restore_pending = false;
inline int  g_restore_page = 0;

inline std::vector<lr::OutlineItem> g_outline;

// ---- 加密密码对话框 ----
inline bool        g_open_password = false;
inline bool        g_auth_pending = false;  // 认证请求在途：弹窗保持打开、背景不变
inline char        g_password_buf[256] = {};
inline std::string g_password_error;

// ---- 画布交互/调度状态 ----
inline float  g_want_scale = -1.0f;         // 已投递给渲染层的倍率；<0 表示尚未初始化
inline float  g_last_target_scale = -1.0f;  // 上一帧的目标倍率
inline double g_zoom_dirty_since = -1.0;    // 目标倍率最后一次变化的时刻；<0 表示无待定
// 滚动方向（-1 上 / 0 未定 / +1 下）：只服务方向感知预加载（ADR-032）。
inline int    g_scroll_dir = 0;
inline float  g_prev_scroll_y = 0.0f;
// 画布留白/间距当前所依据的界面缩放值；不一致时才重新下发（避免每帧失效布局缓存）。
inline float  g_canvas_scale = -1.0f;
// 画布悬停/焦点：**仅供 F3 诊断**，不参与逻辑分支（快捷键不以焦点为门，ADR-026）。
inline bool g_canvas_hovered = false;
inline bool g_canvas_focused = false;
// 右键按下/抬起计数（F3 诊断用）：排查"右键菜单没反应"时区分输入没到 vs 被挡住。
inline int g_dbg_r_down = 0;
inline int g_dbg_r_up = 0;

// ---- 视图动效状态（ADR-047）----
// 三段外壳的显式布局见 ui.cpp 的 draw_shell（ADR-049）。
// 动效只发生在 **app 层写入画布之前**：画布仍是"状态唯一真源 + 纯函数布局"，
// 不引入任何插值/时间概念，`canvas_test` 的断言不受影响。
inline float  g_scroll_pending = 0.0f;   // 待消耗的滚动量（内容像素），每帧按一阶滞后消耗
// 显式跳页（跳页框/目录/书签/缩略图/翻页）在动画结束后要**重新钉一次游标**：
// 滑行途中 scroll_by 会把阅读游标同步到"视口顶部所在行"，而末尾几行够不到视口顶部时
// 这个反推值会落到末行，与"显式跳页应显示目标行"的语义冲突（ADR-023）。
inline int    g_jump_repin_page = -1;    // ≥0 = 动画结束后重新钉住的目标页
inline float  g_jump_repin_align = 0.0f;
inline bool   g_zoom_anim = false;       // 缩放插值进行中
inline float  g_zoom_to = 1.0f;          // 缩放目标倍率（渲染请求/防抖都看它，而不是显示值）
inline float  g_zoom_anchor_x = 0.0f;    // 缩放不动点（画布内屏幕坐标）
inline float  g_zoom_anchor_y = 0.0f;
inline bool   g_zoom_end_fit = false;    // 动画结束后回到 fit-width（"适合宽度"的收尾）
inline float  g_top_bar_h = -1.0f;       // 顶栏当前动画高度（px，0~px(kTopBarH)）；<0 = 未初始化
// 侧栏滑入/滑出（ADR-055）：当前动画宽度（px），0 = 完全收起。与顶栏同一手法（一阶滞后）。
// 侧栏面板**按整宽排布、整体左移**，由 ##shell 的裁剪实现"滑出左侧"，内容不随动画重排。
inline float  g_sidebar_w = 0.0f;
// 设置窗口的打开/关闭进度（ADR-056/061）：value 0 = 完全关闭（不绘制），1 = 静止态。
// 同时驱动**透明度**与**缩放**（以窗口中心为不动点，kWindowScaleFrom ↔ 1.0）；关闭时边缩边淡。
inline ToggleAnim g_settings_anim;
// 设置窗口的"静止态"左上角（默认居中；用户拖动后每帧实测更新）。
// 开合动画以它为基准做缩放，因此关闭时不会把窗口拉回屏幕中央。
inline ImVec2 g_settings_rest_pos{};
inline bool   g_settings_rest_valid = false;

// ---- 自绘滚动条（画布右侧，可拖拽，ADR-050）----
// 三者都由 ui.cpp 的滚动条交互维护：拖拽期间必须抑制画布平移（否则"拖滚动条"变成"拖页面"）。
inline bool   g_scroll_drag = false;      // 正在拖拽滑块
inline float  g_scroll_drag_off = 0.0f;   // 抓取点相对滑块顶端的偏移
inline bool   g_scroll_hover = false;     // 悬停在轨道上（用于高亮与加宽滑块）

// 跳页弹窗
inline bool g_open_jump = false;
inline int  g_jump_page = 1;

// 页面淡入（微动效）：**只在"纹理在眼前就绪"时渐显**（占位 → 内容的换入），
// 页面预加载好之后才滑入视野的**不淡入**（直接显示）—— 否则每次翻页都从透明渐显，
// 看起来就像"翻到哪才开始加载"（人工反馈）。
//   seen    ：本页纹理已画过（同一份纹理不重复淡入）
//   waiting ：本页当前正以占位显示（说明纹理尚未就绪）
struct PageFade { bool seen = false; bool waiting = false; float alpha = 1.0f; };
inline std::vector<PageFade> g_page_fade;

// ---- 会话操作 ----
void reset_doc_state();
void enter_reading();
void request_open_document(std::wstring path);
void close_document();
void poll_document();               // 帧首接收后台打开/认证结果
void submit_password();
void update_title();

// ---- 阅读数据的身份解析与维护（ADR-062）----
// 文档打开成功时调用：算出身份（工作线程给的指纹 + 路径键），分层定位记录并落定主键。
void resolve_document_identity(std::uint64_t content_fp);
// 「另起一份」：把当前文档从共享的阅读数据里摘出去（两边各自独立），跳回第 1 页。
void detach_current_progress();
// 删除一条阅读数据；若它是当前文档，同时解除本次会话的绑定（不再写入）。
void clear_reading_data(std::uint64_t key);
void clear_all_reading_data();
// 只清"位置未知"的旧记录（升级前留下的、认不出是哪份文档的那批）。
void clear_unknown_reading_data();
// 位置未知的记录条数（管理窗口分两区显示用）。
[[nodiscard]] int unknown_reading_data_count();

void push_canvas_sizes();           // 逐页尺寸（含旋转折算）下发画布
void apply_view_transform();        // 旋转/纸张方案下发渲染层
void set_rotation(int deg);
void rotate_view(int delta);
void set_scheme(int scheme);
void toggle_scheme(int scheme);

void save_reading_state();
bool current_page_has_bookmark();
void toggle_bookmark_current();
void remove_bookmark_at(int index);
void set_sidebar(bool on, int tab);

void open_jump_popup();
void scroll_by_rows(int dir);
void handle_canvas_input(const ImVec2& origin, const ImVec2& size, bool hovered);
void handle_reading_commands(const ImVec2& size);   // 阅读态命令派发（命令表驱动，ADR-054）
void handle_global_commands();                      // 全局命令派发（任何状态可用）
void update_want_scale();           // 缩放防抖
void emit_wants();                  // 可见页 + 方向感知预加载 → 渲染请求

// ---- 视图动效（ADR-047）----
// 所有"滚动/缩放意图"统一走这几个入口，由 app 层做插值；画布只接收插值后的结果。
void request_scroll(float delta_px);                       // 滚轮/方向键/整屏：纯增量
void request_jump_scroll(int page, float align);           // 显式跳页/翻页：算目标 → 回退 → 滑行
void zoom_to_animated(float z, float anchor_sx, float anchor_sy);
void zoom_by_animated(float factor, float anchor_sx, float anchor_sy);
void fit_to_width_animated();
void step_view_motion(float dt);                           // 每帧消耗待定量 + 推进缩放插值
[[nodiscard]] float view_zoom_target();                    // 目标倍率（动效中 = 动画目标）

// ============================================================================
// 命令与按键绑定（session.cpp 定义表与派发；ui.cpp 渲染"按键"设置分栏）
// ============================================================================
//
// 为什么要有这张表：早期按键判定散落在 session.cpp 的 handle_canvas_input 与
// draw_shell 里（一处一个 IsKeyPressed），既**无法自定义**、也**无法在界面上列出**。
// 这里把每条命令的默认键、是否连发、是否全局可用集中成一张表：输入侧统一走
// cmd_pressed()，界面侧直接遍历同一张表渲染"按键设置"，两处不会漂移。
//
// 一条命令最多两个键（主键 + 备键）：数字/算术类快捷键"主键盘与小键盘两套都判"
// （ADR-027）由"两个槽"直接表达，不再写别名；"Esc 兼作全屏"这类诉求也由备键承接。

enum class Cmd : int {
    // 导航
    NextRow, PrevRow, ScrollDown, ScrollUp, PageDown, PageUp, FirstPage, LastPage, JumpPage,
    // 缩放
    ZoomIn, ZoomOut, FitWidth,
    // 缩放：滚轮缩放的**修饰键**（Ctrl+滚轮那种）。它是"按住不放的键"而不是一条被按下的
    // 命令，故不参与 cmd_pressed 派发，只由 handle_canvas_input 读取（见 wheel_zoom_mod_held）。
    ZoomWheelMod,
    // 视图
    Col1, Col2, Col3, Col4, ToggleSpread, RotateCW, ToggleDark, ToggleWarm,
    // 界面
    ToggleSidebar, ToggleBookmark, OpenSettings, OpenKeys, ToggleDebug, ToggleFullscreen, OpenFile,
    // 文本（Phase 8）。追加在末尾而不是插进中间：`Cmd` 的整数值虽不落盘（ini 键名是 id 字符串），
    // 但让已有命令的序号保持稳定，能让任何按序号写的调试代码/日志不至于突然错位。
    Copy, SelectAll, OpenSearch,
    Count,
};

inline constexpr int kCmdCount = static_cast<int>(Cmd::Count);
inline constexpr int kBindSlots = 2;   // 每条命令的按键槽数（主键 / 备键）

struct CmdDef {
    const char*   id;      // ini 键名：**稳定标识**，改显示文案不得改它
    const char*   group;   // 分组（按键界面按此分段）
    const char*   name;    // 显示名
    bool          repeat;  // 按住是否连发（滚动/缩放类为 true）
    bool          global;  // 是否任何状态都可用（不受"阅读态/画布输入"约束）
    ImGuiKeyChord def0;    // 默认主键（ImGuiKey | ImGuiMod_*）
    ImGuiKeyChord def1;    // 默认备键（ImGuiKey_None = 无）
    // 修饰键型命令：绑定内容**只是修饰键集合**（ImGuiMod_Ctrl 等，不含主键），
    // 既不参与 cmd_pressed 派发（那要求"有主键被按下"），也不用"捕获按键"改 ——
    // 在按键分栏里渲染成下拉框（无 / Ctrl / Alt / Shift / Ctrl+Alt）。
    bool          mod_only = false;
};

extern const CmdDef kCmds[kCmdCount];

// 当前绑定。空槽 = ImGuiKey_None。由 load_binds 从 ini 覆盖，改动即时落盘。
inline ImGuiKeyChord g_binds[kCmdCount][kBindSlots] = {};

// 按键捕获：在"按键"设置分栏点中某槽后进入；捕获期间**不派发任何命令**，
// 否则按下的那个键会立刻触发它原本绑定的命令。
inline int g_capture_cmd = kCmdCount;   // kCmdCount = 当前未捕获
inline int g_capture_slot = 0;
inline int g_capture_frame = -1;        // 进入捕获的帧号：跳过"点按钮"那一次鼠标点击

void reset_binds_to_default();
void load_binds();                      // 需在 ImGui 上下文建立后调用（GetKeyName）
void save_binds();
bool cmd_pressed(Cmd c);                // 任一槽按下即 true（含修饰键严格匹配）
// 滚轮缩放的修饰键当前是否按住（严格匹配：绑定 Ctrl 时按住 Ctrl+Shift 不触发）。
// 未设置（"无"）时恒 false —— 此时滚轮只滚动，不缩放。
bool wheel_zoom_mod_held();
bool is_bindable_key(ImGuiKey k);       // 排除修饰键/鼠标/手柄
std::string chord_label(ImGuiKeyChord c);   // 显示用（"Ctrl+O"）
std::string chord_to_string(ImGuiKeyChord c);   // ini 存储用（纯 ASCII）
ImGuiKeyChord chord_from_string(const std::string& s);
void update_key_capture();              // 每帧推进捕获（在设置窗口绘制之后调用）
int  find_bind_conflict(int cmd, int slot); // 冲突命令下标；无冲突 -1

// ============================================================================
// 文本交互 / 剪贴板 / 全文搜索（Phase 8）
// ============================================================================
//
// 坐标一律经 page_map.h 折算：屏幕 ↔ **未旋转页面 pt**。命中测试、选区、高亮、
// 搜索命中、链接热区全部在这一个坐标系里做；旋转只在"最后映射到屏幕"时体现，
// 于是旋转视图下不需要任何特殊分支（也正因如此，page_map 的旋转折算被单测钉死）。
//
// 线程纪律不变：本层**零 fz_***。文本布局由渲染工作线程抽好后发布快照（render 的
// request_page_content / take_page_content），本层只做纯浮点命中测试。

// 页面 pt 空间的一个矩形（选区高亮按行合并后的结果）
struct SelRect {
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
};

// 选区。**单页**：跨页选择刻意不做 —— 多数阅读器也没有，且它要引入"页序 + 页内偏移"
// 的复合定位与跨页高亮，收益远小于代价。
//
// 关键设计：选区一旦确定，就把**复制端点 + 高亮矩形**冻结进结构里，而不是只存
// "字符下标"。原因：字符下标只在"当前页的内容快照"里才有意义，而快照会随鼠标移动
// 到别的页而被换掉（内容缓存只有一页）。若只存下标，用户选好一段、把鼠标移到下一页
// 再回来右键复制，就会复制失败或复制到错的内容。冻结之后，选区与内容缓存完全解耦。
struct Selection {
    bool active = false;
    int  page = -1;
    int  anchor = -1;     // 按下时的字符下标（仅拖动期间有效，用于算范围）
    int  head = -1;       // 当前字符下标（同上）
    bool dragging = false;
    // 冻结结果（未旋转页面 pt）
    float ax_pt = 0.0f, ay_pt = 0.0f;   // 复制端点 a（选区阅读顺序前端）
    float bx_pt = 0.0f, by_pt = 0.0f;   // 复制端点 b（后端）
    std::vector<SelRect> rects;         // 高亮矩形（已按行合并）
};
inline Selection g_sel;
inline bool g_select_all_pending = false;   // 全选请求在等内容快照
// 一次按下/抬起的辅助状态：区分"点击"与"拖拽"、以及"点击是否落在链接上"。
// 链接存**副本**而不是下标：按下到抬起之间内容快照可能被换掉（鼠标移到了别的页），
// 下标就失效了；副本里已经带了 uri 与解析好的目标页，与快照解耦。
inline float        g_press_x = 0.0f, g_press_y = 0.0f;
inline int          g_press_page = -1;
inline bool         g_press_link_valid = false;
inline lr::PageLink g_press_link;
inline bool         g_press_on_text = false;

// 鼠标所在页的交互内容（文本布局 / 图片矩形 / 链接）。
// 只请求**鼠标所在那一页**：抽一次 stext 与渲染一页同价，不能对全部可见页盲发。
inline int             g_content_page = -1;   // 已收到内容的页（-1 = 无）
inline int             g_content_want = -1;   // 已发出请求的页
inline double          g_content_since = -1.0;// 该请求的发出时刻（超时重发用，见 update_hovered_content）
inline lr::PageContent g_content;

// 右键菜单锚点：右键按下那一刻的页、页面 pt、以及该点上的链接（若有，存副本）。
// 必须**在按下时**记下来 —— 用户点菜单项时鼠标已移出画布，再取实时位置只会取到菜单上。
inline int          g_ctx_page = -1;
inline float        g_ctx_x_pt = 0.0f, g_ctx_y_pt = 0.0f;
inline bool         g_ctx_link_valid = false;
inline lr::PageLink g_ctx_link;
inline bool         g_ctx_image_valid = false;   // 右键位置是否落在嵌入图片上（决定「复制图片」可用性）
// 悬停诊断（F3 浮层用）
inline int   g_hover_page = -1;
inline int   g_hover_link = -1;
inline int   g_hover_char = -1;

// 状态栏短暂提示（"已复制"/"此处没有图片"）。toast 比弹窗轻，不打断阅读。
inline std::string g_toast;
inline double      g_toast_since = -1.0;

// ---- 全文搜索 ----
inline char g_search_buf[256] = {};
inline bool g_search_focus = false;              // 下一帧把键盘焦点交给输入框
inline std::vector<lr::SearchHit> g_search_hits;
inline int  g_search_cur = -1;                   // 当前命中下标（-1 = 未选中）
inline int  g_search_scanned = 0;                // 已扫描页数（进度显示）
inline int  g_search_total = 0;                  // 总页数
inline bool g_search_active = false;             // 仍在检索
inline bool g_search_truncated = false;          // 命中触顶
inline bool g_search_scroll_pending = false;     // 有待执行的"滚到当前命中"

// 输入防抖（自动检索）。三件状态必须分开：
//   committed —— 结果列表对应的关键字（"现在显示的这批命中是谁的"）；
//   seen      —— 上一帧输入框的内容（用来判定"这一帧用户是否改了字"）；
//   pending   —— 有改动、正在等防抖计时到期。
// 为什么不用 committed 直接和输入框比：改了字之后 committed 还没变，会每帧都判定"改了"，
// 于是计时器每帧被重置、永远等不到到期。必须有一个"已经看见过这次改动"的标记。
inline std::string g_search_committed;
inline char        g_search_seen[256] = {};
inline bool        g_search_pending = false;
inline double      g_search_edit_at = -1.0;      // 输入内容最后一次变化的时刻（ImGui 时间轴）

// ---- 坐标与命中测试 ----
[[nodiscard]] bool page_view_of(const ImVec2& origin, int page, PageView& view, PageGeom& geom);
[[nodiscard]] int  page_at_screen(const ImVec2& origin, const ImVec2& screen);
// 字符命中测试。clamp_to_nearest = false（悬停 / 按下）**严格**：只有落在某行文字的近旁
// 才算命中，否则返回 -1 —— 否则整页都会算"可选文本"，鼠标离开文字也不变回箭头。
// = true（拖拽中）宽松：拖到行尾之外仍吸附到最近字符，选区才能"到底"。
[[nodiscard]] int  char_index_at(int page, float x_pt, float y_pt, bool clamp_to_nearest);
[[nodiscard]] int  link_index_at(int page, float x_pt, float y_pt);
[[nodiscard]] int  image_index_at(int page, float x_pt, float y_pt);
void selection_clear();
void selection_select_all();
void selection_copy();
// 用右键菜单锚点复制嵌入图片 / 打开链接（锚点无效时退回当前悬停位置）
void copy_image_at_context();
void open_link_at_context();

// ---- 文本交互 ----
// 在画布输入里调用；返回 true = 本帧左键被"文本选择"接管（平移必须让位）。
[[nodiscard]] bool handle_text_interaction(const ImVec2& origin, const ImVec2& size, bool hovered);
// 请求/接收鼠标所在页的内容快照（每帧调用）
void update_hovered_content(const ImVec2& origin, bool hovered);
// 取走复制结果并写入剪贴板（每帧调用）
void update_clipboard_results();
void show_toast(std::string text);
[[nodiscard]] std::string toast_text();   // 有效期内返回文案，否则空串

// ---- 全文搜索 ----
void search_start();                 // 用 g_search_buf 发起检索（回车 / 按钮 / 防抖到点）
void search_cancel();                // 取消当前检索，清空输入、结果与高亮
void search_clear();                 // 清空检索状态与结果（关文档 / 清空关键字）
void search_goto(int index);         // 跳到第 index 条命中并高亮
void search_step_hit(int dir);       // 上一条 / 下一条
void update_search();                // 每帧：输入防抖 + 取新命中 + 处理跳转请求
[[nodiscard]] bool search_has_query();

// ============================================================================
// 绘制层（ui.cpp）
// ============================================================================

inline bool g_show_debug = false;
inline bool g_show_settings = false;

// ---- 通用确认弹窗（ADR-062）----
// 一处弹窗、多处复用：① 智能匹配的"要不要沿用"询问；② 阅读数据删除 / 清空的二次确认；
// ③ 上次异常退出的提示（Phase 7，主按钮打开崩溃报告目录）。
// 两个按钮都是动作：主按钮 = 推荐动作，次按钮 = 另一动作或取消。
enum class ConfirmKind : int { None, Relocate, ClearOne, ClearAll, PruneUnknown, CrashNotice };

inline ConfirmKind   g_confirm_kind = ConfirmKind::None;
inline bool          g_confirm_open = false;
inline std::string   g_confirm_title;      // 标题（强调色，一行）
inline std::string   g_confirm_body;       // 说明（可含换行，按宽度折行）
// 「标签 → 值」明细行（如"原位置 → D:\书\a.pdf"）：标签次要色左对齐，值折行，
// 让长路径不至于和说明文字糊成一团。
inline std::vector<std::pair<std::string, std::string>> g_confirm_rows;
inline std::string   g_confirm_ok;         // 主按钮文案
inline std::string   g_confirm_alt;        // 次按钮文案
inline std::uint64_t g_confirm_target = 0; // ClearOne：要删的那条记录的主键
// ---- 设置窗口的分栏索引（draw_settings_window 消费后复位为 -1）----
// 0 界面 / 1 阅读 / 2 性能 / 3 按键 / 4 阅读数据
inline int g_settings_open_tab = -1;
// 「键盘打开画布右键菜单」（Shift+F10 / 菜单键）：由 draw_shell 置位，
// draw_canvas_context_menu 在画布窗口作用域内消费（与右键同一 ID 空间）。
inline bool g_open_canvas_ctx = false;

// 顶栏自动隐藏（沉浸阅读）：鼠标离开顶部一段时间后收起，靠近窗口顶端即重现。
inline bool   g_toolbar_visible = true;
inline double g_toolbar_idle_since = -1.0;
// **顶栏自己的弹出菜单**（主菜单 / 缩放档位）开着时钉住顶栏：菜单挂在顶栏按钮下方，
// 顶栏若在这时滑走，菜单就成了"悬空的菜单"，观感是坏的。
// 与之相对，**画布上的右键菜单、跳页/密码/确认弹窗一律不钉顶栏** —— 它们是画布/屏幕中央
// 的操作，把顶栏一并唤醒属于无谓的视觉噪音（人工反馈）。由 ui.cpp 的 draw_top_bar 每帧写。
inline bool   g_toolbar_pinned = false;

// 自绘滚动指示条：滚动时浮现、静止后渐隐。
inline float  g_scroll_ind_alpha = 0.0f;
inline float  g_scroll_ind_last_y = 0.0f;
inline double g_last_scroll_time = -1.0;

// 「打开文档…」对话框：真正的 GetOpenFileNameW 放在帧与帧之间执行（自带模态消息循环）。
inline bool    g_request_open_dialog = false;
// 全屏切换请求（ADR-060）：**延到帧与帧之间**执行。若在 ImGui 一帧中途改窗口几何，
// 本帧的绘制数据仍是按旧尺寸排布的，会被 DWM 拉伸到新窗口矩形上 —— 这就是"退出全屏闪一下/
// 最大化时往左上缩一下再归位"的来源。放到帧首后，几何变更与随后的 new_frame/render 同帧一致。
inline bool    g_request_fullscreen_toggle = false;
// 窗口已最小化（WM_SIZE 的 SIZE_MINIMIZED）：期间**整帧不渲染**。
// 最小化时客户区为 0，ImGui 在 0 尺寸视口下什么都画不出，整帧只剩清屏色 ——
// 在"收起动画"里就会闪一下（人工反馈）。顺带也省掉最小化期间的 CPU/GPU。
inline bool    g_minimized = false;
inline wchar_t g_open_path_buf[32768] = {};

// 上次异常退出的摘要（入口处由 crash::take_last_crash 填入，交给确认弹窗展示后清空）。
inline std::string g_last_crash_summary;

// 三段外壳的绘制：**高度/位置由 draw_shell 显式给出**，不依赖 ImGui 的"相邻项自动间距"
// （那正是状态栏被挤出窗口底部的根因，见 draw_shell 注释）。
void draw_shell();
void draw_top_bar(float bar_h);
void draw_status_bar();
void draw_canvas_area(float height);
void draw_sidebar(float height, float width);   // width 为动画宽度（px），内容仍按整宽排布
void draw_canvas_context_menu();
// 页内叠加层（Phase 8）：搜索命中高亮 / 选区高亮 / 链接悬停高亮。
// 在画布逐页绘制循环里调用（此时裁剪矩形已是画布区，超出部分自然被裁掉）。
void draw_page_overlays(ImDrawList* dl, const ImVec2& origin, int page);
void draw_search_tab();     // 侧栏「搜索」分栏（Phase 8）
void draw_debug_overlay();
void draw_jump_popup();
void draw_password_popup();
void draw_confirm_popup();          // 通用确认弹窗（智能匹配询问 / 删除二次确认，ADR-062）
void draw_settings_window();
void draw_settings_reading_data_tab();   // 设置 ·「阅读数据」分栏（清单 + 删单条 / 清空全部）
// 打开确认弹窗（由 session / 管理窗口在需要用户拍板时调用）。kind 决定按钮动作的归属。
void request_confirm(ConfirmKind kind, std::string title, std::string body,
                     std::vector<std::pair<std::string, std::string>> rows,
                     std::string ok_label, std::string alt_label,
                     std::uint64_t target = 0);
void update_toolbar_visibility();
bool top_bar_should_show();
float update_top_bar_height(float dt);   // 顶栏高度的滑入/滑出插值
float update_sidebar_width(float dt);    // 侧栏宽度的滑入/滑出插值（ADR-055）

// ============================================================================
// 入口层（main.cpp）
// ============================================================================

void open_file_dialog_now();        // 帧间调用；取消则无操作

}  // namespace lr::app
