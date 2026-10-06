// render_search_test.cpp — 渲染层「文本通道 + 全文检索」的端到端断言（Phase 8，ADR-069）
//
// 为什么单独一个可执行文件：检索是**跨线程**的（UI 投递 → 工作线程增量扫描 → UI 取结果），
// 纯函数单测覆盖不到它；而"搜不到东西"这类故障恰恰只可能出在接线处（页数算错、批次不推进、
// 结果没发布、被新查询误清空）。本用例把整条链路真跑一遍：
//   open → 等 Ready → start_search → 轮询 take_search_hits → 校验命中
//
// 不需要 D3D 设备：用例只走文本通道，**从不调用 set_wanted / set_thumbs_wanted**，
// 故工作线程不会去建纹理。构造时传 nullptr 是刻意的（Renderer 构造只保存指针）。
//
// 用法： render_search_test.exe <samples 目录>

#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

import lilithreader.render;

namespace {

int g_pass = 0, g_fail = 0;

void pass(const char* name, const char* why) {
    ++g_pass;
    std::printf("  [PASS] %-26s %s\n", name, why);
}

void fail(const char* name, const char* why, const char* detail) {
    ++g_fail;
    std::printf("  [FAIL] %-26s %s\n         -> %s\n", name, why, detail);
}

double seconds_since(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// 打开是异步的：轮询文档快照直到 Ready / Failed / 超时。
bool wait_ready(lr::Renderer& r, double limit_s) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const lr::DocState s = r.doc_state();
        if (s.phase == lr::DocPhase::Ready) return true;
        if (s.phase == lr::DocPhase::Failed) return false;
        if (seconds_since(t0) > limit_s) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// 跑完一次检索：轮询取结果，直到不再 active（或超时）。返回是否在时限内收敛。
bool drain_search(lr::Renderer& r, std::vector<lr::SearchHit>& hits, double limit_s) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        r.take_search_hits(hits);                 // 增量取用（可多次）
        const lr::SearchStatus st = r.search_status();
        if (!st.active) {
            r.take_search_hits(hits);             // 收尾：最后一批可能刚发布
            return true;
        }
        if (seconds_since(t0) > limit_s) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring dir;
    if (argc > 1) {
        wchar_t buf[1024] = {};
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, buf, 1024);
        buf[1023] = L'\0';
        dir = buf;
    } else {
        dir = L"samples";
    }

    std::printf("=== Lilith Reader 渲染层检索通道测试 ===\n样本目录：%s\n",
                argc > 1 ? argv[1] : "samples");

    const std::wstring pdf = dir + L"\\real.pdf";

    lr::Renderer r(nullptr);
    r.open(pdf);
    if (!wait_ready(r, 15.0)) {
        fail("real.pdf", "渲染层打开并进入 Ready", "超时或失败");
        std::printf("\n=== 结果：%d 通过 / %d 失败 ===\n", g_pass, g_fail);
        return 1;
    }
    pass("real.pdf", "渲染层打开并进入 Ready");

