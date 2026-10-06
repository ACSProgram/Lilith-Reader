// platform.cpp — Lilith Reader 应用层：平台地基
//
// 职责（不感知文档/阅读状态，只提供"外壳运行所需的底座"）：
//   · D3D11 设备与交换链（RAII）
//   · ImGui 上下文与后端引导、中文字体 + 图标字形合并
//   · DPI / 界面缩放（g_dpi_scale × g_user_scale）
//   · 主题（浅/深/跟随系统）与纸张方案派生出的 chrome 明暗/暖化
//   · 用户偏好持久化（exe 同目录 LilithReader.ini）
//   · 输入法关联切换（ADR-028/040）与全屏
//
// 依赖：只依赖 Win32 / D3D11 / ImGui / utils；不 import document/canvas/render。
// 共享状态与函数声明见 app_internal.h。

#include "app_internal.h"

// DPI 查询辅助来自 Win32 后端头；该头声明了 Init/Shutdown/NewFrame 与 DPI 辅助函数。
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"
// FreeType 装载器（ADR-048）：比内置 stb_truetype 栅格化质量好，且能读 CFF/OTF（子集即 OTF）。
#include "misc/freetype/imgui_freetype.h"
// ImGui 内部错误的接管入口（Phase 7）——它需要 imgui_internal.h，故单独隔离在这个头里。
#include "imgui_stacks.h"

