// platform.cpp — Lilith Reader 应用层：平台地基
//
// 职责（不感知文档/阅读状态，只提供"外壳运行所需的底座"）：
//   · D3D11 设备与交换链（RAII）
//   · ImGui 上下文与后端引导、中文字体 + 图标字形合并
//   · DPI / 界面缩放（g_dpi_scale × g_user_scale）
//   · 主题（浅/深/跟随系统；反色强制深色）与配色
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
    "\xEE\xA0\xAD\xEE\xA2\xA5\xEE\xA3\xA5\xEE\x9C\xA1";                            // E82D E8A5 E8E5 E721

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

// ImGui 控件的配色（圆角/间距等几何量在 ImGuiRaii::initialize 里设定，随 DPI 缩放）。
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

// ---------------- D3D11 ----------------

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
    if (FAILED(D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
            &sc_desc, &swap_chain, &device, &got, &context)))
        return false;
    return create_rtv();
}

bool Graphics::create_rtv() {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
    const bool ok = SUCCEEDED(device->CreateRenderTargetView(back, nullptr, &rtv));
    back->Release();
    return ok;
}

void Graphics::resize(UINT w, UINT h) {
    if (!swap_chain || w == 0 || h == 0) return;
    if (rtv) { rtv->Release(); rtv = nullptr; }
    if (SUCCEEDED(swap_chain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0)))
        create_rtv();
}

void Graphics::render_frame() {
    if (!rtv) return;  // resize 失败瞬间可能无渲染目标
    const float clear[4] = { 0.10f, 0.14f, 0.18f, 1.0f };
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swap_chain->Present(1, 0);
}

void Graphics::shutdown() {
    if (rtv) rtv->Release();
    if (swap_chain) swap_chain->Release();
    if (context) context->Release();
    if (device) device->Release();
}

// ---------------- ImGui 引导 ----------------

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
    // 缓存预算的默认值与范围直接取自渲染层的单一真源（page_cache.ixx），不在此另立常量。
    const int cache_mb_default =
        static_cast<int>(lr::kCacheBudgetDefault / (1024ull * 1024ull));
    g_prefs.cache_mb = std::clamp(
        lr::read_ini_int_ex(g_ini_path, L"cache", L"BudgetMB", cache_mb_default),
        static_cast<int>(lr::kCacheBudgetMin / (1024ull * 1024ull)),
        static_cast<int>(lr::kCacheBudgetMax / (1024ull * 1024ull)));
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

}  // namespace lr::app