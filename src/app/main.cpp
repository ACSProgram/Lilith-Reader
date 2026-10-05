// main.cpp — Lilith Reader 应用外壳（Phase 1 外壳 + Phase 2 文档核心接入）
// Win32 + D3D11 + ImGui：命令行打开、拖放打开、窗口状态持久化。
// Phase 2 起真正打开文档（MuPDF 文档核心），展示元信息或结构化错误。
//
// 项目纪律：不在开发中实际运行本软件做测试（编译/自动化测试除外）；
// 人工运行验证项统一记录在 docs/04-人工验证.md。

#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <d3d11.h>
#include <dxgi.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

import lilithreader.utils;
import lilithreader.document;

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

// ---------------- ImGui（RAII） ----------------
struct ImGuiRaii {
    bool win32 = false, dx11 = false, ctx = false, plot = false;

    void initialize(HWND hwnd) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ctx = true;
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        // 关掉 ImGui 自己的 imgui.ini 持久化。
        // 默认值是相对路径 "imgui.ini"，即落在**当前工作目录**；而「把文件拖到 exe 上
        // 打开」时 Explorer 会把工作目录设成被拖文件所在目录，于是封面文件夹里会凭空
        // 多出一个 imgui.ini（先启动窗口再拖入则落在 exe 目录，所以现象不一致）。
        // 本项目的窗口/布局状态一律由自己的 LilithReader.ini 管理，不需要 ImGui 的
        // 窗口状态（所有窗口都带 NoSavedSettings），故直接禁用，避免污染用户目录。
        io.IniFilename = nullptr;

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

// ---------------- 文档会话（Phase 2 临时实现） ----------------
//
// Phase 2 只做「打开 + 元信息」，引擎不含渲染。为了让 UI 线程永不阻塞
// （架构文档 §3.1 的硬约束：UI 线程零阻塞、绝不碰 fz_* 与磁盘），
// 打开动作交给一个专用工作线程串行执行，该线程独占唯一的 Document 实例。
//
// Phase 2 明确选择了「单渲染线程串行」而非「线程池 + fz_clone_context」：
// MuPDF 单线程渲染已足够流畅，且彻底规避共享 fz_document 的并发风险（ADR-006）。
//
// TODO(Phase 3/4)：这里的裸线程将由 render 调度层（任务队列 + 页状态机 +
//   纹理 LRU + 字节预算缓存）接管；届时 Document 迁入调度层，本结构删除。
class DocSession {
public:
    enum class Phase { Idle, Opening, Ready, Failed };

    struct Snapshot {
        Phase phase = Phase::Idle;
        lr::DocError error = lr::DocError::Ok;
        lr::DocumentInfo info{};
        std::string detail_u8;   // 失败时的原始错误信息（UTF-8）
        std::uint64_t id = 0;    // 请求序号，UI 用它丢弃过期结果
    };

    DocSession() : worker_([this](std::stop_token st) { run(std::move(st)); }) {}

    ~DocSession() {
        worker_.request_stop();
        cv_.notify_all();  // jthread 析构时 join；必须先唤醒再等
    }

    DocSession(const DocSession&) = delete;
    DocSession& operator=(const DocSession&) = delete;

    // 投递打开请求（UI 线程）。返回请求序号。
    std::uint64_t request_open(std::wstring path) {
        std::lock_guard lock(mtx_);
        const std::uint64_t id = ++next_id_;
        pending_ = Request{ Kind::Open, std::move(path), id };
        snap_.phase = Phase::Opening;
        snap_.error = lr::DocError::Ok;
        snap_.detail_u8.clear();
        snap_.id = id;
        cv_.notify_all();
        return id;
    }

    // 投递关闭请求（UI 线程）。返回请求序号。
    std::uint64_t request_close() {
        std::lock_guard lock(mtx_);
        const std::uint64_t id = ++next_id_;
        pending_ = Request{ Kind::Close, {}, id };
        snap_.phase = Phase::Idle;
        snap_.error = lr::DocError::Ok;
        snap_.detail_u8.clear();
        snap_.info = lr::DocumentInfo{};
        snap_.id = id;
        cv_.notify_all();
        return id;
    }

