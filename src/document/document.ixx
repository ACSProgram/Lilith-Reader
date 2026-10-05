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

// ---- 文档元信息 ----
struct DocumentInfo {
    int    page_count = 0;
    float  page_width_pt = 0.0f;   // 首页宽度（点，1 点 = 1/72 英寸）
    float  page_height_pt = 0.0f;  // 首页高度（点）
    bool   needs_password = false;
    bool   has_outline = false;    // 是否存在目录（Phase 5 使用）
    std::string title;             // 元数据标题（UTF-8，可能为空）
    // MuPDF 上报的实际格式串（UTF-8，可能为空）：
    // "PDF 1.7" / "EPUB" / "FictionBook2" / "XPS" / "zip"（CBZ 报归档格式）/ "Image"。
    // 有了它，UI 就能在"扩展名与实际不符"时如实告知用户（ADR-016）。
    std::string format;
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

    // 读取第 index 页的尺寸（点）。index 从 0 开始。注意：PDF 各页尺寸可以不同。
    DocError page_size(int index, float& width_pt, float& height_pt) const noexcept;

    // 渲染第 index 页为 RGBA8 位图。
    //   scale        : 1.0 = 72dpi 原始尺寸；2.0 = 144dpi
    //   max_dimension: 单边像素上限。超出时自动等比下调实际 scale
    //                  （结果见 PageBitmap::effective_scale()），保证不 OOM。
    // 只在拥有本对象的线程内调用；out 会被先重置。
    DocError render_page(int index, float scale, PageBitmap& out,
                         int max_dimension = 8192) noexcept;

    // 最近一次失败的原始信息（UTF-8，截断到 1024 字节），调试/日志用。
    [[nodiscard]] std::string_view last_error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lr
