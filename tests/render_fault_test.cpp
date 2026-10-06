// render_fault_test.cpp — 渲染工作线程的「故障隔离」断言（Phase 7，ADR-078/079）
//
// 为什么单独一个可执行文件：本用例验证的命题是"**一个错误不会让调度器死掉**"，
// 也就是说它必须在**真实的线程边界**上跑 —— 单测覆盖不到 std::jthread 入口那条路径。
//
// 关键命题（全部围绕"错误不逃出线程入口"）：
//   1. 打开失败（不存在 / 损坏 / 空文件）只把**该文档**置为 Failed，工作线程继续存活；
//   2. 失败之后**仍能**正常打开下一个文档（这才是"没有 std::terminate"的证据）；
//   3. 反复 open/close 不泄漏、不死锁、不累积故障状态；
//   4. 越界页码 / 越界请求一律被夹住或忽略，不崩溃、不污染文档状态；
//   5. 压力下（狂投渲染请求 + 反复起停检索）仍能回到可用状态并正常出结果。
//
// 不需要 D3D 设备：只走文本/命令通道，从不真的建纹理（构造时传 nullptr 即可）。

#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

import lilithreader.render;

namespace {

int g_pass = 0;
int g_fail = 0;

void pass(const char* name, const char* why) {
    ++g_pass;
    std::printf("  [PASS] %-30s %s\n", name, why);
}

void fail(const char* name, const char* why, const char* detail) {
    ++g_fail;
    std::printf("  [FAIL] %-30s %s\n         -> %s\n", name, why, detail);
}

double seconds_since(const std::chrono::steady_clock::time_point& t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// 等文档收敛到某个终态（Ready / Failed）。超时返回 DocPhase::Idle 以区别于"确实失败"。
lr::DocPhase wait_settled(lr::Renderer& r, double limit_s) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const lr::DocPhase ph = r.doc_state().phase;
        if (ph == lr::DocPhase::Ready || ph == lr::DocPhase::Failed) return ph;
        if (seconds_since(t0) > limit_s) return lr::DocPhase::Idle;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

bool wait_ready(lr::Renderer& r, double limit_s) {
    return wait_settled(r, limit_s) == lr::DocPhase::Ready;
}

// 把文档状态写成一行，失败时打出来 —— 故障隔离测试自己也要好定位。
void describe_state(lr::Renderer& r, char* out, std::size_t cap) {
    const lr::DocState st = r.doc_state();
    const char* ph = "?";
    switch (st.phase) {
    case lr::DocPhase::Idle:    ph = "Idle"; break;
    case lr::DocPhase::Opening: ph = "Opening"; break;
    case lr::DocPhase::Ready:   ph = "Ready"; break;
    case lr::DocPhase::Failed:  ph = "Failed"; break;
    }
    std::snprintf(out, cap, "phase=%s err=%d pages=%d detail=\"%.80s\"", ph,
                  static_cast<int>(st.error), static_cast<int>(st.info.page_count),
                  st.detail_u8.c_str());
}

// 一轮"打开坏文件 → 期望 Failed"的通用检查。
// 返回 true 表示"确实失败了，且失败信息非空"（失败信息是用户能看懂错误的依据）。
bool expect_open_failure(lr::Renderer& r, const std::wstring& path, char* detail, std::size_t cap) {
    r.open(path);
    const lr::DocPhase ph = wait_settled(r, 20.0);
    const lr::DocState st = r.doc_state();
    if (ph == lr::DocPhase::Idle) {
        std::snprintf(detail, cap, "超时未收敛");
        return false;
    }
    if (ph != lr::DocPhase::Failed) {
        std::snprintf(detail, cap, "期望 Failed，实为 Ready");
        return false;
    }
    if (st.detail_u8.empty()) {
        std::snprintf(detail, cap, "Failed 但 detail 为空（错误不可追溯）");
        return false;
    }
    return true;
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

    std::printf("=== Lilith Reader 渲染层故障隔离测试 ===\n样本目录：%s\n\n", argc > 1 ? argv[1] : "samples");

    const std::wstring good = dir + L"\\real.pdf";
    lr::Renderer r(nullptr);

    // ---- 1) 打开失败的各种形态：都必须是"文档级失败"，而不是"进程级事故" ----
    {
        struct Bad {
            const char*  name;
            const wchar_t* file;  // nullptr = 不存在的路径
        };
        const Bad bads[] = {
            { "不存在的路径",   nullptr },
            { "截断的 PDF",     L"\\truncated.pdf" },
            { "随机字节",       L"\\random.pdf" },
            { "空 PDF",         L"\\empty.pdf" },
            { "坏 zip 容器",    L"\\empty_zip.epub" },
        };
        bool all_failed = true;
        for (const Bad& b : bads) {
            const std::wstring path = b.file ? (dir + b.file) : (dir + L"\\__no_such_file__.pdf");
            char detail[192] = {};
            const bool ok = expect_open_failure(r, path, detail, sizeof detail);
            if (!ok) {
                all_failed = false;
                char msg[256] = {};
                std::snprintf(msg, sizeof msg, "%s：%s", b.name, detail);
                fail("坏文件被隔离为文档级失败", msg, "见左");
                // 只要有一种形态出问题就继续测下一种，避免因一个失败整体中断。
            }
        }
        if (all_failed) pass("坏文件被隔离为文档级失败", "5 种形态均 Failed 且带错误详情");
    }

    // ---- 2) 失败之后工作线程必须还活着（这是"没有 std::terminate"的直接证据）----
    {
        r.open(good);
        if (wait_ready(r, 20.0)) {
            const lr::DocState st = r.doc_state();
            char d[128] = {};
            std::snprintf(d, sizeof d, "%d 页", static_cast<int>(st.info.page_count));
            pass("失败后再开正常文档", d);
        } else {
            char d[256] = {};
            describe_state(r, d, sizeof d);
            fail("失败后再开正常文档", "预期 Ready", d);
        }
    }

    // ---- 3) 越界访问：一律夹住或忽略，不崩、不污染状态 ----
    {
        bool ok = true;
        char detail[192] = {};

        // 负页号与超大页号：slot() 是可被 UI 任意调用的读接口，必须自守卫。
        const lr::PageSlot s_neg = r.slot(-1);
        const lr::PageSlot s_huge = r.slot(1 << 28);
        if (s_neg.status != lr::PageStatus::Unloaded || s_neg.texture != nullptr) {
            ok = false;
            std::snprintf(detail, sizeof detail, "slot(-1) 未安全退化");
        }
        if (ok && (s_huge.status != lr::PageStatus::Unloaded || s_huge.texture != nullptr)) {
            ok = false;
            std::snprintf(detail, sizeof detail, "slot(超大) 未安全退化");
        }
        // 缩略图通道同样。
        if (ok && r.thumb_slot(-1).texture != nullptr) {
            ok = false;
            std::snprintf(detail, sizeof detail, "thumb_slot(-1) 返回了纹理");
        }
        if (ok) {
            // 重试不存在的页：不得让工作线程出问题。
            r.retry_page(-5);
            r.retry_page(1 << 28);
            // 请求列表里混入越界项：应被忽略而不是照单全收。
            r.set_wanted({ { -1, 1.0f }, { 1 << 28, 1.0f }, { 0, 1.0f } });
            r.set_thumbs_wanted({ -1, 1 << 28, 0 }, 150);
            r.request_page_content(-1);
            r.request_copy_text(1 << 28, 0.0f, 0.0f, 1.0f, 1.0f);
            r.request_copy_image(-1, 0.0f, 0.0f);
        }
        // 关键：文档状态必须**不受影响**。
        const lr::DocPhase ph = wait_settled(r, 5.0);
        if (ok && ph != lr::DocPhase::Ready) {
            ok = false;
            std::snprintf(detail, sizeof detail, "越界访问后文档状态被污染（phase=%d）",
                          static_cast<int>(ph));
        }
        if (ok) pass("越界页码/请求被夹住", "slot/retry/wants 全部安全退化，状态仍 Ready");
        else    fail("越界页码/请求被夹住", "不得崩溃或污染状态", detail);
    }

    // ---- 4) 反复 open/close：不泄漏、不死锁、不累积故障 ----
    {
        bool ok = true;
        char detail[192] = {};
        for (int i = 0; i < 25 && ok; ++i) {
            // 好坏交替：既压 open 失败路径，也压成功路径的清理。
            const std::wstring path = (i % 3 == 0) ? (dir + L"\\truncated.pdf")
                                                   : (i % 3 == 1 ? (dir + L"\\real.pdf")
                                                                 : (dir + L"\\with_image.pdf"));
            r.open(path);
            const lr::DocPhase ph = wait_settled(r, 20.0);
            if (ph == lr::DocPhase::Idle) {
                ok = false;
                std::snprintf(detail, sizeof detail, "第 %d 轮未收敛（可能死锁）", i);
                break;
            }
            if (i % 3 == 0 && ph != lr::DocPhase::Failed) {
                ok = false;
                std::snprintf(detail, sizeof detail, "第 %d 轮坏文件未失败", i);
                break;
            }
            if (i % 3 != 0 && ph != lr::DocPhase::Ready) {
                ok = false;
                std::snprintf(detail, sizeof detail, "第 %d 轮好文件未就绪", i);
                break;
            }
            r.close();
        }
        // 收尾：再开一次正常文档，确认线程仍然健康。
        if (ok) {
            r.open(good);
            if (!wait_ready(r, 20.0)) {
                ok = false;
                std::snprintf(detail, sizeof detail, "25 轮之后无法再打开正常文档");
            }
        }
        if (ok) pass("25 轮 open/close 后仍健康", "无死锁、无状态累积");
        else    fail("25 轮 open/close 后仍健康", "反复开关不得拖死调度器", detail);
    }

    // ---- 5) 压力：狂投渲染请求 + 反复起停检索，最后必须回到可用状态 ----
    {
        bool ok = true;
        char detail[192] = {};
        const int total = static_cast<int>(r.doc_state().info.page_count);
        if (total <= 0) {
            ok = false;
            std::snprintf(detail, sizeof detail, "文档页数异常（%d）", total);
        }
        if (ok) {
            // 模拟"用户疯狂滚动 + 不停改缩放"：大量请求投进去，其中越界项必须被忽略。
            for (int i = 0; i < 400; ++i) {
                std::vector<lr::RenderWant> wants;
                wants.push_back({ i % total, 1.0f + static_cast<float>(i % 5) * 0.25f });
                wants.push_back({ (i + 1) % total, 1.0f });
                wants.push_back({ -1, 1.0f });          // 越界项
                wants.push_back({ 1 << 28, 1.0f });     // 越界项
                r.set_wanted(std::move(wants));
                if (i % 7 == 0) r.set_thumbs_wanted({ 0, 1, -1, 1 << 28 }, 120 + (i % 40));
                // 检索反复起停：覆盖"取消在途查询 → 立刻发起新查询"这条最容易出竞态的路径。
                if (i % 11 == 0) {
                    r.start_search("page");
                    r.cancel_search();
                }
            }
            // 排空退役队列（UI 线程每帧会做的事），确认压力后资源回收路径仍可重入。
            for (int i = 0; i < 20; ++i) {
                r.drain_retired();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            // 最终仍应能完成一次正常检索并拿到命中。
            std::vector<lr::SearchHit> hits;
            r.start_search("page");
            const auto t0 = std::chrono::steady_clock::now();
            for (;;) {
                r.take_search_hits(hits);
                if (!r.search_status().active) break;
                if (seconds_since(t0) > 20.0) {
                    ok = false;
                    std::snprintf(detail, sizeof detail, "压力后检索未收敛");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            r.take_search_hits(hits);
            if (ok && hits.empty()) {
                ok = false;
                std::snprintf(detail, sizeof detail, "压力后检索 0 命中");
            }
            if (ok && r.doc_state().phase != lr::DocPhase::Ready) {
                ok = false;
                std::snprintf(detail, sizeof detail, "压力后文档状态异常");
            }
        }
        if (ok) {
            char d[96] = {};
            std::snprintf(d, sizeof d, "400 轮请求 + 检索起停后可正常收敛");
            pass("压力后仍可正常收敛", d);
        } else {
            fail("压力后仍可正常收敛", "压力不得让调度器失效", detail);
        }
    }

    // ---- 6) 规模与幅面压力（Phase 7 压力测试项，ADR-079）----
    // 1000+ 页：页表规模、逐页尺寸探测与"整本书检索"要能收敛；
    // A0 幅面（2384×3370pt）：超过 tile_size_px，走 tile 渲染路径 —— 本用例没有 D3D 设备，
    // 建纹理必然失败，恰好压到"整页分块渲染 → 逐 tile 失败 → 自动重试 → 清理"这条
    // 最容易泄漏/卡死的路径；判定标准是**调度器不死、之后仍能正常打开文档**。
    {
        bool ok = true;
        char detail[192] = {};

        r.open(dir + L"\\stress_1200p.pdf");
        if (!wait_ready(r, 30.0)) {
            ok = false;
            std::snprintf(detail, sizeof detail, "1200 页文档未能进入 Ready");
        }
        int npages = 0;
        if (ok) {
            npages = static_cast<int>(r.doc_state().info.page_count);
            if (npages < 1000) {
                ok = false;
                std::snprintf(detail, sizeof detail, "页数异常：%d", npages);
            }
        }
        if (ok) {
            // 快速缩放抖动：同几页反复以不同倍率请求，压"重渲染 + 缓存逐出"路径。
            for (int i = 0; i < 60; ++i) {
                const float scale = 0.5f + static_cast<float>(i % 10) * 0.3f;
                r.set_wanted({ { 0, scale }, { npages / 2, scale }, { npages - 1, scale } });
            }
            r.start_search("page");
            std::vector<lr::SearchHit> hits;
            const auto t0 = std::chrono::steady_clock::now();
            for (;;) {
                r.take_search_hits(hits);
                if (!r.search_status().active) break;
                if (seconds_since(t0) > 90.0) {
                    ok = false;
                    std::snprintf(detail, sizeof detail, "1200 页检索未收敛");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            r.take_search_hits(hits);
            if (ok && hits.empty()) {
                ok = false;
                std::snprintf(detail, sizeof detail, "1200 页检索 0 命中");
            }
        }
        if (ok) pass("1200 页文档可打开、可检索", "页表/检索/缩放抖动均收敛");
        else    fail("1200 页文档可打开、可检索", "规模压力", detail);

        // A0 幅面。
        bool a0_ok = true;
        char a0_detail[160] = {};
        r.open(dir + L"\\poster_a0.pdf");
        if (!wait_ready(r, 20.0)) {
            a0_ok = false;
            std::snprintf(a0_detail, sizeof a0_detail, "A0 页未能进入 Ready");
        }
        if (a0_ok) {
            const lr::DocState st = r.doc_state();
            if (st.info.page_width_pt < 2300.0f || st.info.page_height_pt < 3300.0f) {
                a0_ok = false;
                std::snprintf(a0_detail, sizeof a0_detail, "幅面读数异常：%.0f x %.0f pt",
                              st.info.page_width_pt, st.info.page_height_pt);
            } else {
                r.set_wanted({ { 0, 1.0f } });
                // 等这一页落到终态（无设备 → Failed；有设备 → Loaded），不得停在 Loading。
                const auto t0 = std::chrono::steady_clock::now();
                for (;;) {
                    const lr::PageSlot s = r.slot(0);
                    if (s.status == lr::PageStatus::Loaded || s.status == lr::PageStatus::Failed) break;
                    if (seconds_since(t0) > 30.0) {
                        a0_ok = false;
                        std::snprintf(a0_detail, sizeof a0_detail, "超大页渲染未收敛（卡在 Loading）");
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                r.drain_retired();
            }
        }
        // 收尾：超大页折腾完之后，调度器必须还能正常工作。
        bool alive = wait_ready(r, 20.0) && r.doc_state().phase == lr::DocPhase::Ready;
        if (a0_ok && alive) pass("A0 超大页渲染不拖垮调度器", "tile 路径收敛，之后仍可正常打开");
        else    fail("A0 超大页渲染不拖垮调度器", "幅面压力",
                     a0_ok ? "超大页之后文档状态异常" : a0_detail);
    }

    std::printf("\n=== 结果：%d 通过 / %d 失败 ===\n", g_pass, g_fail);
    if (g_fail > 0) {
        std::printf("有失败用例，详见上面的 [FAIL] 行。\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
