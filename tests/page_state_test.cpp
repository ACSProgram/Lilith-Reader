// page_state_test.cpp — 单页渲染状态机的纯逻辑断言（架构加固，ADR-085）
//
// 为什么单独一个可执行文件：页状态转移过去由 Entry 的多个布尔/计数字段拼出，判定散落在
// render.cpp 各处；本用例**直接链接项目自己的 lilithreader.page_state 模块**（零 D3D/线程
// 依赖），把转移语义逐条钉死 —— 与 page_cache_test 同一思路。
//
// 覆盖：Unloaded→Loading→Loaded、失败定格与"同倍率不再重渲 / 换倍率重置额度"、
//       自动重试额度消耗、手动重试、逐出/作废/配色失效三条复位路径、PageStatus 取值稳定。
//
// 退出码：0 = 全部通过，1 = 有 FAIL。
//
// 用法： page_state_test.exe

#include <cstdio>

import lilithreader.page_state;

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

lr::PageState fresh() { return lr::PageState{}; }

// ---- 1. PageStatus 取值稳定（UI/序列化都依赖数值，不得漂移）----
void test_status_values() {
    std::printf("\n[1] PageStatus 取值\n");
    check(static_cast<int>(lr::PageStatus::Unloaded) == 0, "Unloaded = 0");
    check(static_cast<int>(lr::PageStatus::Loading) == 1, "Loading = 1");
    check(static_cast<int>(lr::PageStatus::Loaded) == 2, "Loaded = 2");
    check(static_cast<int>(lr::PageStatus::Failed) == 3, "Failed = 3");
}

// ---- 2. 初始态与基本环形：Unloaded → Loading → Loaded ----
void test_basic_cycle() {
    std::printf("\n[2] 基本状态环\n");
    lr::PageState s = fresh();
    check(s.status == lr::PageStatus::Unloaded, "初始为 Unloaded");
    check(s.auto_retries == 0 && !s.manual_retry && s.failed_scale < 0.0f && !s.stale,
          "初始无重试/失败/stale 痕迹");

    check(s.plan_render(1.0f), "Unloaded：plan_render 放行");
    check(s.status == lr::PageStatus::Loading, "plan_render 后进入 Loading");

    s.on_loaded();
    check(s.status == lr::PageStatus::Loaded, "on_loaded 后进入 Loaded");
    check(s.auto_retries == 0 && !s.manual_retry && s.failed_scale < 0.0f && !s.stale,
          "on_loaded 清空重试/失败痕迹");

    // 已 Loaded 页再次 plan_render（换缩放刷新）仍放行。
    check(s.plan_render(2.0f), "Loaded：再次 plan_render 放行（刷新）");
    check(s.status == lr::PageStatus::Loading, "刷新后回到 Loading");
}

// ---- 3. stale 语义：配色变化保留纹理、标记待重渲 ----
void test_stale() {
    std::printf("\n[3] 配色失效（stale）\n");
    lr::PageState s = fresh();
    (void)s.plan_render(1.0f);
    s.on_loaded();
    s.auto_retries = 1;             // 制造"此前失败过"的痕迹
    s.failed_scale = 9.0f;

    s.on_staled();
    check(s.status == lr::PageStatus::Loaded, "on_staled 不改状态（纹理继续显示）");
    check(s.stale, "on_staled 置 stale");
    check(s.auto_retries == 0 && s.failed_scale < 0.0f, "on_staled 清失败痕迹，给重渲新机会");

    check(s.plan_render(1.0f), "stale 页 plan_render 放行");
    check(s.status == lr::PageStatus::Loading, "重渲进入 Loading");
    check(!s.stale, "plan_render 清 stale（本次已开始重渲）");
}

// ---- 4. 自动重试额度 ----
void test_auto_retry() {
    std::printf("\n[4] 自动重试额度\n");
    lr::PageState s = fresh();
    (void)s.plan_render(1.0f);

    check(s.consume_retry(), "首次失败：还有额度（自动重试一次）");
    check(s.auto_retries == 1, "消耗后计数为 1");
    check(!s.consume_retry(), "额度用尽：不再自动重试");
    check(s.auto_retries == 1, "用尽后计数保持 1");

    // 定格失败：记录失败倍率并进入 Failed。
    s.set_failed(1.0f);
    check(s.status == lr::PageStatus::Failed, "set_failed 进入 Failed");
    check(s.failed_scale == 1.0f, "set_failed 记录失败倍率");
}

