// render.ixx — Lilith Reader 渲染调度层（Phase 3 建立，Phase 4 完善，Phase 5 增视图变换与缩略图）
//
// 职责：衔接 document 与 canvas。架构文档 §1 的依赖方向要求
//       document 与 canvas **互不 import**，由本层居中调度，为将来替换
//       MuPDF 为 PDFium 预留解耦（ADR-003）。
//
// 本层做六件事：
//   1. **线程归属**：单工作线程独占唯一的 Document；UI 线程零 fz_* 调用、
//      零阻塞（延续 ADR-006/ADR-012）。
//   2. **页状态机**：Unloaded → Loading → Loaded / Failed（架构文档 §3.2）。
//      Phase 4 补齐 Failed 语义：自动重试 ≤1 次，仍失败则定格并等用户点击重试
//      （`retry_page`）；重试/加载期间保留旧纹理，不闪白（ADR-024/031）。
//   3. **纹理上传与生命周期**：渲染线程用 ID3D11Device 直接建纹理
//      （D3D11 的 device 方法可多线程调用），产出即释放 PageBitmap；
//      被替换/逐出的纹理进**两段式退役队列**，由 UI 线程在帧首 drain_retired()
//      释放 —— 任何纹理都"活过一帧"，杜绝"删除正在被本帧 draw list 引用的纹理"
//      （架构文档 §3.3，ADR-019/033）。
//   4. **缓存**：按**字节预算**的 LRU 逐出（默认 512MiB，可设 128MiB~2GiB）。
//      逐出策略本身是纯函数，单独放在 lilithreader.page_cache 里可单测（ADR-030）。
//   5. **视图变换**（Phase 5）：旋转（0/90/180/270）与纸张方案（原色/深色/暖色）为
//      文档级视图状态；变化即让全部纹理失效并整篇重渲（变换已固化进像素）。
//   6. **缩略图通道**（Phase 5）：侧栏用的低 DPI 缩略图，独立缓存、不占页缓存预算。
//
// 纪律：
//   · 本模块接口不出现 ImGui/D3D 类型：纹理以不透明 void* 暴露，UI 侧自行转
//     ImTextureID（= ID3D11ShaderResourceView*）。UI **只读**该句柄，绝不释放。
//   · 本层不出现任何 fz_* 调用（只经 Document/PageBitmap 公开接口），
//     故不受 ADR-009 的 setjmp 纪律约束。

module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module lilithreader.render;

// 重新导出 document：本模块的公开类型（DocState/PageSlot）含 DocumentInfo/DocError，
// 让 import render 的翻译单元一并可见（避免"导出声明引用非导出模块类型"）。
export import lilithreader.document;

// 重新导出页缓存纯策略：预算常量/钳制、自动重试上限供 app 层直接使用。
export import lilithreader.page_cache;

