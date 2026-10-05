// page_cache_test.cpp — Lilith Reader 页缓存纯策略自动化测试（Phase 4 起常备）
//
// 做法：**直接链接项目自己的 lilithreader.page_cache 模块**，对"选谁逐出""失败是否重试"
// 这类可程序判定的规则逐一断言。本模块零依赖（不碰 D3D/线程/MuPDF），故这里是
// 真正可脱离硬件与运行环境的证据：每条规则都用独立算出的期望值钉死。
//
// 覆盖：预算钳制、自动重试上限、LRU 逐出顺序、pinned 保护、同序号按下标定序、
//       恰好达标即停（不超额逐出）、未驻留页跳过、边界（used == budget）。
//
// 退出码：0 = 全部通过，1 = 有 FAIL。
//
// 用法： page_cache_test.exe

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

import lilithreader.page_cache;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const char* name) {
    if (cond) {
        ++g_pass;
        std::printf("  [PASS] %s\n", name);
    } else {
        ++g_fail;
        std::printf("  [FAIL] %s\n", name);
    }
}

constexpr std::size_t MB = 1024ull * 1024ull;

lr::CachePageView page(std::size_t bytes, std::uint64_t last_use, bool pinned = false) {
    lr::CachePageView v;
    v.bytes = bytes;
    v.last_use = last_use;
    v.pinned = pinned;
    return v;
}

bool eq(const std::vector<int>& got, const std::vector<int>& want) {
    return got == want;
}

std::size_t sum_bytes(const std::vector<lr::CachePageView>& pages,
                      const std::vector<int>& idx) {
    std::size_t s = 0;
    for (int i : idx) s += pages[static_cast<std::size_t>(i)].bytes;
    return s;
}

// ---- 1. 预算钳制 ----
void test_clamp() {
    std::printf("\n[1] 预算钳制\n");
    check(lr::clamp_cache_budget(0) == lr::kCacheBudgetMin, "0 → 下限");
    check(lr::clamp_cache_budget(1) == lr::kCacheBudgetMin, "1 字节 → 下限");
    check(lr::clamp_cache_budget(lr::kCacheBudgetMin - 1) == lr::kCacheBudgetMin,
          "下限-1 → 下限");
    check(lr::clamp_cache_budget(lr::kCacheBudgetMin) == lr::kCacheBudgetMin,
          "下限 → 下限（边界不越界）");
    check(lr::clamp_cache_budget(300 * MB) == 300 * MB, "区间内原样返回");
    check(lr::clamp_cache_budget(lr::kCacheBudgetDefault) == lr::kCacheBudgetDefault,
          "默认值在区间内");
    check(lr::clamp_cache_budget(lr::kCacheBudgetMax) == lr::kCacheBudgetMax,
          "上限 → 上限（边界不越界）");
    check(lr::clamp_cache_budget(lr::kCacheBudgetMax + 1) == lr::kCacheBudgetMax,
          "上限+1 → 上限");
    check(lr::clamp_cache_budget(static_cast<std::size_t>(-1)) == lr::kCacheBudgetMax,
          "SIZE_MAX → 上限");
    check(lr::kCacheBudgetMin < lr::kCacheBudgetDefault &&
          lr::kCacheBudgetDefault < lr::kCacheBudgetMax,
          "默认值严格落在上下限之间");
}

// ---- 2. 失败重试策略 ----
void test_retry_policy() {
    std::printf("\n[2] 失败重试策略\n");
    check(lr::kMaxAutoRetries == 1, "自动重试上限为 1");
    check(lr::should_render_failed(true, 0), "手动重试（已用 0）→ 应重渲");
    check(lr::should_render_failed(true, 1), "手动重试（已用 1）→ 仍应重渲");
    check(lr::should_render_failed(true, 99), "手动重试（已用 99）→ 仍应重渲");
    check(lr::should_render_failed(false, 0), "自动：未用过 → 允许再试一次");
    check(!lr::should_render_failed(false, 1), "自动：已用 1 次 → 定格，不再自动重试");
    check(!lr::should_render_failed(false, 2), "自动：超过上限 → 不再自动重试");
}

