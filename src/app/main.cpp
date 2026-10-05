// main.cpp — Lilith Reader Phase 0 框架：Win32 + D3D11 + ImGui(1.93) + ImPlot(1.1)
// 下一阶段在此骨架上迁移 MuPDF 文档核心与自研画布。

#include <windows.h>
#include <shellscalingapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>

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
        sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // 翻转模型，低延迟

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
        swap_chain->Present(1, 0); // 垂直同步
    }

    void shutdown() {
        if (rtv) rtv->Release();
        if (swap_chain) swap_chain->Release();
        if (context) context->Release();
        if (device) device->Release();
    }
} g_gfx;

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

    void end_frame() {
        ImGui::Render();
    }

    void shutdown() {
        if (dx11) ImGui_ImplDX11_Shutdown();
        if (win32) ImGui_ImplWin32_Shutdown();
        if (plot) ImPlot::DestroyContext();
        if (ctx) ImGui::DestroyContext();
    }
} g_ui;

void draw_ui() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560, 300), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Lilith Reader — Phase 0 框架就绪")) {
        ImGui::TextUnformatted("Win32 + D3D11 + ImGui 1.93 + ImPlot 1.1");
        ImGui::Text("帧率: %.1f FPS / %.2f ms", ImGui::GetIO().Framerate,
                    1000.0f / ImGui::GetIO().Framerate);
        ImGui::Separator();
        static bool show_demo = false;
        ImGui::Checkbox("显示 ImGui Demo 窗口", &show_demo);
        if (show_demo) ImGui::ShowDemoWindow(&show_demo);
        static bool show_plot_demo = false;
        ImGui::Checkbox("显示 ImPlot Demo 窗口", &show_plot_demo);
        if (show_plot_demo) ImPlot::ShowDemoWindow(&show_plot_demo);
        ImGui::Separator();
        ImGui::TextDisabled("拖放文件打开（Phase 1 实现）");
    }
    ImGui::End();
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        g_gfx.resize(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE,
                    _In_ LPWSTR cmd_line, _In_ int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    // TODO(Phase 1): 解析 cmd_line 文档路径；无参数时显示拖放引导
    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle,
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                1280, 800, nullptr, nullptr, inst, nullptr);
    if (!hwnd || !g_gfx.initialize(hwnd)) return 1;
    g_ui.initialize(hwnd);

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto quit;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        g_ui.new_frame();
        draw_ui();
        g_ui.end_frame();
        g_gfx.render_frame();
    }
quit:
    g_ui.shutdown();
    g_gfx.shutdown();
    return 0;
}
