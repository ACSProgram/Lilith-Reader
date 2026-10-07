// main.cpp — Lilith Reader 应用层：入口
//
// 职责（进程与窗口的生命周期，不含任何界面绘制与文档逻辑）：
//   · 命令行解析（LilithReader.exe <文档路径>）
//   · 进程级崩溃防线安装、日志初始化、上次异常退出的提示
//   · 进程 DPI 感知、窗口类注册、窗口创建与恢复（exe 同目录 LilithReader.ini）
//   · 主消息循环（ImGui 帧 → draw_shell → 呈现；模态文件对话框在帧间执行）
//   · **帧级异常边界**（ADR-078）：任何一块界面绘制抛异常都只作废这一帧，不带走进程
//   · 窗口过程：尺寸/DPI 变化、拖放打开、关闭时落盘阅读位置与窗口状态
//
// 分层（架构文档 §1）：main 只做"把外部事件接进来"；界面在 ui.cpp、状态在 session.cpp、
// 底座在 platform.cpp。UI 线程零 fz_*（渲染经 render 调度层）、零阻塞。
//
// 项目纪律：不在开发中实际运行本软件做测试（编译/自动化测试除外）；
// 人工运行验证项统一记录在 docs/04-人工验证.md。

#include "app_internal.h"

#include "crash.h"          // 进程级崩溃防线（Phase 7）
#include "imgui_stacks.h"   // 帧级栈平衡自检与 ImGui 异常后恢复（Phase 7）

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

// 崩溃摘要只取前几行交给弹窗：完整报告（含日志尾部）留在 crash\last_crash.txt 里，
// 弹窗的职责只是"告诉用户出过事、报告在哪"，不是把转储内容塞进模态框。
std::string crash_summary_head(const std::string& full) {
    constexpr std::size_t kMaxLines = 6;
    std::size_t lines = 0;
    for (std::size_t i = 0; i < full.size(); ++i) {
        if (full[i] == '\n' && ++lines >= kMaxLines) return full.substr(0, i);
    }
    return full;
}

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
    g_app.open_path_buf[0] = L'\0';
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_app.hwnd;
    ofn.lpstrFilter =
        L"支持的文档\0*.pdf;*.epub;*.mobi;*.fb2;*.cbz;*.xps;*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0"
        L"PDF\0*.pdf\0电子书\0*.epub;*.mobi;*.fb2\0压缩图集\0*.cbz\0图片\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0"
        L"所有文件\0*.*\0";
    ofn.lpstrFile = g_app.open_path_buf;
    ofn.nMaxFile = ARRAYSIZE(g_app.open_path_buf);
    ofn.lpstrTitle = L"打开文档";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) request_open_document(g_app.open_path_buf);
}

