// render.ixx — Lilith Reader 渲染调度层
//
// 职责：衔接 document 与 canvas。架构文档 §1 的依赖方向要求
//       document 与 canvas **互不 import**，由本层居中调度，为将来替换
//       MuPDF 为 PDFium 预留解耦（ADR-003）。
//
// 本层做六件事：
//   1. **线程归属**：单工作线程独占唯一的 Document；UI 线程零 fz_* 调用、
//      零阻塞（延续 ADR-006/ADR-012）。
//   2. **页状态机**：Unloaded → Loading → Loaded / Failed（架构文档 §3.2）。
//      Failed 语义：自动重试 ≤1 次，仍失败则定格并等用户点击重试
//      （`retry_page`）；重试/加载期间保留旧纹理，不闪白（ADR-024/031）。
//   3. **纹理上传与生命周期**：渲染线程用 ID3D11Device 直接建纹理
//      （D3D11 的 device 方法可多线程调用），产出即释放 PageBitmap；
//      被替换/逐出的纹理进**两段式退役队列**，由 UI 线程在帧首 drain_retired()
//      释放 —— 任何纹理都"活过一帧"，杜绝"删除正在被本帧 draw list 引用的纹理"
//      （架构文档 §3.3，ADR-019/033）。
//   4. **缓存**：按**字节预算**的 LRU 逐出（默认 512MiB，可设 128MiB~2GiB）。
//      逐出策略本身是纯函数，单独放在 lilithreader.page_cache 里可单测（ADR-030）。
//   5. **视图变换**：旋转（0/90/180/270）与纸张方案（原色/深色纸张/暖色）为
//      文档级视图状态；变化即让全部纹理失效并整篇重渲（变换已固化进像素）。
//   6. **缩略图通道**：侧栏用的低 DPI 缩略图，独立缓存、不占页缓存预算。
//
// 纪律：
//   · 本模块接口不出现 ImGui/D3D 类型：纹理以不透明 void* 暴露，UI 侧自行转
//     ImTextureID（= ID3D11ShaderResourceView*）。UI **只读**该句柄，绝不释放。
//   · 本层不出现任何 fz_* 调用（只经 Document/PageBitmap 公开接口），
//     故不受 ADR-009 的 setjmp 纪律约束。
//
// 内部结构（架构加固，ADR-085）：本接口**不变**；实现单元 `render.cpp` 内 `Renderer::Impl`
// 已拆为 TextureStore / DocumentWorker / PageScheduler / ContentService / SearchJob 五个组件
// （唯一互斥量与工作线程仍归 Impl）。页状态机见 lilithreader.page_state。

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

// 重新导出页状态机：PageStatus / PageState。状态本体与转移函数在 lilithreader.page_state
// （纯模块、可单测，ADR-085）；此处重导出使 `import lilithreader.render` 的翻译单元
// 仍可见 `lr::PageStatus`（PageSlot::status 用到它）。
export import lilithreader.page_state;

export namespace lr {

// UI 线程读取的页快照（值拷贝，不持有所有权）。
// status 由 render 内部组件的 PageState 派生填充（ADR-085）。
struct PageSlot {
    PageStatus status = PageStatus::Unloaded;
    void*      texture = nullptr;  // ID3D11ShaderResourceView*，不透明；UI 只读
    int        pixel_w = 0;
    int        pixel_h = 0;
    float      scale = 0.0f;       // 实际使用的渲染倍率（可能因尺寸上限被下调）
    DocError   error = DocError::Ok;
    int        full_pixel_w = 0;   // tiled 页的整页输出宽度；普通页等于 pixel_w
    int        full_pixel_h = 0;   // tiled 页的整页输出高度；普通页等于 pixel_h
    struct Tile {
        void* texture = nullptr;   // ID3D11ShaderResourceView*，UI 只读
        int x = 0;                  // 相对于整页输出左上角的像素偏移
        int y = 0;
        int w = 0;
        int h = 0;
    };
    std::vector<Tile> tiles;       // 空 = 普通单纹理页；非空 = tile 页
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

enum class ResourceTier : int { Low = 0, Balanced = 1, High = 2 };

struct ResourceProfile {
    ResourceTier tier = ResourceTier::Balanced;
    int tile_size_px = 2048;       // 超过此单边才启用 tile
    std::size_t cache_bytes = kCacheBudgetDefault;
};

[[nodiscard]] ResourceProfile resource_profile(ResourceTier tier) noexcept;

// ---- 全文搜索状态快照 ----
//
// 检索是**增量**的：工作线程每轮只扫一小批页，与渲染交替，避免"搜 1000 页文档时
// 界面整段卡死"。结果**渐进发布**（每批追加），UI 边搜边显示；发起新查询即取消旧查询。
struct SearchStatus {
    bool        active = false;     // 仍在检索
    int         scanned = 0;        // 已扫描页数（= 下一页起点）
    int         total = 0;          // 总页数（0 = 无文档）
    int         hits = 0;           // 已找到命中数
    bool        truncated = false;  // 命中数达到上限、提前停止
    std::uint64_t id = 0;           // 查询序号（UI 用它识别"这批属于哪次查询"）
    std::string needle;             // 当前关键字（UTF-8）
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

