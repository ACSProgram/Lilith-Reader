// main.cpp — Lilith Reader 应用外壳（Phase 1）
// Win32 + D3D11 + ImGui(1.93)：命令行打开、拖放打开、窗口状态持久化、占位页。
// 文档渲染核心（MuPDF）Phase 2 接入；当前拖入文档仅识别格式并显示占位页。
//
// 项目纪律：不在开发中实际运行本软件做测试（编译/自动化测试除外）；
// 人工运行验证项统一记录在 docs/04-人工验证.md。

#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>
#include <string>

import lilithreader.utils;

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "implot.h"

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

// ---------------- D3D11（RAII，Phase 0 原样保留） ----------------
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

// ---------------- ImGui（RAII） ----------------
struct ImGuiRaii {
    bool win32 = false, dx11 = false, ctx = false, plot = false;

    void initialize(HWND hwnd) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ctx = true;
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // 中文字体：系统微软雅黑（Phase 6 换为 exe 内嵌子集字体）
        io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\msyh.ttc", 18.0f,
            nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());

        ImPlot::CreateContext();
        plot = true;
        win32 = ImGui_ImplWin32_Init(hwnd);
        dx11 = ImGui_ImplDX11_Init(g_gfx.device, g_gfx.context);
    }

    void new_frame() {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }

    void shutdown() {
        if (dx11) ImGui_ImplDX11_Shutdown();
        if (win32) ImGui_ImplWin32_Shutdown();
        if (plot) ImPlot::DestroyContext();
        if (ctx) ImGui::DestroyContext();
    }
} g_ui;

// ---------------- 应用状态 ----------------
// Phase 1 只做到"识别 + 占位页"；真正打开文档是 Phase 2 的事。
struct AppState {
    enum class OpenState { None, NotFound, Recognized, Unsupported };
    OpenState state = OpenState::None;
    std::wstring path_w;
    std::string path_u8, name_u8, ext_u8;

    void open(HWND hwnd, std::wstring p) {
        p = lr::to_absolute(p);
        path_w = p;
        path_u8 = lr::wide_to_utf8(p);
        name_u8 = lr::wide_to_utf8(lr::file_name_of(p));
        ext_u8 = lr::wide_to_utf8(lr::extension_of(p));
        state = !lr::file_exists(p)       ? OpenState::NotFound
                : lr::is_supported(p)     ? OpenState::Recognized
                                          : OpenState::Unsupported;
        // TODO(Phase 2): Recognized → 交给 document 层真正打开
        if (hwnd) {
            const std::wstring title =
                std::wstring(L"Lilith Reader — ") + lr::file_name_of(p);
            SetWindowTextW(hwnd, title.c_str());
        }
    }

    void close(HWND hwnd) {
        state = OpenState::None;
        path_w.clear();
        path_u8.clear();
        name_u8.clear();
        ext_u8.clear();
        if (hwnd) SetWindowTextW(hwnd, kWindowTitle);
    }
} g_app;

HWND g_hwnd = nullptr;
std::wstring g_ini_path;
bool g_show_debug = false;  // F3 切换调试浮层

// 保存的窗口矩形是否仍可用（至少 100x100 落在虚拟屏幕内，最小 400x300）
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

// ---------------- UI ----------------
constexpr const char* kFormatsLine = "支持 PDF · EPUB · MOBI · FB2 · CBZ · XPS";
constexpr ImVec4 kColBright{ 0.92f, 0.93f, 0.95f, 1.0f };
constexpr ImVec4 kColDim{ 0.58f, 0.62f, 0.66f, 1.0f };

void centered_text(const char* text, float dy, const ImVec4& col) {
    const ImVec2 ws = ImGui::GetWindowSize();
    const ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImVec2((ws.x - ts.x) * 0.5f, (ws.y - ts.y) * 0.5f + dy));
    ImGui::TextColored(col, "%s", text);
}

void draw_drop_guide() {
    centered_text("将文档拖入窗口打开", -56.0f, kColBright);
    centered_text(kFormatsLine, -12.0f, kColDim);
    centered_text("也可以用命令行：LilithReader.exe <文件路径>", 12.0f, kColDim);
    centered_text("按 Esc 退出", 56.0f, kColDim);
}

