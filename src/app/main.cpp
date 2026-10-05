// main.cpp — Lilith Reader 应用层：入口
//
// 职责（进程与窗口的生命周期，不含任何界面绘制与文档逻辑）：
//   · 命令行解析（LilithReader.exe <文档路径>）
//   · 进程 DPI 感知、窗口类注册、窗口创建与恢复（exe 同目录 LilithReader.ini）
//   · 主消息循环（ImGui 帧 → draw_shell → 呈现；模态文件对话框在帧间执行）
//   · 窗口过程：尺寸/DPI 变化、拖放打开、关闭时落盘阅读位置与窗口状态
//
// 分层（架构文档 §1）：main 只做"把外部事件接进来"；界面在 ui.cpp、状态在 session.cpp、
// 底座在 platform.cpp。UI 线程零 fz_*（渲染经 render 调度层）、零阻塞。
//
// 项目纪律：不在开发中实际运行本软件做测试（编译/自动化测试除外）；
// 人工运行验证项统一记录在 docs/04-人工验证.md。

#include "app_internal.h"

// Win32 后端消息处理器的前向声明。两点说明：
//  1. 该声明**刻意不出现在 imgui_impl_win32.h**（避免那头引入 windows.h），官方要求使用者
//     自行拷贝这行（见该头文件第 34~36 行）。
//  2. 必须放在**全局作用域**：ImGui 的实现是全局命名空间的 C 链接符号，声明若落进
//     `lr::app` 或其匿名命名空间，就会被赋予内部链接、生成不同的修饰名而链接失败（LNK2001）。
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace lr::app {
namespace {

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// 窗口矩形是否落在虚拟屏幕内且不至于太小（恢复窗口状态前的合法性校验）。
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

}  // namespace

// 帧与帧之间调用：模态文件对话框自带消息循环，绝不能在 ImGui 一帧中途调用
// （否则窗口消息会在半帧状态下被后端处理）。
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

namespace {

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
        if (!g_fullscreen) {   // 全屏态不覆盖保存的正常态矩形
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
}  // namespace lr::app

int WINAPI wWinMain(_In_ HINSTANCE inst, _In_opt_ HINSTANCE,
                    _In_ LPWSTR cmd_line, _In_ int show) {
    using namespace lr::app;

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
    // 应用图标：资源里同时提供多尺寸，窗口与任务栏各取所需。
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

    // 页缓存字节预算（ADR-030）：来自 [cache] BudgetMB（设置界面可调）；渲染层再钳制。
    g_renderer->set_cache_budget(
        static_cast<std::size_t>(g_prefs.cache_mb) * 1024ull * 1024ull);

    g_ui.initialize(g_hwnd);
    DragAcceptFiles(g_hwnd, TRUE);

    // 让阅读窗口脱离输入法（ADR-028）：输入法启用时 Windows 会把字母/数字键的
    // WM_KEYDOWN 换成 VK_PROCESSKEY(0xE5)，ImGui 后端不映射该键码 → 这些键完全不置位，
    // 所有字母/数字快捷键（1~4 切列、F 回 fit-width、G 跳页…）静默失效。
    // 这里保存默认输入法上下文，平时脱离；仅在文本输入激活（密码框等）时关联回来，
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

        // 「打开文档…」：模态文件对话框放到帧与帧之间执行（见 open_file_dialog_now 注释）。
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