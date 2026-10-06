// imgui_raii_test.cpp — ImGui RAII 包装与"栈平衡自检"的 headless 断言（Phase 7，ADR-077/078）
//
// 为什么可以 headless：本用例只碰 ImGui 的**核心**（上下文 / 帧 / 各层栈），不碰任何后端
// （没有 Win32、没有 D3D）。NewFrame → 绘制 → Render 这条链在无后端时同样成立，只是
// Render() 产出的绘制数据没人消费。于是"栈是否平衡"这件事可以完全离线、确定性验证。
//
// 断言的核心命题（也是这次稳定性加固的立足点）：
//   1. 作用域对象无论**正常退出**还是**异常展开**，Push/Pop 与 Begin/End 都必然配对；
//   2. dismiss() 提前收口后，析构不再重复 Pop（重复 Pop 会让 ImGui 栈下溢）；
//   3. 多帧连续绘制**不累积**泄漏（这是"漏一次 Pop 会在若干帧后演变成界面错乱"的量化防线）；
//   4. 真的漏了配对时，ig::format_stack_diff 必须**当场点名**——诊断工具自身也要被测。

#define NOMINMAX
#include "imgui.h"
#include "imgui_raii.h"    // 被测对象（src/app）
#include "imgui_stacks.h"  // 栈快照 / 差异诊断（src/app）

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace ig = lr::app::ig;

namespace {

int g_pass = 0;
int g_fail = 0;

// ImGui 自己检出的错误（栈不平、类型不匹配、ID 冲突…）——把计数的错接到测试上：
// 只要 ImGui 报过一次，整个用例集就算失败。这比"崩没崩"严格得多：
// 它能抓住"栈被静默搞乱但恰好没崩"这类最难查的问题。
int g_imgui_errors = 0;

void on_imgui_error(ImGuiContext*, void*, const char* msg) {
    ++g_imgui_errors;
    std::printf("         [imgui-error] %s\n", msg ? msg : "(null)");
}

// 用例内的中间断言失败原因（只在 FAIL 时打印，避免成功路径上拼串）。
const char* g_detail = "";

void pass(const char* name) {
    ++g_pass;
    std::printf("  [PASS] %-34s\n", name);
}

void fail(const char* name, const char* detail) {
    ++g_fail;
    std::printf("  [FAIL] %-34s\n         -> %s\n", name, detail ? detail : "(no detail)");
}

// 把失衡写成人能读的一行（复用生产代码里的诊断函数，顺带验证它可用）。
bool report_if_unbalanced(const ig::StackDepths& base) {
    char diff[256] = {};
    ig::format_stack_diff(base, diff, sizeof diff);
    if (diff[0] == '\0') return true;
    g_detail = "stack drift detected";
    std::printf("         diff: %s\n", diff);
    return false;
}

// 起一帧：无后端时 DisplaySize/DeltaTime 必须自己给，否则 ImGui 会按 0 尺寸裁剪掉一切。
void begin_frame() {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1024.0f, 768.0f);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
}

// 在"一帧 + 一个宿主窗口"里执行 body，随后断言栈回到帧首基线并正常关帧。
// 宿主窗口用固定尺寸：无后端时窗口若因自动尺寸/裁剪返回 false，后面的子窗口断言就没意义了。
template <class Body>
bool in_frame(Body&& body) {
    begin_frame();
    const ig::StackDepths base = ig::capture_stacks();
    bool ok = true;
    {
        ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
        const ig::Window host("##host", nullptr, ImGuiWindowFlags_NoSavedSettings);
        ok = body();
    }
    ok = report_if_unbalanced(base) && ok;
    ImGui::Render();
    return ok;
}