    Snapshot snapshot() const {
        std::lock_guard lock(mtx_);
        return snap_;
    }

private:
    enum class Kind { Open, Close };

    struct Request {
        Kind kind = Kind::Close;
        std::wstring path;
        std::uint64_t id = 0;
    };

    void run(std::stop_token stop) {
        for (;;) {
            Request req;
            {
                std::unique_lock lock(mtx_);
                cv_.wait(lock, stop, [this] { return pending_.has_value(); });
                if (stop.stop_requested()) return;
                req = std::move(*pending_);
                pending_.reset();
            }

            if (req.kind == Kind::Close) {
                doc_.close();  // 只有工作线程碰 Document
                publish(req.id, Phase::Idle, lr::DocError::Ok, {}, {}, "close requested");
                continue;
            }

            const lr::DocError err = doc_.open(req.path);
            if (err == lr::DocError::Ok) {
                lr::DocumentInfo info;
                const lr::DocError info_err = doc_.info(info);
                if (info_err == lr::DocError::Ok)
                    publish(req.id, Phase::Ready, lr::DocError::Ok, std::move(info), {},
                            "opened");
                else
                    publish(req.id, Phase::Failed, info_err, {}, to_utf8(doc_.last_error()),
                            "info failed");
            } else {
                publish(req.id, Phase::Failed, err, {}, to_utf8(doc_.last_error()),
                        "open failed");
            }
        }
    }

    // 只有请求没有被更新的请求顶掉时才发布结果
    void publish(std::uint64_t id, Phase phase, lr::DocError error, lr::DocumentInfo info,
                 std::string detail_u8, const char* /*why*/) {
        std::lock_guard lock(mtx_);
        if (snap_.id != id) return;  // 已被更新的请求取代，丢弃过期结果
        snap_.phase = phase;
        snap_.error = error;
        snap_.info = std::move(info);
        snap_.detail_u8 = std::move(detail_u8);
    }

    static std::string to_utf8(std::string_view sv) { return std::string(sv); }

    // ---- 成员声明顺序即析构顺序的逆序：先 join 线程，再释放 Document ----
    mutable std::mutex mtx_;
    std::condition_variable_any cv_;
    Snapshot snap_;
    std::optional<Request> pending_;
    std::uint64_t next_id_ = 0;