namespace lr::app {

// ---------------- 图标字形 ----------------
//
// 图标取自系统字体 **Segoe MDL2 Assets**（Win10+ 内置），以 MergeMode 合并进同一 ImFont。
// 字形编码在 Unicode 私用区（PUA），故以 UTF-8 字面量写死，逐个标注码位便于核对
// （码位对照表见 docs/03-决策记录.md ADR-045）。**只列本项目实际用到的字形**：多烘一个
// 字形就多一份图集体积与一处校验点，无用的码位不应留在表里。
// 该字体缺失（或任一码位无字形）时 g_icons_ok = false，所有按钮退化为文字标签。
constexpr const char* kIconGlyphs =
    "\xEE\x9C\x80\xEE\xA2\xA0\xEE\xA2\xA3\xEE\x9C\x9F\xEE\x9D\x80\xEE\x87\x99"  // E700 E8A0 E8A3 E71F E740 E1D9
    "\xEE\x9C\xB4\xEE\x9C\x93\xEE\xA2\x97\xEE\x9E\xAD\xEE\x9C\x91\xEE\x9D\x8B"  // E734 E713 E897 E7AD E711 E74B
    "\xEE\x9C\xAA\xEE\x9C\xAB\xEE\xA0\x8F\xEE\xA3\xBD\xEE\xA0\x8A\xEE\x9E\x90"  // E72A E72B E80F E8FD E80A E790
    "\xEE\xA0\xAD\xEE\xA2\xA5\xEE\xA3\xA5\xEE\x9C\xA1"                            // E82D E8A5 E8E5 E721
    "\xEE\xA2\xB7\xEE\x9D\x8D"                                                     // E8B7 E74D
    "\xEE\xA3\x88\xEE\xA2\xB9\xEE\x9C\x9B";                                        // E8C8 E8B9 E71B

namespace {

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

// 未缩放的 ImGui 基准样式（CreateContext 后立即留底；每次缩放都从它重算，避免累积）
ImGuiStyle g_base_style;
bool g_base_style_ready = false;

// 取出 exe 内嵌 RCDATA 的只读内存（ADR-048）。
// 资源在进程生命周期内始终映射着，故指针可直接交给 ImGui（配合 FontDataOwnedByAtlas=false），
// 既不用拷贝也不用释放 —— 这正合 ImGui 1.92+ "字体数据须全程有效"的要求。
const void* find_embedded_resource(int id, int* out_size) {
    const HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (res == nullptr) return nullptr;
    const DWORD size = SizeofResource(nullptr, res);
    const HGLOBAL handle = LoadResource(nullptr, res);
    if (handle == nullptr) return nullptr;
    const void* data = LockResource(handle);
    if (data == nullptr) return nullptr;
    if (out_size != nullptr) *out_size = static_cast<int>(size);
    return data;
}

}  // namespace

// ---------------- 界面缩放（DPI） ----------------

void apply_ui_scale() {
    if (!g_base_style_ready) return;
    ImGuiStyle& st = ImGui::GetStyle();
    st = g_base_style;
    st.ScaleAllSizes(ui_scale());      // 内边距/间距/圆角/滚动条（不含字体）
    st.FontScaleMain = g_user_scale;   // 字体：主缩放（来自「设置 → 界面缩放」）
    st.FontScaleDpi = g_dpi_scale;     // 字体：DPI 缩放（自动）
    if (g_theme_applied) apply_theme_colors();  // 覆盖 g_base_style 里的浅色配色
}

void refresh_dpi_scale() {
    if (!g_hwnd) return;
    const float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(g_hwnd);
    if (dpi > 0.0f && std::fabs(dpi - g_dpi_scale) > 0.001f) {
        g_dpi_scale = dpi;
        apply_ui_scale();
    }
}

// ---------------- 主题色 ----------------

const Palette kPalLight{
    IM_COL32(228, 231, 235, 255), IM_COL32(244, 245, 247, 255),
    IM_COL32(214, 218, 223, 255), IM_COL32(0, 0, 0, 40),
    IM_COL32(246, 247, 249, 255), IM_COL32(200, 204, 210, 255),
    IM_COL32(130, 134, 140, 255), IM_COL32(252, 238, 238, 255),
    IM_COL32(220, 150, 150, 255), IM_COL32(56, 60, 66, 255),
    IM_COL32(122, 128, 136, 255), IM_COL32(59, 125, 216, 255),
    IM_COL32(0, 0, 0, 26),
};

const Palette kPalDark{
    IM_COL32(23, 24, 28, 255),    IM_COL32(32, 33, 38, 255),
    IM_COL32(52, 54, 60, 255),    IM_COL32(255, 255, 255, 34),
    IM_COL32(38, 39, 44, 255),    IM_COL32(78, 80, 88, 255),
    IM_COL32(150, 154, 162, 255), IM_COL32(58, 35, 37, 255),
    IM_COL32(150, 80, 80, 255),   IM_COL32(214, 217, 222, 255),
    IM_COL32(142, 147, 154, 255), IM_COL32(106, 166, 240, 255),
    IM_COL32(0, 0, 0, 90),
};

namespace {

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

}  // namespace

// ---------------- 纸张方案的色调（ADR-068）----------------
//
// 三套方案 = 三份 Tone。**原色是 inactive 的**：一个通道都不动 —— 它是用户认可的基准，
// 任何"顺手调一下"都是回归（tests/tone_test.cpp 里有逐通道断言钉着）。
//
// 深色纸张/暖色则把中性表面族旋到**页面纸色所在的色相**上，两边同族 —— "页面暖、周围冷"
// 从结构上不再可能发生。强调色与语义色各有归属，规则与理由见 tone.h。
namespace {

constexpr Tone kToneOriginal{ false, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, {} };

// 深色纸：#1F1D1B（hue 30）—— 纸面本身是暖黑，chrome 取同色相但**饱和度压得很低**：
// 深色纸张对应的 chrome 的"暖"极易过量，因为文字面积小、对比强，一丁点色相在亮色上就非常显眼
// （sat 0.22 时正文会被染成 #EFD7C1 那样的奶油色，整屏就"变成暖深色"了）。
// 暖意只留在强调色上，中性族基本保持中性。
constexpr Tone kToneDarkPage{ true, 29.0f, 0.08f, 1.00f, 1.00f, 0.25f,
                              { 0.878f, 0.700f, 0.480f } };   // 柔沙 #E0B37B

// 暖色纸：#F6EEDC（hue 41 / sat 0.11）—— 色相取 40 与纸面一致。明度整体 ×0.87：
// 让米黄纸面成为全屏**最亮**的一层（原先周围比纸还亮，纸面反而显得发闷）。
// 中间调 ×0.70：色度对比不计入明度对比，彩色底的次要文字/边框需要额外压深一档。
// 强调色用赭石 #A96F25 —— 与米黄同族，而不是把冷蓝硬塞进暖底。
constexpr Tone kToneWarmPage{ true, 40.0f, 0.155f, 0.87f, 0.70f, 0.25f,
                              { 0.663f, 0.435f, 0.145f } };

}  // namespace

ImVec4 tone_apply(ImVec4 c) {
    if (!g_tone.active) return c;
    const Rgb o = lr::app::tone_apply(Rgb{ c.x, c.y, c.z }, g_tone);
    return ImVec4(o.r, o.g, o.b, c.w);
}

ImU32 tone_apply(ImU32 c) {
    if (!g_tone.active) return c;
    return ImGui::ColorConvertFloat4ToU32(tone_apply(ImGui::ColorConvertU32ToFloat4(c)));
}

// 由中性底派生当前方案的调色板。半透明色（页面投影、页面描边）只换 RGB，alpha 原样 ——
// 它们本来就是"叠在画布上的一层黑/白"，改透明度会连叠出来的效果一起变。
Palette tone_palette(const Palette& base) {
    if (!g_tone.active) return base;
    Palette p;
    p.backdrop            = tone_apply(base.backdrop);
    p.chrome              = tone_apply(base.chrome);
    p.chrome_border       = tone_apply(base.chrome_border);
    p.page_border         = tone_apply(base.page_border);
    p.placeholder         = tone_apply(base.placeholder);
    p.placeholder_border  = tone_apply(base.placeholder_border);
    p.placeholder_text    = tone_apply(base.placeholder_text);
    p.failed              = tone_apply(base.failed);
    p.failed_border       = tone_apply(base.failed_border);
    p.chrome_text         = tone_apply(base.chrome_text);
    p.chrome_dim          = tone_apply(base.chrome_dim);
    p.accent              = tone_apply(base.accent);
    p.shadow              = tone_apply(base.shadow);
    return p;
}

// ImGui 控件的配色（圆角/间距等几何量在 ImGuiRaii::initialize 里设定，随 DPI 缩放）。
//
// **两个分支都必须先把整套样式重置一次**（StyleColorsLight / StyleColorsDark），再逐项覆盖。
// 只覆盖"我们在意的那些"是不够的：ImGui 有 60 多个颜色项，未覆盖的会**沿用上一次留下的值** ——
// 于是"先浅色后深色"的路径下，那些项会带着浅色值活到深色主题里。实测踩到的就是
// `ImGuiCol_CheckboxSelectedBg`（勾选框选中态的底，浅色是近白 (0.95,0.97,1.00)）：
// 深色下勾选框变成一整块亮奶油色，与周围完全割裂。
void apply_theme_colors() {
    ImGuiStyle& st = ImGui::GetStyle();
    ImVec4* c = st.Colors;
    if (g_dark_theme) {
        ImGui::StyleColorsDark(&st);
        c[ImGuiCol_Text]                  = ImVec4(0.85f, 0.86f, 0.88f, 1.00f);
        c[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.53f, 0.57f, 1.00f);
        c[ImGuiCol_WindowBg]              = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
        c[ImGuiCol_ChildBg]               = ImVec4(0.13f, 0.13f, 0.15f, 1.00f);
        c[ImGuiCol_PopupBg]               = ImVec4(0.15f, 0.15f, 0.17f, 0.99f);
        c[ImGuiCol_Border]                = ImVec4(0.24f, 0.25f, 0.28f, 1.00f);
        c[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
        c[ImGuiCol_FrameBg]               = ImVec4(0.19f, 0.20f, 0.23f, 1.00f);
        // 勾选框"选中态"的底：比 FrameBg 提亮一档（与悬停态同级），让强调色的勾醒目，
        // 而不是让整块底变成强调色 —— "深底 + 彩色勾"才是这套浅/深主题一贯的样子。
        c[ImGuiCol_CheckboxSelectedBg]    = ImVec4(0.25f, 0.26f, 0.30f, 1.00f);
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
        // 会显得很重。改用近黑文字与略带灰的白，整体更柔和（ADR-045）。
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
    // 纸张方案下，控件配色的**全部**条目统一过一遍色调映射 —— 逐条手写变体既难保持一致，
    // 也必然漏改某几个（ADR-068）。原色方案下 g_tone.active 为假，这一步是空操作。
    if (g_tone.active) {
        for (int i = 0; i < ImGuiCol_COUNT; ++i) c[i] = tone_apply(c[i]);
    }
}

// 依据「界面主题 + 纸张方案」决定 chrome 的明暗与色调；只在需要时真正改动。
//
// 两个维度的分工（ADR-067/068）：
//   · 明暗来自**界面主题**（用户偏好）。只有在主题为「跟随系统」时，「深色纸张」才会
//     额外令 chrome 变暗；用户显式选浅色/深色后，文档状态不能覆盖这个决定；
//   · 色调来自**纸张方案**：中性表面族旋到与纸面同色相，强调色换成该方案的强调色。
// 这样"页面暖、周围冷""页面暖、按钮冷"两个不协调从源头消失 —— 两侧走同一条映射。
void sync_theme(int scheme) {
    const bool follows_system = g_prefs.theme == 0;
    const bool want_dark = (g_prefs.theme == 2) ||
                           (follows_system &&
                            (system_prefers_dark_cached() || scheme == 1));
    const Tone& want_tone = (scheme == 1) ? kToneDarkPage
                          : (scheme == 2) ? kToneWarmPage
                                          : kToneOriginal;
    if (g_theme_applied && want_dark == g_dark_theme && tone_same(want_tone, g_tone)) return;
    g_dark_theme = want_dark;
    g_tone = want_tone;
    g_pal = tone_palette(want_dark ? kPalDark : kPalLight);
    apply_theme_colors();
    g_theme_applied = true;
}

// ---------------- D3D11 ----------------

// 把 HRESULT 转成可检索的字段（16 进制），日志里比十进制可读得多。
namespace {
std::string hr_field(HRESULT hr) {
    char buf[16] = {};
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}
}  // namespace

bool Graphics::initialize(HWND hwnd) {
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
    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
        &sc_desc, &swap_chain, &device, &got, &context);
    if (FAILED(hr)) {
        // 设备创建失败是"启动即不可用"，必须留痕：常见原因是显卡驱动过旧 / 远程桌面 /
        // 虚拟机缺 D3D11 支持。之前这里只 return false，用户在入口处只看到"程序没起来"。
        lr::log::error("platform", "D3D11CreateDeviceAndSwapChain failed " +
                                       lr::log::kv("hr", hr_field(hr)));
        shutdown();
        return false;
    }
    if (!create_rtv()) {
        shutdown();
        return false;
    }
    RECT rc{};
    if (GetClientRect(hwnd, &rc)) { cur_w = rc.right - rc.left; cur_h = rc.bottom - rc.top; }
    lr::log::info("platform", "d3d11 device ready " + lr::log::kv("w", static_cast<int>(cur_w)) +
                                  " " + lr::log::kv("h", static_cast<int>(cur_h)));
    return true;
}

bool Graphics::create_rtv() {
    if (swap_chain == nullptr || device == nullptr) return false;
    ID3D11Texture2D* back = nullptr;
    const HRESULT hb = swap_chain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hb) || back == nullptr) {
        lr::log::error("platform", "swapchain GetBuffer failed " + lr::log::kv("hr", hr_field(hb)));
        return false;
    }
    const HRESULT hr = device->CreateRenderTargetView(back, nullptr, &rtv);
    back->Release();
    if (FAILED(hr)) {
        lr::log::error("platform",
                       "CreateRenderTargetView failed " + lr::log::kv("hr", hr_field(hr)));
        return false;
    }
    return true;
}