// ---- 用例 1：全部 Push/Pop 包装的配对 ----
bool case_push_pop_pairs() {
    return in_frame([&] {
        {
            const ig::StyleVar  sv(ImGuiStyleVar_Alpha, 0.5f);
            const ig::StyleVar2 sv2(ImGuiStyleVar_ItemSpacing, ImVec2(1.0f, 2.0f),
                                    ImGuiStyleVar_FramePadding, ImVec2(3.0f, 4.0f));
            const ig::StyleVar3 sv3(ImGuiStyleVar_WindowRounding, 1.0f,
                                    ImGuiStyleVar_WindowBorderSize, 1.0f,
                                    ImGuiStyleVar_WindowPadding, ImVec2(3.0f, 4.0f));
            const ig::StyleColor  c1(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            const ig::StyleColor3 c3(ImGuiCol_Text, ImVec4(1, 1, 1, 1),
                                     ImGuiCol_Button, ImVec4(0, 0, 0, 1),
                                     ImGuiCol_Border, ImVec4(0.5f, 0.5f, 0.5f, 1));
            const ig::StyleColor4 c4(ImGuiCol_Text, ImVec4(1, 1, 1, 1),
                                     ImGuiCol_Button, ImVec4(0, 0, 0, 1),
                                     ImGuiCol_Border, ImVec4(0.4f, 0.4f, 0.4f, 1),
                                     ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1));
            const ig::Font       f(nullptr, 18.0f);
            const ig::Id         id(3);
            const ig::ItemWidth  iw(80.0f);
            const ig::TextWrapPos tw(60.0f);
            const ig::Disabled   dis(true);
            const ig::ClipRect   cr(ImGui::GetWindowDrawList(), ImVec2(0, 0), ImVec2(10, 10), true);
            ImGui::TextUnformatted("x");
        }
        return true;
    });
}

// ---- 用例 2：条件配对的 Begin/End（含"返回 false 就不配对"的分支）----
bool case_begin_end_conditional() {
    return in_frame([&] {
        {
            const ig::Child ch("##c1", ImVec2(120.0f, 80.0f));
            if (ch) ImGui::TextUnformatted("in child");

            if (const ig::Table t = ig::Table("##t1", 2)) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("a");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted("b");
            }

            if (const ig::TabBar tb = ig::TabBar("##tb1")) {
                // 首个页签自动选中（返回 true），其余返回 false —— 两条分支都要走一遍。
                if (const ig::TabItem ti = ig::TabItem("one")) ImGui::TextUnformatted("1");
                if (const ig::TabItem ti = ig::TabItem("two")) ImGui::TextUnformatted("2");
            }

            if (const ig::TreeNode tn = ig::TreeNode("node")) ImGui::TextUnformatted("leaf");

            if (const ig::Menu m = ig::Menu("menu")) ImGui::MenuItem("entry");

            ImGui::OpenPopup("##p1");
            if (const ig::Popup p = ig::Popup("##p1")) ImGui::TextUnformatted("popup body");
        }
        return true;
    });
}

// ---- 用例 3：异常展开时栈必须被收回（本次加固的核心保证）----
// 抛出点刻意**无条件**执行：不依赖任何 ImGui 调用返回 true，否则一旦某个 Begin* 返回
// false，用例就会"因为没抛而通过"——那是假绿。
bool case_exception_unwind() {
    return in_frame([&] {
        bool caught = false;
        try {
            const ig::Window     w("##wexc", nullptr, ImGuiWindowFlags_NoSavedSettings);
            const ig::StyleVar   sv(ImGuiStyleVar_Alpha, 0.25f);
            const ig::StyleColor sc(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
            const ig::Id         id(9);
            const ig::Disabled   dis(true);
            ImGui::TextUnformatted("before throw");
            const ig::Table     t("##texc", 1);
            const ig::Child     ch("##cexc", ImVec2(100.0f, 60.0f));
            const ig::ItemWidth iw(40.0f);
            const ig::TreeNode  tn("n");
            throw std::runtime_error("injected mid-draw");
        } catch (const std::runtime_error&) {
            caught = true;
        }
        if (!caught) {
            g_detail = "throw not caught";
            return false;
        }
        // 同帧内继续正常绘制：栈既然已经收回，后面的绘制不应受影响。
        ImGui::TextUnformatted("after throw");
        return true;
    });
}

// ---- 用例 4：dismiss() 提前收口，且析构不重复 Pop ----
bool case_dismiss_is_idempotent() {
    return in_frame([&] {
        const ig::StackDepths base = ig::capture_stacks();
        {
            const ig::StyleVar sv(ImGuiStyleVar_Alpha, 0.5f);
            sv.dismiss();
            // 提前收口后，栈应当**已经**回到基线（而不是等到作用域结束）。
            if (!ig::stacks_match(base, ig::capture_stacks())) {
                g_detail = "dismiss() did not unwind immediately";
                return false;
            }
            sv.dismiss();  // 幂等：再调一次不得产生第二次 Pop
            if (!ig::stacks_match(base, ig::capture_stacks())) {
                g_detail = "second dismiss() double-popped";
                return false;
            }
        }
        // 作用域结束：析构同样不得再 Pop 一次。
        if (!ig::stacks_match(base, ig::capture_stacks())) {
            g_detail = "destructor double-popped after dismiss()";
            return false;
        }
        return true;
    });
}

// ---- 用例 5：诊断工具必须真的能检出漏配对 ----
// 这条用例测的是"错误可追溯"本身：故意漏掉一次 Pop，ig::format_stack_diff 必须点名。
bool case_diff_detects_leak() {
    return in_frame([&] {
        const ig::StackDepths base = ig::capture_stacks();
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);            // 故意漏配对
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));  // 故意漏配对

        char diff[256] = {};
        ig::format_stack_diff(base, diff, sizeof diff);
        const bool detected = std::strstr(diff, "stylevar=+1") != nullptr &&
                              std::strstr(diff, "stylecolor=+1") != nullptr &&
                              std::strstr(diff, "IMGUI STACK LEAK") != nullptr;

        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        const bool restored = ig::stacks_match(base, ig::capture_stacks());

        if (!detected) {
            g_detail = "format_stack_diff missed the injected leak";
            std::printf("         diff: \"%s\"\n", diff);
        } else if (!restored) {
            g_detail = "manual restore did not return to baseline";
        }
        return detected && restored;
    });
}