namespace {

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    // 切换输入语言（Alt+Shift / Win+Space）时，系统会给窗口**重新关联**输入法，
    // 把 ImmAssociateContext(hwnd, nullptr) 的脱离顶掉 —— 这里按当前状态再脱/再关联一次。
    // 必须放在 ImGui 后端处理器**之前**：后端会消费 WM_INPUTLANGCHANGE 并 return 1。
    if (msg == WM_INPUTLANGCHANGE)
        ImmAssociateContext(hwnd, g_app.ime_attached ? g_app.saved_ime : nullptr);
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            // 最小化：客户区为 0，既不能重建 swapchain，也不能继续渲染 —— 置标记让主循环
            // 整帧跳过（否则 0 尺寸视口下 ImGui 什么都画不出，整帧只剩清屏色，在收起动画里闪一下）。
            g_app.minimized = true;
            return 0;
        }
        g_app.minimized = false;
        // 只记录待处理尺寸，**不**在消息里直接重建 swapchain：WM_SIZE 会在
        // SetWindowPos / SetWindowPlacement 过程中同步到达（可能在 ImGui 一帧中途），
        // 当场 ResizeBuffers 会让"按旧尺寸排布的本帧绘制数据"被画进新尺寸后备缓冲，
        // 呈现为一帧拉伸/闪烁。改由帧首统一应用（见 main 循环）。
        g_app.resize_pending = true;
        g_app.resize_w = LOWORD(lp);
        g_app.resize_h = HIWORD(lp);
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
        save_reading_state();                 // 更新 g_app.session.state 快照（书签已实时更新）
        if (g_app.session.persist) g_app.session.persist->flush();    // 退出前把在途快照写完（ADR-082）
        if (!g_app.fullscreen) {   // 全屏态不覆盖保存的正常态矩形
            WINDOWPLACEMENT placement{ sizeof(placement) };
            if (GetWindowPlacement(hwnd, &placement)) {
                lr::WindowState s;
                s.x = placement.rcNormalPosition.left;
                s.y = placement.rcNormalPosition.top;
                s.w = placement.rcNormalPosition.right - placement.rcNormalPosition.left;
                s.h = placement.rcNormalPosition.bottom - placement.rcNormalPosition.top;
                s.maximized = (placement.showCmd == SW_SHOWMAXIMIZED);
                s.valid = true;
                lr::save_window_state(g_app.ini_path, s);
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

    // ---- 最早的接线（Phase 7）----
    // 崩溃标记的读取必须**先于** install()：install 会写 running.flag，晚于它读到的永远是
    // 本次进程自己的标记，结果就是"每次启动都提示上次异常退出"（实测踩过；take_last_crash
    // 内部另有 PID 自检兜底，双保险）。
    crash::set_phase("startup");
    std::string crash_summary;
    const bool had_last_crash = crash::take_last_crash(crash_summary);
    // 崩溃防线必须**早于任何可能抛异常的代码**：Crash 类问题不允许"来不及装处理器"。
    crash::install();
    lr::log::init(lr::exe_dir());
    lr::log::info("app", "LilithReader start " + lr::log::kv("exe_dir", lr::exe_dir()));

    // 上次异常退出：只提示一次，交给确认弹窗展示。
    if (had_last_crash) {
        lr::log::warn("app", "previous run ended abnormally; see crash\\last_crash.txt");
        g_app.last_crash_summary = crash_summary_head(crash_summary);
    }

    // 命令行解析：LilithReader.exe <文档路径>（wWinMain 的 cmd_line 不含程序名）
    std::wstring doc_path;
    if (cmd_line && *cmd_line) {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(cmd_line, &argc)) {
            if (argc >= 1) doc_path = argv[0];
            LocalFree(argv);
        }
    }

    g_app.ini_path = lr::exe_dir() + L"LilithReader.ini";
    g_app.state_path = lr::exe_dir() + L"reader_state.bin";
    g_app.session.state = lr::load_state(g_app.state_path);  // 阅读位置/书签（损坏则安全忽略为空）
    load_prefs();  // 用户偏好（界面缩放/主题/动效/间距/缓存预算）
    // 异步持久化服务（ADR-082）：在载入 g_app.session.state 之后创建；写盘在工作线程，
    // UI 线程只产快照。退出路径经 flush 保证写入（见 WM_DESTROY 与下面的 shutdown）。
    g_app.session.persist = std::make_unique<lr::PersistService>(g_app.state_path);

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

    const lr::WindowState saved = lr::load_window_state(g_app.ini_path);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = 1280, h = 800;
    if (saved.valid && placement_usable(saved)) {
        x = saved.x; y = saved.y; w = saved.w; h = saved.h;
    }

    g_app.hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW,
                             x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (!g_app.hwnd || !g_app.gfx.initialize(g_app.hwnd)) return 1;

    g_app.renderer = std::make_unique<lr::Renderer>(g_app.gfx.device);

    g_app.renderer->set_resource_tier(static_cast<lr::ResourceTier>(g_app.prefs.resource_tier));

    g_app.ui.initialize(g_app.hwnd);
    load_binds();  // 按键绑定（[keys] 节）：需在 ImGui 上下文建立后（按名反查键码）
    DragAcceptFiles(g_app.hwnd, TRUE);

    // 让阅读窗口脱离输入法（ADR-028）：输入法启用时 Windows 会把字母/数字键的
    // WM_KEYDOWN 换成 VK_PROCESSKEY(0xE5)，ImGui 后端不映射该键码 → 这些键完全不置位，
    // 所有字母/数字快捷键（1~4 切列、F 回 fit-width、G 跳页…）静默失效。
    // 这里保存默认输入法上下文，平时脱离；仅在文本输入激活（密码框等）时关联回来，
    // 使中文可输入（见 update_ime_association，落实 ADR-028 的后续要求）。
    g_app.saved_ime = ImmAssociateContext(g_app.hwnd, nullptr);
    g_app.ime_attached = false;

    if (!doc_path.empty()) request_open_document(std::move(doc_path));

    ShowWindow(g_app.hwnd, (saved.valid && saved.maximized) ? SW_MAXIMIZE : show);
    UpdateWindow(g_app.hwnd);

    // 上次异常退出的提示：复用既有的通用确认弹窗（ADR-063），不新增界面类型。
    // 主按钮直接打开报告目录 —— 用户"看得到、点得动"，而不是只收到一句看不见的日志。
    if (!g_app.last_crash_summary.empty()) {
        request_confirm(ConfirmKind::CrashNotice,
                        std::string("上次运行异常退出"),
                        g_app.last_crash_summary +
                            "\n\n完整摘要（含转储文件名与日志尾部）在报告目录的 last_crash.txt 里。",
                        { { "报告目录", lr::wide_to_utf8(crash::report_dir()) } },
                        std::string("打开报告文件夹"), std::string("忽略"));
        g_app.last_crash_summary.clear();
    }

    crash::set_phase("frame");

    MSG msg{};
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto quit;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        // 最小化期间整帧不渲染（见 WM_SIZE）：既省 CPU/GPU，也避免"0 尺寸视口 → 整帧清屏色"
        // 在最小化的收起动画里闪一下。消息照常泵，恢复时下一条 WM_SIZE 会清掉标记。
        if (g_app.minimized) {
            Sleep(10);
            continue;
        }
        // 帧首的"帧间窗口操作"：全屏切换会改窗口几何并（同步）触发 WM_SIZE，
        // 必须排在尺寸应用**之前**。放在帧首而不是命令派发处，是为了让"几何变更 →
        // swapchain 重建 → ImGui 视口"三者在同一帧内一致：否则本帧会用旧尺寸的绘制数据
        // 去填充新几何的窗口，被 DWM 拉伸成可见的闪烁（ADR-058）。
        if (g_app.request_fullscreen_toggle) {
            g_app.request_fullscreen_toggle = false;
            toggle_fullscreen();
        }
        // 应用待处理的窗口尺寸：与 ImGui 视口尺寸同帧一致，且多次 WM_SIZE 合并为一次重建。
        if (g_app.resize_pending) {
            g_app.resize_pending = false;
            g_app.gfx.resize(g_app.resize_w, g_app.resize_h);
        }
        g_app.ui.new_frame();

        // ---- 帧级异常边界（ADR-078）----
        //  1) 先记下 ImGui 各栈的基线（官方 ErrorRecoveryStoreState，专为"异常后恢复"设计）；
        //  2) draw_shell 抛异常 → 记录 + 把内部状态拉回基线；
        //  3) Debug 下再比对一次栈深度：任何漏配对都在**当帧**被点名，而不是拖成几帧后的怪现象。
        // 顺序要求：恢复必须在 ImGui::Render() **之前**完成，否则会带着脏栈进 EndFrame。
        const ig::StackDepths stack_base = ig::capture_stacks();
        ImGuiErrorRecoveryState frame_base;
        ig::store_frame_state(frame_base);

        bool frame_ok = true;
        try {
            draw_shell();
        } catch (const std::exception& e) {
            frame_ok = false;
            char buf[256] = {};
            std::snprintf(buf, sizeof buf, "frame aborted: %s", e.what() ? e.what() : "?");
            lr::log::error("ui", buf);
        } catch (...) {
            frame_ok = false;
            lr::log::error("ui", "frame aborted: non-std exception");
        }
        if (!frame_ok) ig::recover_frame_state(frame_base);

#ifndef NDEBUG
        {
            char diff[256] = {};
            ig::format_stack_diff(stack_base, diff, sizeof diff);
            if (diff[0] != '\0') lr::log::error("ui", diff);
        }
#endif

        // ImGui::Render() **必须**调用：它负责关闭本帧（合成绘制数据、结算各栈）。
        // 漏掉它，下一次 NewFrame 会命中 ImGui 的 "Forgot to call Render()" 断言。
        ImGui::Render();
        // 帧被异常截断时不提交绘制数据：窗口保留上一帧画面，胜过闪一帧半成品。
        // （ImGui 帧已经在上面正常收口，故跳过一次 Present 不会留下不完整状态。）
        if (frame_ok) g_app.gfx.render_frame();

        // 「打开文档…」：模态文件对话框放到帧与帧之间执行（见 open_file_dialog_now 注释）。
        if (g_app.request_open_dialog) {
            g_app.request_open_dialog = false;
            open_file_dialog_now();
        }
    }
quit:
    crash::set_phase("shutdown");
    g_app.renderer.reset();  // 必须先于 gfx 释放：纹理依赖 D3D11 设备
    g_app.ui.shutdown();
    g_app.gfx.shutdown();
    g_app.session.persist.reset();   // 析构即 flush；须在 log::shutdown 之前（写失败要能记日志）
    lr::log::info("app", "clean exit");
    crash::mark_clean_exit();   // 正常退出：清掉"运行中"标记，下次启动不误报
    lr::log::shutdown();
    return 0;
}