void draw_recognized() {
    centered_text(g_app.name_u8.c_str(), -84.0f, kColBright);
    const std::string line = "格式 " + g_app.ext_u8 + " · 已识别";
    centered_text(line.c_str(), -42.0f, kColDim);
    centered_text("文档渲染核心（MuPDF）将在 Phase 2 接入", 0.0f, kColDim);
    centered_text("按 Esc 返回", 42.0f, kColDim);
}

void draw_unsupported() {
    centered_text("暂不支持的格式", -84.0f, kColBright);
    centered_text(g_app.name_u8.c_str(), -42.0f, kColDim);
    centered_text(kFormatsLine, 0.0f, kColDim);
    centered_text("按 Esc 返回", 42.0f, kColDim);
}

void draw_not_found() {
    centered_text("文件不存在", -42.0f, kColBright);
    centered_text(g_app.path_u8.c_str(), 0.0f, kColDim);
    centered_text("按 Esc 返回", 42.0f, kColDim);
}

void draw_debug_overlay() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    if (ImGui::Begin("##debug", nullptr, ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::Text("%.1f FPS / %.2f ms", io.Framerate, 1000.0f / io.Framerate);
        ImGui::TextDisabled("%dx%d @ %.0f%%", (int)io.DisplaySize.x,
                            (int)io.DisplaySize.y, io.FontGlobalScale * 100.0f);
    }
    ImGui::End();
}

void draw_shell(HWND hwnd) {
    if (ImGui::IsKeyPressed(ImGuiKey_F3)) g_show_debug ^= 1;

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
                 ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);

    switch (g_app.state) {
    case AppState::OpenState::None:        draw_drop_guide();   break;
    case AppState::OpenState::Recognized:  draw_recognized();   break;
    case AppState::OpenState::Unsupported: draw_unsupported();  break;
    case AppState::OpenState::NotFound:    draw_not_found();    break;
    }
    ImGui::End();

    if (g_show_debug) draw_debug_overlay();

    // Esc：有文档 → 返回引导页；无文档 → 退出
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g_app.state != AppState::OpenState::None)
            g_app.close(hwnd);
        else
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

// ---------------- 窗口过程 ----------------
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        g_gfx.resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = { 480, 320 };
        return 0;
    }
    case WM_DROPFILES: {
        const HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT len = DragQueryFileW(drop, 0, nullptr, 0);
        if (len > 0 && len < 4096) {
            std::wstring path(len, L'\0');
            DragQueryFileW(drop, 0, path.data(), len + 1);
            g_app.open(hwnd, path);
        }
        DragFinish(drop);
        return 0;
    }
    case WM_DPICHANGED:
        // Per-Monitor DPI v2：按系统建议矩形调整窗口
        if (const RECT* r = reinterpret_cast<const RECT*>(lp))
            SetWindowPos(hwnd, nullptr, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_DESTROY: {
        // 退出前持久化窗口状态（正常态矩形 + 是否最大化）
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
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE,
                    _In_ LPWSTR cmd_line, _In_ int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 命令行解析：LilithReader.exe <文档路径>
    // 注意 wWinMain 的 cmd_line 不含程序名，argv[0] 即第一个实参
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

    // 恢复上次的窗口位置/尺寸（无效则用默认值）
    const lr::WindowState saved = lr::load_window_state(g_ini_path);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1280, h = 800;
    if (saved.valid && placement_usable(saved)) {
        x = saved.x; y = saved.y; w = saved.w; h = saved.h;
    }

    g_hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                             x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd || !g_gfx.initialize(g_hwnd)) return 1;
    g_ui.initialize(g_hwnd);
    DragAcceptFiles(g_hwnd, TRUE);

    if (!doc_path.empty()) g_app.open(g_hwnd, doc_path);

    ShowWindow(g_hwnd,
               (saved.valid && saved.maximized) ? SW_MAXIMIZE : show);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto quit;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        g_ui.new_frame();
        draw_shell(g_hwnd);
        ImGui::Render();
        g_gfx.render_frame();
    }
quit:
    g_ui.shutdown();
    g_gfx.shutdown();
    return 0;
}