export namespace lr {

// ---- 单页状态机 ----
enum class PageStatus : int {
    Unloaded = 0,  // 无纹理（尚未请求，或已被逐出）
    Loading,       // 渲染中（保留旧纹理，UI 继续显示旧图，ADR-024）
    Loaded,        // 纹理可用
    Failed,        // 渲染失败：自动重试已用尽。UI 显示错误占位，点击可重试（retry_page）
};

// UI 线程读取的页快照（值拷贝，不持有所有权）
struct PageSlot {
    PageStatus status = PageStatus::Unloaded;
    void*      texture = nullptr;  // ID3D11ShaderResourceView*，不透明；UI 只读
    int        pixel_w = 0;
    int        pixel_h = 0;
    float      scale = 0.0f;       // 实际使用的渲染倍率（可能因尺寸上限被下调）
    DocError   error = DocError::Ok;
};

// ---- 文档级状态 ----
enum class DocPhase : int { Idle, Opening, Ready, Failed };

struct DocState {
    DocPhase      phase = DocPhase::Idle;
    DocError      error = DocError::Ok;
    DocumentInfo  info{};
    std::string   detail_u8;   // 失败时的原始错误信息（UTF-8）
    std::uint64_t id = 0;      // 请求序号，UI 用它丢弃过期结果
    // 打开时刻的**稀疏采样内容指纹**（`lr::file_fingerprint`，ADR-062）：跨路径稳定的
    // "同一份文件"判据，UI 侧用它定位阅读记录。由工作线程在打开前算好（UI 线程不读
    // 文件内容，ADR-009）；取不到时为 0，调用方退回路径键。
    std::uint64_t file_fingerprint = 0;
};

// 一次渲染请求：把某页渲染到目标倍率（倍率 1.0 = 72dpi）
struct RenderWant {
    int   page = -1;
    float scale = 1.0f;
};

// 缓存统计（F3 调试浮层/诊断用）
struct CacheStats {
    std::size_t budget_bytes = 0;    // 当前预算
    std::size_t used_bytes = 0;      // 已驻留纹理字节数
    int         resident_pages = 0;  // 已驻留页数
    int         evictions = 0;       // 自打开文档以来累计逐出页数
};

// ---- 渲染调度器 ----
class Renderer {
public:
    // device 为 ID3D11Device*（不透明：接口不引入 d3d11.h，保持后端解耦）。
    // 设备必须比本对象活得久（约定：UI 在释放 D3D 设备之前先销毁本对象）。
    explicit Renderer(void* device) noexcept;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // 投递命令（UI 线程）。返回请求序号。
    std::uint64_t open(std::wstring path);
    std::uint64_t close();
    std::uint64_t authenticate(std::string_view utf8_password);

    // 读取快照（UI 线程）
    [[nodiscard]] DocState doc_state() const;
    [[nodiscard]] PageSlot slot(int page) const;
    // 目录（outline）快照。打开/解锁成功后由工作线程一次性加载；UI 进入阅读态时取一次即可。
    // 无目录返回空表。**不要每帧调用**（会拷贝整个目录表）。
    [[nodiscard]] std::vector<OutlineItem> outline() const;

    // 声明本帧需要的页（可见 + 预加载，按优先级排序）。渲染线程去重、
    // 排队、换入；已在足够倍率上 Loaded 的页会被跳过。
    void set_wanted(std::vector<RenderWant> wants);

    // 帧首调用：释放**上一帧之前**退役的纹理。必须在构建本帧 draw list 之前调用。
    void drain_retired();

    // 设置缓存字节预算（钳制到 [kCacheBudgetMin, kCacheBudgetMax]）。
    // 预算调小会立即触发一次逐出。默认 kCacheBudgetDefault。
    void set_cache_budget(std::size_t bytes);
    [[nodiscard]] CacheStats cache_stats() const;

    // 手动重试某页（用户点击失败占位）：重置自动重试计数并重新排队。
    // 页不可见时仅置标记，待其重新进入可见范围后生效。
    void retry_page(int page);

    // ---- 视图变换（Phase 5）----
    // 旋转（0/90/180/270，顺时针）与页面配色（纸张方案）。**任一变化即让全部已缓存纹理失效**
    // 并重新排队——因为变换结果已固化进纹理像素，必须整篇重渲。
    // 页纹理与缩略图纹理都会失效重渲。
    void set_view_transform(int rotation_deg, PageScheme scheme);

    // ---- 缩略图通道（Phase 5）----
    // 声明侧栏需要的缩略图页（低 DPI）。与页纹理**互不干扰、独立缓存**，
    // 不占页缓存字节预算（缩略图很小）。相同请求不会重复投递。
    //   pages     : 需要的页（0 基），顺序即优先级
    //   target_px : 缩略图最长边目标像素（如 150）
    void set_thumbs_wanted(std::vector<int> pages, int target_px);
    [[nodiscard]] PageSlot thumb_slot(int page) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lr