    // 资源档位同时约束 MuPDF worker 数、tile 单边和默认页缓存预算。
    // 变更在下一次文档打开或下一批渲染请求生效，不创建新的 D3D device。
    void set_resource_tier(ResourceTier tier);
    [[nodiscard]] ResourceTier resource_tier() const;
    [[nodiscard]] ResourceProfile resource_profile() const;

    // 手动重试某页（用户点击失败占位）：重置自动重试计数并重新排队。
    // 页不可见时仅置标记，待其重新进入可见范围后生效。
    void retry_page(int page);

    // ---- 视图变换 ----
    // 旋转（0/90/180/270，顺时针）与页面配色（纸张方案）。**任一变化即让全部已缓存纹理失效**
    // 并重新排队——因为变换结果已固化进纹理像素，必须整篇重渲。
    // 页纹理与缩略图纹理都会失效重渲。
    void set_view_transform(int rotation_deg, PageScheme scheme);

    // ---- 缩略图通道 ----
    // 声明侧栏需要的缩略图页（低 DPI）。与页纹理**互不干扰、独立缓存**，
    // 不占页缓存字节预算（缩略图很小）。相同请求不会重复投递。
    //   pages     : 需要的页（0 基），顺序即优先级
    //   target_px : 缩略图最长边目标像素（如 150）
    void set_thumbs_wanted(std::vector<int> pages, int target_px);
    [[nodiscard]] PageSlot thumb_slot(int page) const;

    // ---- 文本 / 图片 / 链接 ----
    //
    // 这些请求走**独立的辅助队列**，刻意不共用 open/close/authenticate 的那个命令槽：
    // 文本请求由鼠标悬停触发、频率高得多，若共用槽位，一次悬停就能把"待打开的文档"
    // 覆盖掉（用户点了打开却没反应）。辅助请求与渲染请求同优先级、互不覆盖。
    //
    // 坐标一律是**未旋转页面 pt**（见 document.ixx 坐标约定）；旋转折算由 app 层做。

    // 请求某页的可交互内容（文本布局 / 图片矩形 / 链接）。同一页已在队列中则不重复投。
    void request_page_content(int page);
    // 取走某页的**新**内容快照：有未取走的快照时写入 out 并返回 true。
    // 没有则返回 false 且不动 out —— UI 因此不必每帧拷贝整页字符表。
    [[nodiscard]] bool take_page_content(int page, PageContent& out);

    // 请求把某页 [a,b] 选区复制为文本。结果由 take_copy_text 一次性取走。
    void request_copy_text(int page, float ax, float ay, float bx, float by);
    // 取走复制文本结果；无结果返回 false。文本为空串也是有效结果（选区为空）。
    [[nodiscard]] bool take_copy_text(std::string& out);

    // 请求复制某页 (x,y) 处的**嵌入图片**。结果由 take_copy_image 取走。
    void request_copy_image(int page, float x, float y);
    // 取走复制图片结果；无结果返回 false。**返回 true 但 out 无效 = 该处没有图片**，
    // UI 据此提示"此处没有可复制的图片"（而不是静默什么都不发生）。
    [[nodiscard]] bool take_copy_image(ImageData& out);

    // ---- 全文搜索 ----
    // 发起一次全文检索（UTF-8 关键字；空串 = 取消）。会取消上一次仍在跑的检索。
    void start_search(std::string utf8_needle);
    void cancel_search();
    [[nodiscard]] SearchStatus search_status() const;
    // 把自上次调用以来**新增**的命中追加到 out 尾部（不覆盖已有内容）。
    void take_search_hits(std::vector<SearchHit>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lr
