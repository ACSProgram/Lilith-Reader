// document.ixx — Lilith Reader 文档核心模块（Phase 2）
//
// 职责：MuPDF 的唯一入口。对外只暴露与 MuPDF 无关的类型，
//       fz_context / fz_document / fz_pixmap 全部藏在实现单元的 PIMPL 里，
//       为将来替换为 PDFium 预留解耦（ADR-003）。
//
// 铁律（本模块边界）：
//   1. 任何 fz_* 调用都必须被 fz_try/fz_catch 包住，错误以 DocError 出边界，
//      绝不让 MuPDF 的 longjmp 逃出模块。
//   2. 本模块的对象**不是线程安全的**（fz_context 以 nullptr locks 创建，
//      即单线程上下文）。约定：一个 Document 只属于一个线程，
//      由上层调度层负责串行化（ADR-006）。
//   3. 渲染结果是零拷贝视图：像素缓冲的寿命与 PageBitmap 绑定，
//      上层必须在上传 GPU 之后再销毁 PageBitmap。
//      Document 与 PageBitmap 共享同一份 fz_context 所有权，
//      因此即使 PageBitmap 比 Document 活得久，也不会出现悬垂 context——
//      代价是 ctx 会延后到最后一个位图销毁时才释放。
//
// 项目纪律：不在开发中实际运行本软件做测试（编译/自动化测试除外）；
//           人工运行验证项统一记录在 docs/04-人工验证.md。

module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

export module lilithreader.document;