void Graphics::resize(UINT w, UINT h) {
    if (!swap_chain || w == 0 || h == 0) return;
    if (rtv && w == cur_w && h == cur_h) return;  // 尺寸未变：不重建（重建会丢后备缓冲内容）
    if (rtv) { rtv->Release(); rtv = nullptr; }
    const HRESULT hr = swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (SUCCEEDED(hr)) {
        create_rtv();
        cur_w = w;
        cur_h = h;
    } else {
        // 重建失败会留下"有尺寸、无渲染目标"的状态；如实记下 HRESULT 并按设备丢失处理。
        lr::log::error("platform", "ResizeBuffers failed " + lr::log::kv("hr", hr_field(hr)) +
                                       " " + lr::log::kv("w", static_cast<int>(w)) + " " +
                                       lr::log::kv("h", static_cast<int>(h)));
        device_lost = true;
    }
}

void Graphics::render_frame() {
    if (!rtv || device_lost) return;  // resize 失败瞬间 / 设备已丢失：静默跳过本帧
    const float clear[4] = { 0.10f, 0.14f, 0.18f, 1.0f };
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    const HRESULT hr = swap_chain->Present(1, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        const HRESULT reason = device->GetDeviceRemovedReason();
        lr::log::error("platform", "device removed " + lr::log::kv("hr", hr_field(hr)) +
                                       " " + lr::log::kv("reason", hr_field(reason)));
        device_lost = true;   // 只标记、不再呈现；重建设备属未实现的边界（见 docs §3.4）
    }
}

