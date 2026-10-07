// page_cache.ixx — 页纹理缓存的**纯策略**
//
// 为什么单独成模块：缓存逐出与失败重试都是**可程序判定**的规则。把"选谁逐出、
// 失败后是否再试"从"怎么建纹理、怎么开线程"里剥出来，就能脱离 D3D11 与线程单测
// （tests/page_cache_test.cpp）——与 canvas 把布局纯函数化同一思路（ADR-018）。
//
// 本模块**零依赖**：不 import document / render，不碰 D3D / ImGui / 线程 / Win32。
// render.cpp 是唯一使用者：把内部缓存状态映射成 CachePageView[] 交给 select_evictions。

module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

export module lilithreader.page_cache;

export namespace lr {

// ---- 缓存字节预算 ----
//
// 为什么按**字节**而不是页数：单页纹理大小随渲染倍率变化极大 ——
// A4 在 zoom 1.0 约 2MB，在 zoom 4.0 约 32MB，超大页面（A0 海报）可达数百 MB。
// 按页数计预算会在高倍率下 OOM、在低倍率下浪费显存（ADR-030）。
inline constexpr std::size_t kCacheBudgetDefault = 512ull * 1024 * 1024;   // 512 MiB
inline constexpr std::size_t kCacheBudgetMin     = 128ull * 1024 * 1024;   // 128 MiB
inline constexpr std::size_t kCacheBudgetMax     = 2048ull * 1024 * 1024;  // 2 GiB

// 把任意输入钳制到 [kCacheBudgetMin, kCacheBudgetMax]。
// 设置来源（ini/将来的设置面板）可能给 0、负数换算值或天文数字，统一在此收口。
[[nodiscard]] constexpr std::size_t clamp_cache_budget(std::size_t bytes) noexcept {
    return bytes < kCacheBudgetMin ? kCacheBudgetMin
         : (bytes > kCacheBudgetMax ? kCacheBudgetMax : bytes);
}

// ---- 失败重试策略 ----
//
// "最多自动重试 1 次"：初次渲染失败后再自动试一次；仍失败则**定格为 Failed**，
// 显示错误占位、等待用户点击重试（ADR-031）。之所以要限额：失败多为确定性原因
// （文件损坏、超大页面被钳制后仍分配失败），无限重试只会让渲染线程空转。
inline constexpr int kMaxAutoRetries = 1;

// 一个当前为 Failed 的页是否还应（再次）渲染：
//   manual = true（用户点击重试）→ 总是重渲，并由调用方重置自动重试计数；
//   否则仅当已用自动重试次数 < kMaxAutoRetries。
[[nodiscard]] constexpr bool should_render_failed(bool manual, int auto_retries_used) noexcept {
    return manual || auto_retries_used < kMaxAutoRetries;
}

// ---- 逐出选择 ----

// 缓存中一页的纯数据视图（与 render 的内部结构解耦，便于单测）。
struct CachePageView {
    std::size_t   bytes = 0;       // 纹理字节数；0 = 未驻留（无纹理）
    std::uint64_t last_use = 0;    // 最近使用序号（越大越新）
    bool          pinned = false;  // 本帧被需要（可见/预加载）→ 不可逐出
};

// 选择应逐出的页下标，使驻留总量降到预算内。规则：
//   1. 只考虑 bytes > 0 且 !pinned 的页；
//   2. 按 last_use 升序（最久未用先出）；last_use 相同时按下标升序，保证**确定性**；
//   3. 一旦"已释放字节"足以让总量降到预算内即停止，不超额逐出；
//   4. 返回逐出顺序。
//
// 不变量：pinned 页**永不**逐出。因此当"可见/预加载页自身的字节数"就超过预算时，
// 返回值会少于需要、总量仍高于预算 —— 这是**有意**的：宁可短暂超预算，也不能把
// 正在显示的页抽走（抽走会闪回占位框）。预算的真正作用是给"窗口外的历史页"设上限。
[[nodiscard]] std::vector<int> select_evictions(const std::vector<CachePageView>& pages,
                                                std::size_t used_bytes,
                                                std::size_t budget);

// 定义（非 inline：由本接口单元发射，测试链 page_cache.ixx.obj 即可）
std::vector<int> select_evictions(const std::vector<CachePageView>& pages,
                                  std::size_t used_bytes,
                                  std::size_t budget) {
    std::vector<int> victims;
    if (used_bytes <= budget || pages.empty()) return victims;

    // 候选 = 可逐出页，按 (last_use, 下标) 升序
    std::vector<int> candidates;
    candidates.reserve(pages.size());
    for (int i = 0; i < static_cast<int>(pages.size()); ++i)
        if (pages[static_cast<std::size_t>(i)].bytes > 0 &&
            !pages[static_cast<std::size_t>(i)].pinned)
            candidates.push_back(i);

    std::sort(candidates.begin(), candidates.end(), [&pages](int a, int b) {
        const CachePageView& pa = pages[static_cast<std::size_t>(a)];
        const CachePageView& pb = pages[static_cast<std::size_t>(b)];
        if (pa.last_use != pb.last_use) return pa.last_use < pb.last_use;
        return a < b;
    });

    std::size_t freed = 0;
    for (int i : candidates) {
        const std::size_t remaining = used_bytes > freed ? used_bytes - freed : 0;
        if (remaining <= budget) break;
        victims.push_back(i);
        freed += pages[static_cast<std::size_t>(i)].bytes;
    }
    return victims;
}

}  // namespace lr