// ---- 5. 失败定格：同倍率不重渲，换倍率视为新请求 ----
void test_failed_freeze() {
    std::printf("\n[5] 失败定格语义\n");
    lr::PageState s = fresh();
    (void)s.plan_render(1.0f);
    (void)s.consume_retry();
    s.set_failed(1.0f);

    // 同倍率（容差 0.002 内）且额度已用尽 → 不再重渲，状态保持 Failed。
    check(!s.plan_render(1.0f), "同倍率：不再重渲");
    check(s.status == lr::PageStatus::Failed, "拒绝后状态仍为 Failed");
    check(!s.plan_render(1.001f), "同倍率（容差内）：仍拒绝");
    check(!s.plan_render(0.9995f), "同倍率（容差内）：仍拒绝");

    // 倍率变化 = 新请求：重置额度并放行。
    check(s.plan_render(1.5f), "换倍率：视为新请求，放行");
    check(s.status == lr::PageStatus::Loading, "新请求进入 Loading");
    check(s.auto_retries == 0, "换倍率重置自动重试额度");
    check(s.failed_scale == 1.0f, "failed_scale 保留至下次定格（plan_render 不动它）");
}

// ---- 6. 手动重试 ----
void test_manual_retry() {
    std::printf("\n[6] 手动重试\n");
    lr::PageState s = fresh();
    (void)s.plan_render(1.0f);
    (void)s.consume_retry();
    s.set_failed(1.0f);
    check(!s.plan_render(1.0f), "前置：定格后同倍率拒绝");

    s.request_retry();
    check(s.manual_retry, "request_retry 置手动标记");
    check(s.auto_retries == 0, "request_retry 重置额度");

    check(s.plan_render(1.0f), "手动重试：同倍率也放行");
    check(!s.manual_retry, "放行后清手动标记（一次性）");
    check(s.status == lr::PageStatus::Loading, "手动重试进入 Loading");
}

// ---- 7. 逐出 / 旋转作废 ----
void test_evict_and_invalidate() {
    std::printf("\n[7] 逐出与旋转作废\n");
    {
        lr::PageState s = fresh();
        (void)s.plan_render(1.0f);
        s.on_loaded();
        s.auto_retries = 1;
        s.failed_scale = 5.0f;
        s.stale = true;
        s.on_evicted();
        check(s.status == lr::PageStatus::Unloaded, "逐出回到 Unloaded");
        check(s.auto_retries == 0 && s.failed_scale < 0.0f && !s.stale, "逐出清失败/stale 痕迹");
    }
    {
        lr::PageState s = fresh();
        (void)s.plan_render(1.0f);
        s.set_failed(1.0f);
        s.on_invalidated();
        check(s.status == lr::PageStatus::Unloaded, "旋转作废回到 Unloaded");
        check(s.auto_retries == 0 && s.failed_scale < 0.0f, "旋转作废清失败痕迹");
        check(s.plan_render(1.0f), "作废后同倍率可重新渲染");
    }
}

// ---- 8. 一条完整生命周期（含失败重试一路）----
void test_full_lifecycle() {
    std::printf("\n[8] 完整生命周期\n");
    lr::PageState s = fresh();

    // 成功一路：Unloaded → Loading → Loaded
    check(s.plan_render(1.0f), "1 请求放行");
    check(s.status == lr::PageStatus::Loading, "2 进入 Loading");
    s.on_loaded();
    check(s.status == lr::PageStatus::Loaded, "3 成功进入 Loaded");

    // 配色失效一路：保留纹理 → 重渲
    s.on_staled();
    check(s.stale && s.status == lr::PageStatus::Loaded, "4 配色失效保留纹理");
    check(s.plan_render(1.0f), "5 重渲放行");
    check(!s.stale, "6 清 stale");

    // 失败一路：重试一次，仍失败则定格
    check(s.consume_retry(), "7 首次失败尚有额度");
    s.set_failed(1.0f);
    check(s.status == lr::PageStatus::Failed, "8 重试用尽后定格");
    check(!s.plan_render(1.0f), "9 定格后同倍率不再重渲");

    // 收尾：逐出
    s.on_evicted();
    check(s.status == lr::PageStatus::Unloaded, "10 逐出收尾");
    check(s.auto_retries == 0 && !s.stale && s.failed_scale < 0.0f, "11 状态无残留");
}

}  // namespace

int main() {
    std::printf("Lilith Reader 页状态机测试（ADR-085）\n");
    test_status_values();
    test_basic_cycle();
    test_stale();
    test_auto_retry();
    test_failed_freeze();
    test_manual_retry();
    test_evict_and_invalidate();
    test_full_lifecycle();
    std::printf("\n通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}