void Graphics::shutdown() {
    // 释放顺序必须由内向外（视图 → 交换链 → 上下文 → 设备）：反过来会留下悬垂引用。
    if (rtv) { rtv->Release(); rtv = nullptr; }
    if (swap_chain) { swap_chain->Release(); swap_chain = nullptr; }
    if (context) { context->Release(); context = nullptr; }
    if (device) { device->Release(); device = nullptr; }
    cur_w = cur_h = 0;
}

// ---------------- ImGui 引导 ----------------

namespace {
// ImGui 内部错误的落点：写进日志（GUI 侧的工具提示由 ImGui 自己显示）。
// 注意这是被 ImGui 内部调用的回调：拼串可能抛（bad_alloc），必须自己兜住 ——
// 在"诊断路径"上抛异常，会把一次可恢复的 UI 错误升级成进程终止。
void imgui_error_sink(ImGuiContext*, void*, const char* msg) noexcept {
    try {
        lr::log::error("ui", std::string("imgui error: ") + (msg ? msg : "(null)"));
    } catch (...) {
    }
}
}  // namespace

void ImGuiRaii::initialize(HWND hwnd) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ctx = true;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // 关掉 ImGui 自己的 imgui.ini 持久化：「把文件拖到 exe 上」时 Explorer 会把工作目录
    // 设为被拖文件目录，默认相对路径会在用户目录里凭空生成 imgui.ini。窗口/布局状态由
    // LilithReader.ini 自管，故直接禁用。
    io.IniFilename = nullptr;

    // ---- 错误恢复与错误接管（Phase 7）----
    //
    // ImGui 1.93 对"可恢复错误"（栈不平、Begin/End 误用、ID 冲突）自带恢复机制：它会自愈到
    // 一个可用状态，而不是直接把进程带走。我们做两件事：
    //   1) 保留工具提示（EnableTooltip）：错误**必须**被浮出水面 —— ImGui 官方的原则是
    //      "不允许错误静默"，一条只在日志里的记录对正在用的用户没有意义；
    //   2) 把错误同时接到 lr::log（ErrorCallback）：日志里留下时间线与前后文，便于事后追溯。
    // 二者一起才构成"既看见、又查得到"。
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableTooltip = true;
    io.ConfigErrorRecoveryEnableDebugLog = true;
    // 断言只在 Debug 保留：开发时希望"就地断在出错的那一行"，而发布版必须遵守
    // "一个错误不拖垮进程"—— 可恢复错误降级为"记录 + 自愈"（ADR-078）。