    // ---- 1) 页内容通道（悬停文本走的就是它）----
    {
        r.request_page_content(0);
        lr::PageContent pc;
        const auto t0 = std::chrono::steady_clock::now();
        bool got = false;
        while (seconds_since(t0) < 10.0) {
            if (r.take_page_content(0, pc)) { got = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (got && !pc.chars.empty() && !pc.lines.empty())
            pass("real.pdf", "request_page_content：拿到字符与行");
        else
            fail("real.pdf", "request_page_content：拿到字符与行", "超时或内容为空");
    }

    // ---- 2) 基本检索：跨页扫描 + 增量发布 ----
    std::vector<lr::SearchHit> hits;
    r.start_search("page");
    if (!drain_search(r, hits, 15.0)) {
        fail("real.pdf", "start_search 在时限内收敛", "超时仍 active");
    } else if (!hits.empty()) {
        pass("real.pdf", "start_search：取到命中");
    } else {
        fail("real.pdf", "start_search：取到命中", "0 条命中");
    }

    // real.pdf 共 3 页，每页正文都是 "page" → 应至少每页各一条，且页号覆盖 0/1/2。
    {
        bool seen[3] = { false, false, false };
        int  page_ok = 0;
        for (const lr::SearchHit& h : hits)
            if (h.page >= 0 && h.page < 3) { seen[h.page] = true; }
        for (bool b : seen) if (b) ++page_ok;
        char d[128];
        std::snprintf(d, sizeof d, "命中 %d 条，覆盖页 %d/3", static_cast<int>(hits.size()), page_ok);
        if (page_ok == 3) pass("real.pdf", "检索跨页：三页都命中");
        else fail("real.pdf", "检索跨页：三页都命中", d);
    }

    // 命中矩形必须落在页面范围内（200x300 pt）—— 防止坐标空间串了（旋转/缩放混入）。
    {
        bool ok = true;
        char d[160] = {};
        for (const lr::SearchHit& h : hits) {
            if (!(h.x0 >= -1.0f && h.x1 <= 201.0f && h.y0 >= -1.0f && h.y1 <= 301.0f)) {
                ok = false;
                std::snprintf(d, sizeof d, "越界命中 x[%.1f,%.1f] y[%.1f,%.1f]",
                              (double)h.x0, (double)h.x1, (double)h.y0, (double)h.y1);
                break;
            }
        }
        if (ok) pass("real.pdf", "命中矩形在未旋转页面 pt 空间内");
        else fail("real.pdf", "命中矩形在未旋转页面 pt 空间内", d);
    }

    // 结束后状态应收敛：不再 active、已扫完、命中数与取到的一致。
    {
        const lr::SearchStatus st = r.search_status();
        char d[160];
        std::snprintf(d, sizeof d, "active %d scanned %d/%d hits %d truncated %d",
                      st.active ? 1 : 0, st.scanned, st.total, st.hits, st.truncated ? 1 : 0);
        if (!st.active && st.scanned == st.total && st.hits == static_cast<int>(hits.size()))
            pass("real.pdf", "检索收敛：inactive + 扫完 + 计数一致");
        else
            fail("real.pdf", "检索收敛：inactive + 扫完 + 计数一致", d);
    }

    // ---- 3) 无匹配：返回空表且收敛 ----
    {
        std::vector<lr::SearchHit> none;
        r.start_search("zzzzzzzz");
        const bool conv = drain_search(r, none, 15.0);
        if (conv && none.empty()) pass("real.pdf", "无匹配：空表且收敛");
        else fail("real.pdf", "无匹配：空表且收敛", conv ? "出现了命中" : "超时仍 active");
    }

    // ---- 4) 换关键字：新查询必须取代旧查询（旧结果不得混入）----
    {
        std::vector<lr::SearchHit> a;
        r.start_search("page");
        // 不等它跑完就立刻换一个查不到的词：旧批次必须作废
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        r.start_search("zzzzzzzz");
        const bool conv = drain_search(r, a, 15.0);
        if (conv && a.empty()) pass("real.pdf", "换关键字：旧查询结果不混入");
        else fail("real.pdf", "换关键字：旧查询结果不混入", conv ? "混入了旧命中" : "超时仍 active");
    }

    // ---- 5) 取消：cancel_search 后立刻收敛且无新命中 ----
    {
        std::vector<lr::SearchHit> a;
        r.start_search("page");
        r.cancel_search();
        const bool conv = drain_search(r, a, 15.0);
        const lr::SearchStatus st = r.search_status();
        if (conv && !st.active) pass("real.pdf", "cancel_search：立即收敛");
        else fail("real.pdf", "cancel_search：立即收敛", "取消后仍 active");
    }

    // ---- 6) 空关键字 = 取消（不得进入"永远 active"）----
    {
        r.start_search("");
        const lr::SearchStatus st = r.search_status();
        if (!st.active) pass("real.pdf", "空关键字：不启动检索");
        else fail("real.pdf", "空关键字：不启动检索", "active 为真");
    }

    // ---- 7) stop_search：只"收手"，不清进度/结果（UI「停止」按钮走的就是它）----
    //
    // 与 cancel_search 的分工必须钉死：cancel 是"作废"（换关键字 / 换文档，进度与结果都不要了），
    // stop 是"收手"（用户不想再等，但已找到的结果还要能看、能跳）。
    // 若两者共用一条实现，UI 在「停止」后会显示"已扫描 0 / M 页"—— 那是"从未搜过"的样子。
    {
        std::vector<lr::SearchHit> a;
        r.start_search("page");
        drain_search(r, a, 15.0);
        const lr::SearchStatus before = r.search_status();

        r.stop_search();
        const lr::SearchStatus after = r.search_status();
        char d[200];
        std::snprintf(d, sizeof d,
                      "stop 前 scanned %d/%d hits %d → 后 scanned %d/%d hits %d",
                      before.scanned, before.total, before.hits,
                      after.scanned, after.total, after.hits);
        if (!after.active && after.scanned == before.scanned &&
            after.hits == before.hits && after.scanned == after.total)
            pass("real.pdf", "stop_search：保留进度与命中（不清零）");
        else
            fail("real.pdf", "stop_search：保留进度与命中（不清零）", d);

        // 在途停止：active 必须立刻为假，且过一会儿也不会自己"复活"继续发布。
        r.start_search("page");
        r.stop_search();
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        const lr::SearchStatus st = r.search_status();
        if (!st.active) pass("real.pdf", "stop_search：在途停止后保持静止");
        else fail("real.pdf", "stop_search：在途停止后保持静止", "仍 active");

        // 对照组：cancel 必须清零 —— 否则"换关键字"会残留旧查询的进度读数。
        r.cancel_search();
        const lr::SearchStatus c = r.search_status();
        char dc[128];
        std::snprintf(dc, sizeof dc, "cancel 后 scanned %d hits %d", c.scanned, c.hits);
        if (!c.active && c.scanned == 0 && c.hits == 0)
            pass("real.pdf", "cancel_search：进度清零（与 stop 语义区分）");
        else
            fail("real.pdf", "cancel_search：进度清零（与 stop 语义区分）", dc);
    }

    std::printf("\n=== 结果：%d 通过 / %d 失败 ===\n", g_pass, g_fail);
    if (g_fail > 0) {
        std::printf("有失败用例，详见上面的 [FAIL] 行。\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