export namespace lr {

// ---- 结构化错误码：区分"文件不存在/格式不支持/文件损坏/加密" ----
enum class DocError : std::int32_t {
    Ok = 0,        // 成功
    NotFound,      // 路径不存在或不是普通文件
    NotOpen,       // 文档尚未打开（或已被关闭）
    Unsupported,   // MuPDF 没有对应的文档处理器（内容无法识别）
    NeedsPassword, // 加密文档，需要密码（可调 authenticate 解锁）
    // 内容与扩展名不符、且"照认出来的格式打开会影响用户"。
    // 目前只用于一种情况：压缩包（zip/tar）冒充单文档 —— CBZ 处理器会把归档条目当页，
    // 于是 zip 图集改名 .epub/.pdf 也能"打开"，但页数与内容类别都是错的（ADR-016）。
    // 反过来（epub 改名 .pdf、图片改名 .pdf 等）能如实呈现，属于放宽放行的范围。
    Mismatched,
    Corrupt,       // 文件损坏 / 语法错误 / 截断 / 内容连 MuPDF 都认不出（如纯文本改名 .pdf）
    TooLarge,      // 页面尺寸超出渲染上限
    Internal,      // MuPDF 内部错误（内存、系统调用等）
};

// 英文短标识，用于日志/调试
[[nodiscard]] std::string_view to_string(DocError e) noexcept;
// 中文描述，用于 UI 文案
[[nodiscard]] std::string_view describe(DocError e) noexcept;

// MuPDF 上报的实际格式串（format_utf8）与文件扩展名（ext_utf8，含点）是否"族别一致"。
// 纯提示用途：两侧任一无法判定时返回 true（视为相符）。见 ADR-016。
[[nodiscard]] bool format_matches_extension(std::string_view format_utf8,
                                            std::string_view ext_utf8) noexcept;

// ---- 页面配色（Phase 5）----
//
// 在渲染线程对渲染结果做一次色彩变换（位置决策见 ADR-036；反色端点见 ADR-043）：
//   Normal  原样；
//   Invert  反色（暗色背景阅读）——柔化映射：白→#1F1D1B、黑→#D8D4CE（逐通道 LUT），保留 alpha；
//   EyeCare 护眼（暖色纸张）——RGB 经线性 LUT 映射 黑→#2B2318、白→#F6EEDC。
// 为什么不用 fz_invert_pixmap / fz_tint_pixmap：这两者面向 RGB/Gray，对 **RGBA（带 alpha）
// 的 4 分量 pixmap** 行为不在公开契约里（tint 明确只写 RGB/BGR/Gray）；自实现逐像素变换
// 只改 RGB、保留 alpha，行为可控且可单测。
// 之所以放在渲染层而不是 UI 层叠 shader：纹理是 IMMUTABLE 且零拷贝上传，
// 变换必须发生在像素进入 GPU 之前；且配色变化即触发一次重渲染（缓存整体失效）。
enum class ColorMode : int {
    Normal = 0,
    Invert = 1,
    EyeCare = 2,
};

// ---- 文档元信息 ----

// 单页尺寸（点）。与 canvas 的 PageSizePt 是**同名不同型**的两个结构：
// 两模块互不 import（ADR-003），由 app 层做一次转换。
struct PageSize {
    float width_pt = 0.0f;
    float height_pt = 0.0f;
};

// ---- 目录（outline）条目（Phase 5）----
//
// 从 MuPDF 的 fz_outline 树**展平**为前序序列：depth 表示层级（顶层为 0），
// 顺序即阅读顺序。用展平而非树结构，是为了让 UI 侧只需一次线性遍历即可绘制，
// 且本模块接口不引入递归容器（PIMPL 与 longjmp 纪律都更省心）。
struct OutlineItem {
    std::string title;      // UTF-8，可能为空
    int         page = -1;  // 目标页（0 基）；-1 表示无内部目标（外链/无目标）
    int         depth = 0;  // 层级，顶层为 0
};

struct DocumentInfo {
    int    page_count = 0;
    float  page_width_pt = 0.0f;   // 首页宽度（点，1 点 = 1/72 英寸）
    float  page_height_pt = 0.0f;  // 首页高度（点）
    bool   needs_password = false;
    bool   has_outline = false;    // 是否存在目录（Phase 5 使用）
    // 权限位（Phase 5）：PDF 标准加密可声明"禁复制/禁打印"。UI 层据此把相应功能置灰。
    // 无加密或无法判定时**一律为 true**（保守放行，避免误禁）。
    bool   can_copy = true;
    bool   can_print = true;
    std::string title;             // 元数据标题（UTF-8，可能为空）
    // MuPDF 上报的实际格式串（UTF-8，可能为空）：
    // "PDF 1.7" / "EPUB" / "FictionBook2" / "XPS" / "zip"（CBZ 报归档格式）/ "Image"。
    // 有了它，UI 就能在"扩展名与实际不符"时如实告知用户（ADR-016）。
    std::string format;
    // **逐页**尺寸（点）。**长度 = page_count**；探测失败的那一页为 {0,0}；
    // **整表为空**表示未探测（页数超过上限，见 document.cpp 的 kMaxSizeProbePages），
    // 上层应回退到"用首页尺寸统一布局"。
    //
    // 为什么必须有：PDF 允许各页尺寸/纵横比不同（封面页、插页、横向页、扫描裁切不一）。
    // 画布若只用首页尺寸统一布局，渲染出的各页纹理会被拉伸进"首页纵横比"的矩形，
    // 于是**尺寸不一致的 PDF 会形变**（第三轮调试实测确认）。上层用本表调
    // canvas.set_page_sizes()，行高取行内最大、每页按自身纵横比绘制，形变消失。
    //
    // 探测成本：每页一次 fz_load_page + fz_bound_page（不跑内容流，不解码图像），
    // 实测约 20µs/页；单页损坏只让该页回退为 {0,0}，不影响整篇打开。
    std::vector<PageSize> page_sizes;
};

class Document;  // 前向声明：PageBitmap 需要它作为工厂友元

// ---- 单页渲染结果：MuPDF pixmap 的零拷贝 RGBA8 视图 ----
//
// 像素格式固定为 RGBA8（4 字节/像素），可直接上传
// DXGI_FORMAT_R8G8B8A8_UNORM。渲染时已用不透明白底清屏，因此 alpha 恒为 255。
//
// 非线程安全；只能在创建它的线程内使用。
// 与 Document 共享 fz_context 所有权，因此不要求在本体之前析构；
// 但为控制内存占用，仍应尽早销毁（推荐：GPU 上传完成后立即释放）。
class PageBitmap {
public:
    PageBitmap() noexcept = default;
    ~PageBitmap();
    PageBitmap(PageBitmap&& other) noexcept;
    PageBitmap& operator=(PageBitmap&& other) noexcept;
    PageBitmap(const PageBitmap&) = delete;
    PageBitmap& operator=(const PageBitmap&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int width() const noexcept;       // 像素
    [[nodiscard]] int height() const noexcept;      // 像素
    [[nodiscard]] int stride() const noexcept;      // 字节/行（= width * 4）
    [[nodiscard]] int components() const noexcept;  // 恒为 4
    [[nodiscard]] const std::uint8_t* samples() const noexcept;  // 行主序 RGBA
    [[nodiscard]] std::size_t size_bytes() const noexcept;

    // 实际使用的缩放：若因尺寸上限被下调，这里返回下调后的值
    [[nodiscard]] float effective_scale() const noexcept;

    void reset() noexcept;

private:
    friend class Document;   // 只允许 Document 构造（工厂方法）
    struct Impl;
    explicit PageBitmap(Impl* impl) noexcept;
    Impl* impl_ = nullptr;
};

// ---- 文档对象 ----
//
// 生命周期：open → （可选 authenticate）→ info / page_size / render_page → close
// 约定：一个实例只在一个线程内使用；跨线程迁移请用移动构造转交所有权。
class Document {
public:
    Document() noexcept;
    ~Document();
    Document(Document&& other) noexcept;
    Document& operator=(Document&& other) noexcept;
    Document(const Document&) = delete;
    Document& operator=(const Document&) = delete;