#ifdef _DEBUG
    io.ConfigErrorRecoveryEnableAssert = true;
#else
    io.ConfigErrorRecoveryEnableAssert = false;
#endif
    ig::redirect_errors_to(&imgui_error_sink, nullptr);

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
    g_base_style = ImGui::GetStyle();
    g_base_style_ready = true;

    // ---- 字体装载（ADR-048）----
    //
    // 1) 先换 FreeType 装载器：内置 stb_truetype 只吃 TrueType，而子集是 CFF/OTF；
    //    且小字号 CJK 的栅格化质量明显更好（imgui_freetype 的官方用途）。
    //    等效于在 imconfig.h 里 #define IMGUI_ENABLE_FREETYPE，但不必改第三方文件。
    io.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());

    // 2) UI 主字体 = **随 exe 内嵌的子集**（assets/ui_font_subset.otf，Noto Sans SC 子集）。
    //    这样界面观感不随"这台机器装没装某款系统字体"漂移。覆盖范围有自动化断言兜底
    //    （tests 扫描源码字符集 ⊆ 子集 cmap），故界面自述文字不可能出现豆腐块。
    //    字形**按需栅格化**（DX11 后端声明了 RendererHasTextures，走动态图集路径），
    //    因此这里 ranges 传 nullptr 不会预烘整本字库。
    int ui_font_size = 0;
    const void* ui_font_data = find_embedded_resource(kUiFontResId, &ui_font_size);
    bool ui_font_ok = false;
    if (ui_font_data != nullptr && ui_font_size > 0) {
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;   // 资源内存归 exe 映射所有，ImGui 不得释放
        cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
        ui_font_ok = io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(ui_font_data), ui_font_size,
                                                    kUiFontBasePx, &cfg, nullptr) != nullptr;
    }
    if (!ui_font_ok) {
        // 资源缺失（异常构建或资源被剥离）：退回系统微软雅黑，界面仍可用但不保证覆盖
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", kUiFontBasePx,
                                     nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }

    // 3) 生僻字 fallback：把系统微软雅黑**合并**进同一 ImFont，只兜子集里没有的字形
    //    （文件名、PDF 目录标题里可能出现生僻字）。合并顺序即优先级 —— 子集在前，
    //    常规字仍走内嵌子集；系统缺该字体时 ImGui 静默跳过，界面自述文字不受影响。
    {
        ImFontConfig fb;
        fb.MergeMode = true;
        fb.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LightHinting;
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", kUiFontBasePx, &fb, nullptr);
    }

    // 图标字体：把 Segoe MDL2 Assets 的相关字形**合并**进同一字体。
    // 只烘 kIconGlyphs 里用到的二十余个 PUA 码位，图集不膨胀。字体缺失、或任一码位
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
        // 度量校正（ADR-045）：
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
    if (!win32) lr::log::error("platform", "ImGui_ImplWin32_Init failed");
    if (!dx11) lr::log::error("platform", "ImGui_ImplDX11_Init failed");
    if (!ui_font_ok)
        lr::log::warn("platform", "embedded ui font unavailable, fell back to system font");
    if (!g_icons_ok) lr::log::info("platform", "icon glyphs unavailable, text labels only");

    // DPI：按窗口所在显示器缩放字体与界面度量（100% 时为 1.0，等价于旧行为）
    refresh_dpi_scale();
    apply_ui_scale();
}