// ---- 用例 6：连续多帧不累积泄漏 ----
bool case_no_accumulation_over_frames() {
    for (int i = 0; i < 200; ++i) {
        begin_frame();
        const ig::StackDepths base = ig::capture_stacks();
        bool ok = true;
        {
            ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
            const ig::Window host("##loop", nullptr, ImGuiWindowFlags_NoSavedSettings);
            if (host) {
                const ig::StyleVar   sv(ImGuiStyleVar_Alpha, 1.0f);
                const ig::StyleColor sc(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
                const ig::Id         id(i);
                if (const ig::Child ch = ig::Child("##lc", ImVec2(100.0f, 60.0f)))
                    ImGui::Text("%d", i);
                if (const ig::Table t = ig::Table("##lt", 1)) ImGui::TextUnformatted("cell");
            }
        }
        ok = ig::stacks_match(base, ig::capture_stacks());
        ImGui::Render();
        if (!ok) {
            char buf[64] = {};
            std::snprintf(buf, sizeof buf, "drift at frame %d", i);
            g_detail = buf;
            return false;
        }
    }
    return true;
}

// ---- 用例 7：帧首基线跨帧稳定（stacks_match 的判据是否可判定）----
//
// 这里刻意**不**断言"各层为 0"：ImGui 在 NewFrame 里就会自动开一个隐式窗口并压入默认
// 字体，帧首的 id / window / font 天然非 0。可判定的命题只有"帧尾 == 帧首"。
bool case_frame_baseline_stable() {
    begin_frame();
    const ig::StackDepths f0 = ig::capture_stacks();

    bool mid_scope_differed = true;
    {
        ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
        const ig::Window     host("##e1", nullptr, ImGuiWindowFlags_NoSavedSettings);
        const ig::StyleVar   sv(ImGuiStyleVar_Alpha, 1.0f);
        const ig::StyleColor sc(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        // 反证：作用域里确实"偏离了基线"，否则说明这个判据恒为真、等于没测。
        mid_scope_differed = !ig::stacks_match(f0, ig::capture_stacks());
    }
    const ig::StackDepths e0 = ig::capture_stacks();
    ImGui::Render();

    // 第二帧的帧首基线必须与第一帧完全一致（隐式窗口/默认字体不得逐帧累积）。
    begin_frame();
    const ig::StackDepths f1 = ig::capture_stacks();
    ImGui::Render();

    if (!mid_scope_differed) {
        g_detail = "stacks_match() did not notice the in-scope pushes (predicate is vacuous)";
        return false;
    }
    if (!ig::stacks_match(f0, e0)) {
        char diff[256] = {};
        ig::format_stack_diff(f0, diff, sizeof diff);
        std::printf("         frame-tail drift: %s\n", diff);
        g_detail = "frame tail did not return to the frame-start baseline";
        return false;
    }
    if (!ig::stacks_match(f0, f1)) {
        g_detail = "frame-start baseline drifted between frames";
        return false;
    }
    return true;
}

// ---- 用例 8：帧首快照不得污染 ImGui 的隐式回退窗口 ----
//
// 回归测试（真实踩过）：capture_stacks() 曾用 ImGui::GetCurrentWindow() 取"当前窗口"，
// 而这个公开 API 会顺手把窗口标记为 WriteAccessed=true。它在 NewFrame 之后、第一个 Begin
// 之前被调用，标的正是隐式回退窗口 Debug##Default —— EndFrame 见"已写入"就不再把它当
// "未使用"隐藏，于是真实应用里多出一个关不掉的 400×400 空 "Debug" 窗口。
// 断言：调用 capture_stacks() 之后，回退窗口必须仍是"未写入"（即仍会被正常隐藏）。
bool case_capture_stacks_does_not_touch_fallback_window() {
    begin_frame();
    ig::capture_stacks();   // 帧首快照（曾在此处污染回退窗口）
    char diff[256] = {};    // 顺带覆盖诊断路径（它内部也会再取一次栈）
    ig::format_stack_diff(ig::capture_stacks(), diff, sizeof diff);

    ImGuiContext* g = ImGui::GetCurrentContext();
    if (g == nullptr || g->CurrentWindow == nullptr) {
        g_detail = "no current window after NewFrame";
        return false;
    }
    if (!g->CurrentWindow->IsFallbackWindow) {
        g_detail = "current window after NewFrame is not the fallback window";
        return false;
    }
    if (g->CurrentWindow->WriteAccessed) {
        g_detail = "capture_stacks() marked the fallback window as written (Debug window leak)";
        return false;
    }
    ImGui::Render();
    return true;
}

}  // namespace

int main() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    // headless 配置：不给后端名、不落盘任何文件。
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1024.0f, 768.0f);
    io.DeltaTime = 1.0f / 60.0f;
    io.BackendPlatformName = "headless-test";
    io.BackendRendererName = "headless-test";
    // 让 ImGui 的"可恢复错误"不中止进程，而是走我们的回调计数 —— 于是任何一次
    // ImGui 检出的误用都会变成一条 FAIL，而不是把测试进程带走。
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigErrorRecoveryEnableDebugLog = false;
    ig::redirect_errors_to(&on_imgui_error, nullptr);
    io.Fonts->AddFontDefault();
    io.Fonts->Build();

    std::printf("imgui_raii_test — ImGui %s (headless)\n", IMGUI_VERSION);

    struct Case {
        const char* name;
        bool (*fn)();
    };
    const Case cases[] = {
        { "push/pop 全家族配对",         case_push_pop_pairs },
        { "Begin/End 条件配对",          case_begin_end_conditional },
        { "异常展开收回全部栈",           case_exception_unwind },
        { "dismiss() 提前收口且幂等",     case_dismiss_is_idempotent },
        { "栈差异诊断能检出漏配对",        case_diff_detects_leak },
        { "200 帧连续绘制不累积泄漏",      case_no_accumulation_over_frames },
        { "帧首基线跨帧稳定",            case_frame_baseline_stable },
        { "帧首快照不污染回退窗口",        case_capture_stacks_does_not_touch_fallback_window },
    };

    for (const Case& c : cases) {
        g_detail = "";
        if (c.fn()) pass(c.name);
        else        fail(c.name, g_detail);
    }

    // 最后一道：ImGui 自己检出的错误一律算失败。这条比"进程有没有崩"严格得多 ——
    // 它能抓住"栈被静默搞乱但恰好没崩"的情况。
    if (g_imgui_errors == 0) {
        pass("ImGui 未检出任何内部误用");
    } else {
        char buf[64] = {};
        std::snprintf(buf, sizeof buf, "ImGui 报错 %d 次", g_imgui_errors);
        fail("ImGui 未检出任何内部误用", buf);
    }

    ImGui::DestroyContext();

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