// ---- 3. LRU 逐出 ----
void test_eviction_basic() {
    std::printf("\n[3] LRU 逐出基础\n");
    {
        std::vector<lr::CachePageView> p = { page(10 * MB, 1), page(10 * MB, 2),
                                             page(10 * MB, 3), page(10 * MB, 4) };
        auto v = lr::select_evictions(p, 40 * MB, 25 * MB);
        check(eq(v, { 0, 1 }), "40MB/25MB：逐出 2 个最久未用（下标 0,1）");
        check(sum_bytes(p, v) >= 40 * MB - 25 * MB, "逐出字节数足以降到预算内");
    }
    {
        std::vector<lr::CachePageView> p = { page(10 * MB, 1), page(10 * MB, 2),
                                             page(10 * MB, 3), page(10 * MB, 4) };
        auto v = lr::select_evictions(p, 40 * MB, 35 * MB);
        check(eq(v, { 0 }), "40MB/35MB：只需逐出 1 个（不超额逐出）");
    }
    {
        std::vector<lr::CachePageView> p = { page(10 * MB, 1), page(10 * MB, 2) };
        check(lr::select_evictions(p, 20 * MB, 20 * MB).empty(), "used == budget → 不逐出");
        check(lr::select_evictions(p, 20 * MB, 100 * MB).empty(), "used < budget → 不逐出");
    }
    {
        std::vector<lr::CachePageView> p = { page(10 * MB, 1), page(10 * MB, 2) };
        auto v = lr::select_evictions(p, 20 * MB, 20 * MB - 1);
        check(eq(v, { 0 }), "used = budget+1 → 逐出恰好 1 个");
    }
    check(lr::select_evictions({}, 100 * MB, 1 * MB).empty(), "空缓存 → 无逐出");
}

// ---- 4. pinned 保护 ----
void test_pinned() {
    std::printf("\n[4] pinned（可见/预加载）保护\n");
    {
        // p0 最久未用但被 pinned → 跳过它，逐出 p1、p2
        std::vector<lr::CachePageView> p = { page(10 * MB, 1, true), page(10 * MB, 2),
                                             page(10 * MB, 3), page(10 * MB, 4) };
        auto v = lr::select_evictions(p, 40 * MB, 25 * MB);
        check(eq(v, { 1, 2 }), "pinned 页即使最久未用也不逐出");
    }
    {
        // 全部 pinned 且超预算 → 无逐出（有意：宁可短暂超预算也不抽走在显示的页）
        std::vector<lr::CachePageView> p = { page(30 * MB, 1, true), page(30 * MB, 2, true) };
        check(lr::select_evictions(p, 60 * MB, 10 * MB).empty(),
              "全部 pinned 且超预算 → 不逐出（不抽走正在显示的页）");
    }
    {
        // pinned 的字节计入总量，但不可逐出 → 只能逐出未 pinned 的
        std::vector<lr::CachePageView> p = { page(100 * MB, 1, true), page(10 * MB, 2),
                                             page(10 * MB, 3) };
        auto v = lr::select_evictions(p, 120 * MB, 110 * MB);
        check(eq(v, { 1 }), "pinned 占额后，逐出刚好够量（120→110 只需 1 页）");
    }
}

// ---- 5. 定序与跳过 ----
void test_ordering_and_skip() {
    std::printf("\n[5] 定序、跳过未驻留页\n");
    {
        // 相同 last_use → 按下标升序，保证确定性
        std::vector<lr::CachePageView> p = { page(10 * MB, 7), page(10 * MB, 7),
                                             page(10 * MB, 7) };
        auto v = lr::select_evictions(p, 30 * MB, 15 * MB);
        check(eq(v, { 0, 1 }), "last_use 相同 → 按下标升序逐出");
    }
    {
        // 未驻留页（bytes == 0）即使最久未用也不进入逐出列表
        std::vector<lr::CachePageView> p = { page(0, 1), page(10 * MB, 2), page(10 * MB, 3) };
        auto v = lr::select_evictions(p, 20 * MB, 10 * MB);
        check(eq(v, { 1 }), "未驻留页（bytes=0）被跳过");
    }
    {
        // 确定性：同输入两次结果一致
        std::vector<lr::CachePageView> p = { page(5 * MB, 3), page(5 * MB, 1),
                                             page(5 * MB, 2) };
        auto a = lr::select_evictions(p, 15 * MB, 8 * MB);
        auto b = lr::select_evictions(p, 15 * MB, 8 * MB);
        check(eq(a, b) && eq(a, { 1, 2 }), "同输入 → 同输出（确定性），按 last_use 升序");
    }
    {
        // 混合大小：逐出量按字节累计，不是按页数
        std::vector<lr::CachePageView> p = { page(40 * MB, 1), page(5 * MB, 2),
                                             page(5 * MB, 3) };
        auto v = lr::select_evictions(p, 50 * MB, 15 * MB);
        check(eq(v, { 0 }), "逐出 1 个大页即可达标（按字节累计）");
        check(sum_bytes(p, v) >= 50 * MB - 15 * MB, "释放字节数达标");
    }
}

}  // namespace

int main() {
    std::printf("Lilith Reader 页缓存纯策略测试\n");
    std::printf("预算：min=%zuMB default=%zuMB max=%zuMB\n",
                lr::kCacheBudgetMin / MB, lr::kCacheBudgetDefault / MB,
                lr::kCacheBudgetMax / MB);

    test_clamp();
    test_retry_policy();
    test_eviction_basic();
    test_pinned();
    test_ordering_and_skip();

    std::printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