void ImGuiRaii::new_frame() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void ImGuiRaii::shutdown() {
    if (dx11) ImGui_ImplDX11_Shutdown();
    if (win32) ImGui_ImplWin32_Shutdown();
    if (ctx) ImGui::DestroyContext();
}

// ---------------- 用户偏好持久化 ----------------

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
    // 资源档位是缓存预算、tile 单边和 worker 并发的唯一入口。
    // 旧版 BudgetMB 只用于一次性兼容映射，避免升级后突然改变用户的资源策略。
    const int old_mb = lr::read_ini_int_ex(g_ini_path, L"cache", L"BudgetMB", -1);
    const int old_tier = lr::read_ini_int_ex(g_ini_path, L"cache", L"ResourceTier", -1);
    if (old_tier >= 0) {
        g_prefs.resource_tier = std::clamp(old_tier, 0, 2);
    } else if (old_mb >= 0) {
        g_prefs.resource_tier = old_mb < 384 ? 0 : (old_mb < 640 ? 1 : 2);
    } else {
        g_prefs.resource_tier = 1;
    }
    if (old_tier < 0 && old_mb >= 0)
        lr::write_ini_int(g_ini_path, L"cache", L"ResourceTier", g_prefs.resource_tier);
    g_prefs.smart_match = std::clamp(
        lr::read_ini_int_ex(g_ini_path, L"reading", L"SmartMatch", kSmartMatchAsk),
        kSmartMatchOff, kSmartMatchAuto);
    g_user_scale = g_prefs.ui_scale;

    // 启动时把生效的偏好记一行：用户报"设置没生效 / 数字不对"时，这一行就能定性。
    lr::log::info("platform", "prefs " + lr::log::kv("ui_scale", static_cast<double>(g_prefs.ui_scale)) +
                                  " " + lr::log::kv("theme", g_prefs.theme) + " " +
                                  lr::log::kv("resource_tier", g_prefs.resource_tier) + " " +
                                  lr::log::kv("motion", g_prefs.motion ? 1 : 0) + " " +
                                  lr::log::kv("smart_match", g_prefs.smart_match));
}

