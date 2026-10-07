// page_state.ixx — 单页渲染的**状态机**与状态转移（架构加固，ADR-085）
//
// 为什么单独成模块：一页的"要不要渲、失败后要不要重试、视图变换后要不要重渲"过去由
// Entry 的多个布尔/计数字段（status / stale / manual_retry / auto_retries / failed_scale）
// 拼出来 —— 这些字段理论上存在"不应出现"的组合，且判定散落在 render.cpp 各处。把状态收进
// 一个显式对象 + 一组转移函数后，规则成为**可脱离 D3D/线程单测**的纯逻辑
// （tests/page_state_test.cpp），与 page_cache 把缓存策略纯函数化同一思路（ADR-018）。
//
// 本模块**零 D3D/线程/Win32 依赖**：只依赖 lilithreader.page_cache 的重试策略
// （kMaxAutoRetries / should_render_failed）——重试规则保持单一出处。
// render 的内部组件 TextureStore 持有 PageState；发布给 UI 的像素快照 PageSlot::status
// 由 PageState 派生，不再单独维护一份。

module;

#include <cmath>

export module lilithreader.page_state;

import lilithreader.page_cache;   // kMaxAutoRetries / should_render_failed（重试策略单一出处）

export namespace lr {

// ---- 单页状态机（语义见 02-架构设计 §3.2）----
enum class PageStatus : int {
    Unloaded = 0,  // 无纹理（尚未请求，或已被逐出）
    Loading,       // 渲染中（保留旧纹理，UI 继续显示旧图，ADR-024）
    Loaded,        // 纹理可用
    Failed,        // 渲染失败：自动重试已用尽。UI 显示错误占位，点击可重试（retry_page）
};

// 一页的**渲染策略状态**：只服务"下一帧要不要渲、要不要重试"，与"发布给 UI 的像素快照"
// （PageSlot：纹理句柄与像素尺寸）分离。
//
// 转移图：
//   Unloaded → Loading → Loaded
//                     ↘ Failed（自动重试 ≤ kMaxAutoRetries 次后定格；点击重试可再入 Loading）
//   Loaded → (视图变换) → stale（保留纹理）→ Loading → Loaded
//   任意 → (逐出/旋转作废) → Unloaded
struct PageState {
    PageStatus status = PageStatus::Unloaded;
    int        auto_retries = 0;      // 本轮失败已消耗的自动重试次数
    bool       manual_retry = false;  // UI 请求重试（点击失败占位）
    float      failed_scale = -1.0f;  // 定格失败时的倍率（用于识别"新请求"）
    bool       stale = false;         // 视图变换已变、本纹理待重渲（仍可显示，不闪白）

    // 是否应为该倍率发起渲染（可渲染时把状态推进到 Loading、清掉 stale）。
    //   · 已 Failed 且非手动：**同倍率**且自动重试已用尽 → 返回 false（不随 wants 变化无限重渲）；
    //     倍率变化视为**新请求**，重置自动重试额度 —— 否则页会永久卡在失败时的旧倍率上。
    //   · 手动重试：放行并重置额度。
    [[nodiscard]] bool plan_render(float scale) noexcept {
        if (status == PageStatus::Failed && !manual_retry) {
            const bool same_scale = std::fabs(failed_scale - scale) <= 0.002f;
            if (same_scale && !should_render_failed(false, auto_retries)) return false;
            if (!same_scale) auto_retries = 0;
        }
        if (manual_retry) {
            manual_retry = false;
            auto_retries = 0;
        }
        stale = false;
        status = PageStatus::Loading;
        return true;
    }

    // 渲染成功：→ Loaded，清重试与失败痕迹。
    void on_loaded() noexcept {
        status = PageStatus::Loaded;
        auto_retries = 0;
        manual_retry = false;
        failed_scale = -1.0f;
        stale = false;
    }

    // 渲染失败一次：若仍有自动重试额度则消耗并返回 true（调用方应重试）；
    // 否则返回 false，由调用方调 set_failed 定格 —— **不在此改动像素快照**，
    // 旧纹理因此保留（ADR-024：宁可继续显示旧图，也不闪回失败占位）。
    [[nodiscard]] bool consume_retry() noexcept {
        if (auto_retries < kMaxAutoRetries) {
            ++auto_retries;
            return true;
        }
        return false;
    }

    // 定格失败：记录失败倍率（重试已尽 / TooLarge / 内部异常均走此路）。
    void set_failed(float scale) noexcept {
        status = PageStatus::Failed;
        failed_scale = scale;
    }

    // 用户点击重试：复位额度，下一轮 plan_render 必放行。
    void request_retry() noexcept {
        manual_retry = true;
        auto_retries = 0;
    }

    // 逐出：→ Unloaded，清失败痕迹与额度（像素快照由 TextureStore 退役）。
    void on_evicted() noexcept {
        status = PageStatus::Unloaded;
        auto_retries = 0;
        failed_scale = -1.0f;
        stale = false;
    }

    // 视图变换（旋转）：版面已变、旧纹理无法复用 → 全部作废重渲。
    void on_invalidated() noexcept { on_evicted(); }

    // 视图变换（配色）：版面不变，调用方保留旧纹理继续显示，这里只标记待重渲；
    // 给此前失败的页一次全新机会（清失败痕迹与额度）。
    void on_staled() noexcept {
        stale = true;
        auto_retries = 0;
        failed_scale = -1.0f;
    }
};

}  // namespace lr