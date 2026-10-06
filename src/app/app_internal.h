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

#include "imgui.h"

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

// ---- 视图动效（Phase 6 收尾，ADR-047）----
// 统一用**一阶滞后**（帧率无关的指数趋近）而不是补间：无过冲、必然收敛、不需要维护速度状态，
// 且连续输入时自然叠加（补间则每次输入都要重排时间轴）。速率单位 1/s，时间常数 = 1/rate：
// 22/s ≈ 45ms，95% 收敛约 135ms —— 这个量级"跟得上手"又把台阶抹平了。
inline constexpr float  kScrollSmoothRate = 22.0f;  // 滚轮 / 方向键 / 整屏滚动
inline constexpr float  kZoomSmoothRate   = 26.0f;  // 缩放插值
inline constexpr float  kTopBarAnimRate   = 20.0f;  // 顶栏自动隐藏的滑入/滑出
inline constexpr float  kMotionEpsPx      = 0.5f;   // 动画收敛阈值（像素/倍率），到阈值即吸附到目标

// 一阶趋近（帧率无关的指数趋近）：无过冲、必然收敛，不需要维护速度状态。
inline float approach(float cur, float target, float rate, float dt) {
    return target + (cur - target) * std::exp(-rate * dt);
}

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
// 阅读器有三套 chrome 表现：浅色/深色/跟随系统；反色模式下强制深色（ADR-043）。
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
inline bool g_theme_applied = false;   // g_pal / ImGui 颜色是否已按 g_dark_theme 应用
inline bool g_icons_ok = false;        // 图标字形是否已成功合并进字体（否则退化为文字标签）

void apply_theme_colors();
void sync_theme(int color_mode);       // 主题偏好 + 文档配色 → chrome 明暗（帧首调用）

// ---- D3D11（RAII） ----
struct Graphics {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swap_chain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    DXGI_SWAP_CHAIN_DESC sc_desc{};

    bool initialize(HWND hwnd);
    bool create_rtv();
    void resize(UINT w, UINT h);
    void render_frame();
    void shutdown();
};
inline Graphics g_gfx;

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
struct UiPrefs {
    float ui_scale = 1.0f;             // [ui] UiScale      0.80~1.50
    int   theme = 0;                   // [ui] Theme        0 跟随系统 / 1 浅色 / 2 深色
    bool  auto_hide_toolbar = true;    // [ui] AutoHideToolbar
    bool  motion = true;               // [ui] Motion       页面淡入 / 滚动指示条渐隐
    float gap_percent = kCanvasGapRatio * 100.0f;  // [ui] GapPercent   页面间距（列宽百分比）0~6
    int   cache_mb = 512;              // [cache] BudgetMB  128~2048
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

// ---- 阅读状态持久化（ADR-034）----
inline lr::ReaderState g_state;     // exe 同目录 reader_state.bin 的全部记录
inline std::uint64_t   g_doc_key = 0;  // 当前文档键（0 = 无效，不参与存取）

// ---- 侧栏（目录 / 书签 / 缩略图）----
inline bool g_show_sidebar = false;
inline int  g_sidebar_tab = 0;      // 0 目录 / 1 书签 / 2 缩略图

// ---- 视图变换（0/90/180/270 与配色）----
inline int g_rotation = 0;
inline int g_color_mode = 0;        // 0 正常 / 1 反色 / 2 护眼
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

// ---- 自绘滚动条（画布右侧，可拖拽，ADR-050）----
// 三者都由 ui.cpp 的滚动条交互维护：拖拽期间必须抑制画布平移（否则"拖滚动条"变成"拖页面"）。
inline bool   g_scroll_drag = false;      // 正在拖拽滑块
inline float  g_scroll_drag_off = 0.0f;   // 抓取点相对滑块顶端的偏移
inline bool   g_scroll_hover = false;     // 悬停在轨道上（用于高亮与加宽滑块）

// 跳页弹窗
inline bool g_open_jump = false;
inline int  g_jump_page = 1;

// 页面淡入（微动效）：仅在纹理"首次出现"的那一帧从 0 渐显。
struct PageFade { bool seen = false; float alpha = 1.0f; };
inline std::vector<PageFade> g_page_fade;

// ---- 会话操作 ----
void reset_doc_state();
void enter_reading();
void request_open_document(std::wstring path);
void close_document();
void poll_document();               // 帧首接收后台打开/认证结果
void submit_password();
void update_title();

void push_canvas_sizes();           // 逐页尺寸（含旋转折算）下发画布
void apply_view_transform();        // 旋转/配色下发渲染层
void set_rotation(int deg);
void rotate_view(int delta);
void set_color_mode(int mode);
void toggle_color_mode(int mode);

void save_reading_state();
bool current_page_has_bookmark();
void toggle_bookmark_current();
void remove_bookmark_at(int index);
void set_sidebar(bool on, int tab);

void open_jump_popup();
void scroll_by_rows(int dir);
void handle_canvas_input(const ImVec2& origin, const ImVec2& size, bool hovered);
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
// 绘制层（ui.cpp）
// ============================================================================

inline bool g_show_debug = false;
inline bool g_show_settings = false;
inline bool g_show_help = false;
// 「键盘打开画布右键菜单」（Shift+F10 / 菜单键）：由 draw_shell 置位，
// draw_canvas_context_menu 在画布窗口作用域内消费（与右键同一 ID 空间）。
inline bool g_open_canvas_ctx = false;

// 顶栏自动隐藏（沉浸阅读）：鼠标离开顶部一段时间后收起，靠近窗口顶端即重现。
inline bool   g_toolbar_visible = true;
inline double g_toolbar_idle_since = -1.0;

// 自绘滚动指示条：滚动时浮现、静止后渐隐。
inline float  g_scroll_ind_alpha = 0.0f;
inline float  g_scroll_ind_last_y = 0.0f;
inline double g_last_scroll_time = -1.0;

// 「打开文档…」对话框：真正的 GetOpenFileNameW 放在帧与帧之间执行（自带模态消息循环）。
inline bool    g_request_open_dialog = false;
inline wchar_t g_open_path_buf[32768] = {};

// 三段外壳的绘制：**高度/位置由 draw_shell 显式给出**，不依赖 ImGui 的"相邻项自动间距"
// （那正是状态栏被挤出窗口底部的根因，见 draw_shell 注释）。
void draw_shell();
void draw_top_bar(float bar_h);
void draw_status_bar();
void draw_canvas_area(float height);
void draw_sidebar(float height);
void draw_canvas_context_menu();
void draw_debug_overlay();
void draw_jump_popup();
void draw_password_popup();
void draw_settings_window();
void draw_help_window();
void update_toolbar_visibility();
bool top_bar_should_show();
float update_top_bar_height(float dt);   // 顶栏高度的滑入/滑出插值

// ============================================================================
// 入口层（main.cpp）
// ============================================================================

void open_file_dialog_now();        // 帧间调用；取消则无操作

}  // namespace lr::app