void save_prefs() {
    lr::write_ini_float(g_ini_path, L"ui", L"UiScale", g_prefs.ui_scale);
    lr::write_ini_int(g_ini_path, L"ui", L"Theme", g_prefs.theme);
    lr::write_ini_int(g_ini_path, L"ui", L"AutoHideToolbar", g_prefs.auto_hide_toolbar ? 1 : 0);
    lr::write_ini_int(g_ini_path, L"ui", L"Motion", g_prefs.motion ? 1 : 0);
    lr::write_ini_float(g_ini_path, L"ui", L"GapPercent", g_prefs.gap_percent);
    lr::write_ini_int(g_ini_path, L"cache", L"ResourceTier", g_prefs.resource_tier);
    lr::write_ini_int(g_ini_path, L"reading", L"SmartMatch", g_prefs.smart_match);
}

// ---------------- 按键绑定持久化（[keys] 节，ADR-054） ----------------
//
// 值形如 "Ctrl+O|F11"：两个槽（主键 / 备键），空槽留空。整条缺失 → 保留默认。
// 键名一律用 ImGui::GetKeyName 的原文（纯 ASCII），故这里只有宽窄转换、不涉及编码歧义；
// 显示用的美化名（"Ctrl+="）只用于界面，不落盘。

void load_binds() {
    reset_binds_to_default();
    for (int i = 0; i < kCmdCount; ++i) {
        const std::wstring wkey = lr::utf8_to_wide(kCmds[i].id);
        const std::wstring wval = lr::read_ini_string_ex(g_ini_path, L"keys", wkey.c_str(), L"");
        if (wval.empty()) continue;   // 无记录：保留默认
        const std::string v = lr::wide_to_utf8(wval);
        const std::size_t bar = v.find('|');
        const std::string a = (bar == std::string::npos) ? v : v.substr(0, bar);
        const std::string b = (bar == std::string::npos) ? std::string() : v.substr(bar + 1);
        g_binds[i][0] = chord_from_string(a);
        g_binds[i][1] = chord_from_string(b);
    }
}

void save_binds() {
    for (int i = 0; i < kCmdCount; ++i) {
        std::string v = chord_to_string(g_binds[i][0]);
        v += '|';
        v += chord_to_string(g_binds[i][1]);
        const std::wstring wkey = lr::utf8_to_wide(kCmds[i].id);
        const std::wstring wval = lr::utf8_to_wide(v);
        lr::write_ini_string(g_ini_path, L"keys", wkey.c_str(), wval.c_str());
    }
}

// ---------------- 输入法关联 ----------------

// 阅读窗口平时脱离输入法（让字母/数字快捷键生效）；文本输入激活时临时关联回来，
// 否则中文打不进去（落实 ADR-028 的后续要求）。
void update_ime_association() {
    const bool want = ImGui::GetIO().WantTextInput;
    if (want == g_ime_attached) return;
    g_ime_attached = want;
    ImmAssociateContext(g_hwnd, want ? g_saved_ime : nullptr);
}

// ---------------- 全屏 ----------------