    lr::Document doc_;              // 仅工作线程访问
    std::jthread worker_;           // 最后声明 ⇒ 最先析构（join）
};

// ---------------- UI 侧文档状态（UI 线程独占） ----------------
struct UiDoc {
    enum class Kind { None, Rejected, Opening, Ready, Failed };
    Kind kind = Kind::None;
    std::wstring path_w;
    std::string name_u8, ext_u8;
    lr::DocError error = lr::DocError::Ok;  // Rejected / Failed 时的具体原因
    std::string detail_u8;                  // MuPDF 原始错误信息
    lr::DocumentInfo info{};
    std::uint64_t request_id = 0;
} g_doc;

DocSession g_session;
HWND g_hwnd = nullptr;
std::wstring g_ini_path;
bool g_show_debug = false;  // F3 切换调试浮层

void update_title() {
    if (!g_hwnd) return;
    std::wstring title = kWindowTitle;
    if (!g_doc.path_w.empty()) {
        title += L" — ";
        title += lr::file_name_of(g_doc.path_w);
    }
    SetWindowTextW(g_hwnd, title.c_str());
}

// 清空为"无文档"状态
void reset_doc_state() {
    g_doc.kind = UiDoc::Kind::None;
    g_doc.path_w.clear();
    g_doc.name_u8.clear();
    g_doc.ext_u8.clear();
    g_doc.error = lr::DocError::Ok;
    g_doc.detail_u8.clear();
    g_doc.info = lr::DocumentInfo{};
    g_doc.request_id = 0;
    update_title();
}

// 请求打开某文档（UI 线程）
void request_open_document(std::wstring path) {
    path = lr::to_absolute(path);
    g_doc.path_w = path;
    g_doc.name_u8 = lr::wide_to_utf8(lr::file_name_of(path));
    g_doc.ext_u8 = lr::wide_to_utf8(lr::extension_of(path));
    g_doc.error = lr::DocError::Ok;
    g_doc.detail_u8.clear();
    g_doc.info = lr::DocumentInfo{};

    // 本地即时判定：不存在 / 不在支持清单内（不必浪费一次线程往返）
    // 注意：支持清单是"策略"，MuPDF 本身能认更多格式；放开只需改 utils 的清单。
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
    g_doc.request_id = g_session.request_open(path);
    update_title();
}

// 关闭当前文档（UI 线程）
void close_document() {
    if (g_doc.kind == UiDoc::Kind::Opening) g_session.request_close();
    reset_doc_state();
}

// 把工作线程的结果同步到 UI 状态（每帧调用）
void poll_document() {
    if (g_doc.kind != UiDoc::Kind::Opening) return;

    const DocSession::Snapshot snap = g_session.snapshot();
    if (snap.id != g_doc.request_id) return;  // 已被更新的请求取代
    if (snap.phase == DocSession::Phase::Opening) return;

    switch (snap.phase) {
    case DocSession::Phase::Ready:
        g_doc.kind = UiDoc::Kind::Ready;
        g_doc.info = snap.info;
        g_doc.error = lr::DocError::Ok;
        break;
    case DocSession::Phase::Failed:
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
constexpr const char* kFormatsLine = "支持 PDF · EPUB · MOBI · FB2 · CBZ · XPS · 图片(PNG/JPG/GIF/BMP/TIFF)";
constexpr ImVec4 kColBright{ 0.92f, 0.93f, 0.95f, 1.0f };
constexpr ImVec4 kColDim{ 0.58f, 0.62f, 0.66f, 1.0f };
constexpr ImVec4 kColWarn{ 0.95f, 0.72f, 0.42f, 1.0f };

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

void draw_opening() {
    centered_text(g_doc.name_u8.c_str(), -44.0f, kColBright);
    // 简易进度指示（Phase 6 换成正式动效）
    static const char* kDots[] = { "正在打开 ．", "正在打开 ．．", "正在打开 ．．．" };
    const int frame = static_cast<int>(ImGui::GetTime() * 3.0) % 3;
    centered_text(kDots[frame], 0.0f, kColDim);
    centered_text("按 Esc 取消", 44.0f, kColDim);
}

void draw_ready() {
    const char* title = !g_doc.info.title.empty() ? g_doc.info.title.c_str()
                                                  : g_doc.name_u8.c_str();
    centered_text(title, -96.0f, kColBright);

    char line[256];
    snprintf(line, sizeof line, "%d 页 · %.0f × %.0f pt",
             g_doc.info.page_count,
             static_cast<double>(g_doc.info.page_width_pt),
             static_cast<double>(g_doc.info.page_height_pt));
    centered_text(line, -54.0f, kColDim);

    std::string facts = "已打开 · 实际格式 ";
    facts += g_doc.info.format.empty() ? g_doc.ext_u8 : g_doc.info.format;
    // 扩展名与实际不符时如实告知（内容优先策略下不拦，但要说明白）
    if (!g_doc.info.format.empty() &&
        !lr::format_matches_extension(g_doc.info.format, g_doc.ext_u8)) {
        facts += "（扩展名 ";
        facts += g_doc.ext_u8;
        facts += " 不符）";
    }
    if (g_doc.info.has_outline) facts += " · 含目录";
    if (g_doc.info.needs_password) facts += " · 已加密";
    centered_text(facts.c_str(), -18.0f, kColDim);

    centered_text("页面渲染与画布将在 Phase 3 接入", 18.0f, kColDim);
    centered_text("按 Esc 返回", 54.0f, kColDim);
}

void draw_failed() {
    centered_text(lr::describe(g_doc.error).data(), -84.0f, kColWarn);
    centered_text(g_doc.name_u8.c_str(), -42.0f, kColBright);

    if (g_doc.error == lr::DocError::Unsupported) {
        centered_text(kFormatsLine, 0.0f, kColDim);
    } else if (g_doc.error == lr::DocError::Mismatched) {
        // 唯一被拒的"内容与扩展名不符"：压缩包冒充单文档。给出可操作的建议，
        // 而不是把 MuPDF 的英文原文丢给用户（原文仍在 F3 调试浮层的 last_error 里可查）
        centered_text("实际内容是一个压缩包（zip/tar）", 0.0f, kColDim);
        centered_text("若是图片集，请把扩展名改回 .cbz；否则请先解压", 36.0f, kColDim);
    } else if (!g_doc.detail_u8.empty()) {
        // MuPDF 原始信息（英文）——上限截断，避免超长行撑破布局
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

void draw_debug_overlay() {
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    if (ImGui::Begin("##debug", nullptr, ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing)) {
        if (io.Framerate > 0.0f)
            ImGui::Text("%.1f FPS / %.2f ms", io.Framerate, 1000.0f / io.Framerate);
        else
            ImGui::TextUnformatted("-- FPS");
        ImGui::TextDisabled("%dx%d @ %.0f%%", (int)io.DisplaySize.x,
                            (int)io.DisplaySize.y, io.FontGlobalScale * 100.0f);

        // Phase 2：文档核心状态
        static const char* kKindNames[] = { "none", "rejected", "opening", "ready", "failed" };
        ImGui::Separator();
        ImGui::Text("doc: %s", kKindNames[static_cast<int>(g_doc.kind)]);
        ImGui::TextDisabled("err: %.*s", (int)lr::to_string(g_doc.error).size(),
                            lr::to_string(g_doc.error).data());
        if (g_doc.kind == UiDoc::Kind::Ready) {
            ImGui::Text("pages: %d / %.0fx%.0f pt", g_doc.info.page_count,
                        (double)g_doc.info.page_width_pt, (double)g_doc.info.page_height_pt);
            ImGui::TextDisabled("fmt: %s / ext: %s",
                                g_doc.info.format.empty() ? "?" : g_doc.info.format.c_str(),
                                g_doc.ext_u8.c_str());
        }
        if (g_doc.kind == UiDoc::Kind::Failed && !g_doc.detail_u8.empty())
            ImGui::TextDisabled("last_error: %.120s", g_doc.detail_u8.c_str());
    }
    ImGui::End();
}

void draw_shell() {
    poll_document();

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
                 ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar(3);

    switch (g_doc.kind) {
    case UiDoc::Kind::None:     draw_drop_guide(); break;
    case UiDoc::Kind::Opening:  draw_opening();    break;
    case UiDoc::Kind::Ready:    draw_ready();      break;
    case UiDoc::Kind::Failed:   draw_failed();     break;
    case UiDoc::Kind::Rejected: draw_rejected();   break;
    }
    ImGui::End();

    if (g_show_debug) draw_debug_overlay();

    // Esc：有文档 → 关闭返回引导页；无文档 → 退出
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        if (g_doc.kind != UiDoc::Kind::None)
            close_document();
        else
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
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
            std::wstring path(len + 1, L'\0');  // 含终止符
            DragQueryFileW(drop, 0, path.data(), len + 1);
            path.resize(len);
            request_open_document(std::move(path));
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

    if (!doc_path.empty()) request_open_document(std::move(doc_path));

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
        draw_shell();
        ImGui::Render();
        g_gfx.render_frame();
    }
quit:
    g_ui.shutdown();
    g_gfx.shutdown();
    return 0;
}