    // 打开文档（path 为 Windows 原生 UTF-16 路径）。
    // 返回 NeedsPassword 时文档仍处于"已打开"状态，可继续调 authenticate；
    // 其余错误码下文档保持关闭。
    DocError open(const std::wstring& path) noexcept;
    void     close() noexcept;
    [[nodiscard]] bool is_open() const noexcept;

    // 用密码解锁（UTF-8）。成功后 is_open() 为 true 且可读取内容。
    DocError authenticate(std::string_view utf8_password) noexcept;

    // 读取元信息。需已打开且已解锁。
    DocError info(DocumentInfo& out) const noexcept;

    // 读取文档目录（outline）。展平为前序序列（depth 表层级，见 OutlineItem）。
    // 无目录时返回 Ok 且 out 为空（不是错误）。需已打开且已解锁。
    DocError outline(std::vector<OutlineItem>& out) const noexcept;

    // 读取第 index 页的尺寸（点）。index 从 0 开始。注意：PDF 各页尺寸可以不同。
    DocError page_size(int index, float& width_pt, float& height_pt) const noexcept;

    // 渲染第 index 页为 RGBA8 位图。
    //   scale        : 1.0 = 72dpi 原始尺寸；2.0 = 144dpi
    //   max_dimension: 单边像素上限。超出时自动等比下调实际 scale
    //                  （结果见 PageBitmap::effective_scale()），保证不 OOM。
    //   rotation_deg : 页面旋转（0/90/180/270，顺时针）。90/270 时输出宽高互换；
    //                  max_dimension 按**旋转后**包围盒钳制。
    //   color_mode   : 页面配色（Normal/Invert/EyeCare），在像素上传前于渲染线程完成。
    // 只在拥有本对象的线程内调用；out 会被先重置。
    DocError render_page(int index, float scale, PageBitmap& out,
                         int max_dimension = 8192,
                         int rotation_deg = 0,
                         ColorMode color_mode = ColorMode::Normal) noexcept;

    // 最近一次失败的原始信息（UTF-8，截断到 1024 字节），调试/日志用。
    [[nodiscard]] std::string_view last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lr