void toggle_fullscreen() {
    if (!g_hwnd) return;
    if (!g_fullscreen) {
        GetWindowPlacement(g_hwnd, &g_prev_placement);
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, nullptr,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        g_fullscreen = true;
    } else {
        // 退出全屏：**先提交新样式（FRAMECHANGED），再落位**。
        // 反过来（原实现：先 SetWindowPlacement 再补 FRAMECHANGED）会出问题：
        //   SetWindowPlacement 用的是尚未生效的 WS_POPUP（无边框）度量来摆放窗口，
        //   随后的 FRAMECHANGED 重算非客户区、把窗口再挪一次 —— 肉眼即"变了又归位"。
        // 这里先让样式在**全屏矩形上**生效（NOMOVE|NOSIZE，几何不动、无可见中间态），
        // 再用 SetWindowPlacement 一次落位：几何只变一次，且工作区坐标换算交给系统
        // （rcNormalPosition 是工作区坐标，手工 SetWindowPos 在多显示器下会偏）。
        SetWindowLongPtrW(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        SetWindowPlacement(g_hwnd, &g_prev_placement);
        g_fullscreen = false;
    }
}

// ---------------- 剪贴板（Phase 8） ----------------
//
// 为什么不用 ImGui 的 SetClipboardText：它只能放文本，且依赖后端实现；
// 图片必须走原生 CF_DIB，两者放一处才好统一处理"剪贴板被别的进程占用"这一失败路径
// （OpenClipboard 会失败，此时必须如实报错，而不是假装复制成功）。

bool set_clipboard_text(const std::string& utf8) {
    if (utf8.empty()) return false;
    // 内部一律 UTF-8；剪贴板面向 Windows 应用，用 CF_UNICODETEXT（UTF-16）。
    // 直接以 UTF-8 写 CF_TEXT 会让中文变乱码（那走的是 ANSI 代码页）。
    const std::wstring w = lr::utf8_to_wide(utf8);
    if (w.empty()) return false;

    const std::size_t bytes = (w.size() + 1) * sizeof(wchar_t);   // 含结尾 '\0'
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem == nullptr) return false;
    if (void* dst = GlobalLock(mem)) {
        std::memcpy(dst, w.c_str(), bytes);
        GlobalUnlock(mem);
    } else {
        GlobalFree(mem);
        return false;
    }

    // 剪贴板是全局独占资源：别的进程正开着时 OpenClipboard 会失败。
    // 重试几次再放弃 —— 实践中多数占用只有几十毫秒。
    bool ok = false;
    for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
        if (!OpenClipboard(g_hwnd)) {
            Sleep(10);
            continue;
        }
        ok = EmptyClipboard() != FALSE;
        if (ok) {
            // SetClipboardData 成功后所有权移交系统；失败则我们必须自己释放。
            if (SetClipboardData(CF_UNICODETEXT, mem) != nullptr) {
                mem = nullptr;
            } else {
                ok = false;
            }
        }
        CloseClipboard();
    }
    if (mem != nullptr) GlobalFree(mem);
    return ok;
}

bool set_clipboard_image_rgba(int w, int h, const std::uint8_t* rgba) {
    if (w <= 0 || h <= 0 || rgba == nullptr) return false;

    // CF_DIB：BITMAPINFOHEADER + 像素。约定要点：
    //   · 32bpp BI_RGB，**自下而上**存储（第 0 行是图像最后一行）；
    //   · 通道顺序是 BGRA，不是 RGBA —— 写错会得到"红蓝互换"的图；
    //   · alpha 一律写成 255：多数应用把 32bpp BI_RGB 当作"不含 alpha"处理，
    //     若沿用源图的 0，Word/画图里会整张变黑。
    const std::size_t pixel_bytes = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u;
    const std::size_t total = sizeof(BITMAPINFOHEADER) + pixel_bytes;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, total);
    if (mem == nullptr) return false;

    if (void* raw = GlobalLock(mem)) {
        auto* bi = static_cast<BITMAPINFOHEADER*>(raw);
        std::memset(bi, 0, sizeof(BITMAPINFOHEADER));
        bi->biSize = sizeof(BITMAPINFOHEADER);
        bi->biWidth = w;
        bi->biHeight = h;          // 正值 = 自下而上
        bi->biPlanes = 1;
        bi->biBitCount = 32;
        bi->biCompression = BI_RGB;
        bi->biSizeImage = static_cast<DWORD>(pixel_bytes);

        auto* dst = reinterpret_cast<std::uint8_t*>(bi + 1);
        for (int y = 0; y < h; ++y) {
            const std::uint8_t* srow =
                rgba + static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 4u;
            std::uint8_t* drow =
                dst + static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w) * 4u;
            for (int x = 0; x < w; ++x) {
                drow[x * 4 + 0] = srow[x * 4 + 2];   // B ← R
                drow[x * 4 + 1] = srow[x * 4 + 1];   // G
                drow[x * 4 + 2] = srow[x * 4 + 0];   // R ← B
                drow[x * 4 + 3] = 255;
            }
        }
        GlobalUnlock(mem);
    } else {
        GlobalFree(mem);
        return false;
    }

    bool ok = false;
    for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
        if (!OpenClipboard(g_hwnd)) {
            Sleep(10);
            continue;
        }
        ok = EmptyClipboard() != FALSE;
        if (ok) {
            if (SetClipboardData(CF_DIB, mem) != nullptr) {
                mem = nullptr;
            } else {
                ok = false;
            }
        }
        CloseClipboard();
    }
    if (mem != nullptr) GlobalFree(mem);
    return ok;
}

}  // namespace lr::app
