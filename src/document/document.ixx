// document.ixx — Lilith Reader 文档核心模块
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

// ---- 纸张方案（页面配色；分层渲染见 ADR-067）----
//
// 决策位置见 ADR-036/043；**按内容分层**见 ADR-067。这里只描述对外语义：
//   Original  原样（不做任何变换，走单遍快路径）；
//   Dark      「深色纸张」：深暖灰纸面 + 浅暖灰墨迹（柔化暗色映射，白→#1F1D1B、黑→#D7D5D3）；
//   Warm      「暖色」：米黄纸面 + 深褐墨迹（黑→#2B2318、白→#F6EEDC）。
//
// **变换只作用于"纸墨层"（背景/文字/矢量/单色蒙版图），照片与插图原样保留**
// （按 kImageDim 略降不透明度压暗，见 document.cpp）。这是本模块与「整张位图套一个 LUT」
// 的关键差别：后者分不清纸墨与照片，会把照片一起变成负片或一起染黄。
//
// 为什么不用 fz_invert_pixmap / fz_tint_pixmap：这两者面向 RGB/Gray，对 **RGBA（带 alpha）
// 的 4 分量 pixmap** 行为不在公开契约里（tint 明确只写 RGB/BGR/Gray）；自实现逐像素变换
// 只改 RGB、保留 alpha，行为可控且可单测。
// 之所以放在渲染层而不是 UI 层叠 shader：纹理是 IMMUTABLE 且零拷贝上传，
// 变换必须发生在像素进入 GPU 之前；且纸张方案变化即触发一次重渲染（缓存整体失效）。
//
// **枚举数值 0/1/2 是持久化契约**（reader_state.bin 的 scheme 字段），不得重排。
enum class PageScheme : int {
    Original = 0,
    Dark     = 1,
    Warm     = 2,
};

// ---- 文档元信息 ----

// 单页尺寸（点）。与 canvas 的 PageSizePt 是**同名不同型**的两个结构：
// 两模块互不 import（ADR-003），由 app 层做一次转换。
struct PageSize {
    float width_pt = 0.0f;
    float height_pt = 0.0f;
};

// ---- 目录（outline）条目 ----
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
    bool   has_outline = false;    // 是否存在目录
    // 权限位：PDF 标准加密可声明"禁复制/禁打印"。UI 层据此把相应功能置灰。
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
//
// 属性访问（ADR-083）：w/h/stride/samples 在**构造时一次性快照**，访问器是纯读取、
// 不再回调 MuPDF；资源释放在统一的边界函数内受 fz_try/fz_catch 覆盖。
class PageBitmap {
public:
    PageBitmap() noexcept = default;
    ~PageBitmap();
    PageBitmap(PageBitmap&& other) noexcept;
    PageBitmap& operator=(PageBitmap&& other) noexcept;
    PageBitmap(const PageBitmap&) = delete;
    PageBitmap& operator=(const PageBitmap&) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] int width() const noexcept;       // 像素（构造时快照）
    [[nodiscard]] int height() const noexcept;      // 像素（构造时快照）
    [[nodiscard]] int stride() const noexcept;      // 字节/行（= width * 4，构造时快照）
    [[nodiscard]] const std::uint8_t* samples() const noexcept;  // 行主序 RGBA（构造时快照）

    // 实际使用的缩放：若因尺寸上限被下调，这里返回下调后的值
    [[nodiscard]] float effective_scale() const noexcept;

    void reset() noexcept;

private:
    friend class Document;   // 只允许 Document 构造（工厂方法）
    struct Impl;
    explicit PageBitmap(Impl* impl) noexcept;
    Impl* impl_ = nullptr;
};

// tile 在旋转后输出像素空间中的矩形。x/y 相对于整页输出的左上角。
// 调度层用它把超大页面拆成有界纹理，避免为整页分配单个巨型 pixmap。
struct TileRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// ---- 页面可交互内容：文本布局 / 嵌入图片 / 链接 ----
//
// **坐标空间约定（唯一且贯穿全部接口）**：一律是**未旋转的页面 pt 空间**，即
// fz_stext_page / fz_load_links 原生所在的坐标系（原点在 MediaBox 左上、y 向下、
// 单位 = 点）。它与 render_page 的像素输出只差一个 ctm：
//   pixel = transform(point, scale × rotate(user_rotation)) − pixmap 原点
// 因此**旋转折算由 app 层做**（0/90/180/270 的归一化坐标换算是纯浮点运算），
// 本模块不必也不该知道用户当前的旋转角 —— 否则同一份抽取结果无法跨旋转复用。
//
// 为什么四边形而不是矩形：旋转/斜排的文字外接框不是轴对齐的，用矩形高亮会错位；
// fz_stext_char 本来就给 quad，原样带出来即可。
struct TextQuad {
    float ulx = 0, uly = 0;   // 左上
    float urx = 0, ury = 0;   // 右上
    float llx = 0, lly = 0;   // 左下
    float lrx = 0, lry = 0;   // 右下
};

// 单个字符。cp <= 0 的字符（MuPDF 用于标记换行/占位的空字符）已在抽取时滤除。
struct TextChar {
    std::uint32_t cp = 0;     // Unicode 码点（UTF-32）
    TextQuad      quad{};     // 外接四边形（页面 pt）
    int           line = 0;   // 所属行，PageContent::lines 的下标
};

// 一行文字的外接框。用途：命中测试时"点在行间空白"要吸附到最近的行。
struct TextLine {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// 页面内嵌入图片的外接矩形（来自 stext 的 IMAGE 块）。
struct PageImageRect {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// 页面内的超链接热区。内部跳转与外部 URL 用同一结构表达：
// target_page >= 0 ⇒ 文档内跳转（点击翻到该页）；否则看 uri（外部 URL / 无法解析）。
struct PageLink {
    float       x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // 热区（页面 pt）
    int         target_page = -1;                 // 文档内目标页（0 基）；-1 = 外部或无法解析
    std::string uri;                              // 原始 URI（UTF-8，可能为空）
};

// 一页的"可交互内容"快照。三部分一次抽好：UI 侧只做纯浮点命中测试，
// 不必为每次鼠标移动回工作线程（否则拖动选择会与渲染抢线程、明显卡顿）。
struct PageContent {
    std::vector<TextChar>      chars;
    std::vector<TextLine>      lines;
    std::vector<PageImageRect> images;
    std::vector<PageLink>      links;
};

// 复制图片的结果：RGBA8 行主序（stride = w × 4），已是独立 CPU 缓冲（无 fz 生命周期）。
struct ImageData {
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    [[nodiscard]] bool valid() const noexcept { return w > 0 && h > 0 && !rgba.empty(); }
};

// ---- 导出为图片 ----
//
// 支持的编码格式（均由 MuPDF 内置编码器提供，**不引入新依赖**）：
//   Png  —— 无损，体积大；走 fz_new_buffer_from_pixmap_as_png（RGBA 直出，页面已是不透明白底）。
//   Jpeg —— 有损，质量可调（1~100）；走 fz_new_buffer_from_pixmap_as_jpeg（先转 RGB，JPEG 无 alpha）。
// MuPDF 还能写 JP2 / PNM / PSD / PCL 等，但面向用户无意义或过于专业，故只开放这两种。
enum class ImageFormat : int {
    Png  = 0,
    Jpeg = 1,
};

// 一次搜索命中。矩形是命中文字的**外接框**（未旋转页面 pt）；snippet 是命中所在行的
// 文本（结果列表显示上下文用，已截断）。同一处命中可能跨行 —— 跨行时拆成多条，
// 这是刻意的：列表里逐条可跳转，比一条含换行的记录更好用。
struct SearchHit {
    int         page = -1;
    float       x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    std::string snippet;
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

    // 渲染第 index 页为 RGBA8 位图。
    //   scale        : 1.0 = 72dpi 原始尺寸；2.0 = 144dpi
    //   max_dimension: 单边像素上限。超出时自动等比下调实际 scale
    //                  （结果见 PageBitmap::effective_scale()），保证不 OOM。
    //   rotation_deg : 页面旋转（0/90/180/270，顺时针）。90/270 时输出宽高互换；
    //                  max_dimension 按**旋转后**包围盒钳制。
    //   scheme       : 纸张方案（Original/Dark/Warm）。变换在像素上传前于渲染线程完成，
    //                  **只作用于纸墨层**，照片与插图原样保留（ADR-067）。
    // 只在拥有本对象的线程内调用；out 会被先重置。
    DocError render_page(int index, float scale, PageBitmap& out,
                         int max_dimension = 8192,
                         int rotation_deg = 0,
                         PageScheme scheme = PageScheme::Original) noexcept;

    // 只栅格化整页输出像素空间中的一个 tile。scale 不因整页尺寸被钳低；
    // max_dimension 仅作为 tile 单边防御上限（0 表示不额外钳制）。
    DocError render_page_tile(int index, float scale, TileRect tile, PageBitmap& out,
                              int max_dimension = 0,
                              int rotation_deg = 0,
                              PageScheme scheme = PageScheme::Original) noexcept;

    // ---- 文本 / 图片 / 链接 ----
    //
    // 三者都只在拥有本对象的线程内调用（与 render_page 同一纪律）。
    // 内部维护**一条** stext 缓存（最近一页）：抽一次文本的代价与渲染一页相当，
    // 而选择/复制会在同一页上反复触发，故必须缓存；只缓存一条即可 —— 交互始终
    // 集中在鼠标所在的那一页，跨页时重建一次（毫秒级）可接受，且内存有界。

    // 读取一页的可交互内容。**无文本层的页（纯扫描件）返回 Ok 且 chars 为空**，
    // 这不是错误 —— 本模块不做 OCR（产品决定），上层据此把"选择文本"置灰。
    // 返回的坐标全部在未旋转页面 pt 空间（见文件头坐标约定）。
    DocError page_content(int index, PageContent& out) const noexcept;

    // 取选中范围的纯文本。a = 按下点、b = 当前点（均为未旋转页面 pt）。
    // 内部走 fz_copy_selection：按**阅读顺序**拼接，自动补词间空格与换行 ——
    // 自己拼字符会漏掉"两个词之间有间隙"这类只有版式知道的信息（英文尤甚）。
    // 失败或空选区返回 Ok + 空串（复制空串由上层决定是否写剪贴板）。
    DocError copy_text(int index, float ax, float ay, float bx, float by,
                       std::string& out) noexcept;

    // 取包含点 (x,y) 的**嵌入图片**（原始分辨率，RGBA8）。
    // 该点没有图片时返回 DocError::NotFound（不是错误，是"此处无图"）。
    DocError image_at(int index, float x, float y, ImageData& out) noexcept;

    // 在**单页**内搜索关键字（UTF-8，大小写不敏感；MuPDF 负责 Unicode 归一）。
    // out 先清空，随后追加该页的命中（最多 max_hits 条，防止病态文档撑爆内存）。
    // 无文本层的页（扫描件）返回 Ok + 空表 —— 本模块不做 OCR。
    // 分页搜索的调度（跨页顺序、取消、进度）在 render 层，本模块只负责"一页"。
    DocError search_page(int index, std::string_view utf8_needle, int max_hits,
                         std::vector<SearchHit>& out) noexcept;

    // ---- 导出为图片 ----
    //
    // 把第 index 页渲染并编码为图片，按 Windows 原生 UTF-16 路径写出。
    // 渲染**复用 render_page 的完整路径**（含旋转与纸张方案），故"导出当前视图"无需另写逻辑。
    //   scale        : 1.0 = 72dpi；导出时由调用方按 DPI 折算（scale = dpi / 72）。
    //   fmt          : Png / Jpeg。
    //   quality      : JPEG 质量 1~100（Png 忽略）。
    //   max_dimension: 单边像素上限（超出等比下调，实际倍率见 out_effective_scale）。
    //   rotation_deg / scheme : 视图变换（导出"当前视图"时传入当前值，导出"原始页面"传 0）。
    //   path         : 输出文件的 Windows 原生 UTF-16 路径。
    //   out_effective_scale : 可空；回填实际使用的倍率（被上限下调时 < scale）。
    //
    // 为什么编码放在 document 层而不是 app 层：fz_* 只能出现在本模块（ADR-003/009）。
    // 编码结果先落 fz_buffer，再经 Win32 写出 —— 避免把宽字符路径交给 MuPDF 的
    // UTF-8 文件名入口（非 ASCII 路径会踩 ANSI 代码页）。
    DocError save_page_as_image(int index, float scale, ImageFormat fmt, int quality,
                                int max_dimension, int rotation_deg, PageScheme scheme,
                                const std::wstring& path,
                                float* out_effective_scale = nullptr) noexcept;

    // 最近一次失败的原始信息（UTF-8，截断到 1024 字节），调试/日志用。
    [[nodiscard]] std::string_view last_error() const noexcept;

private:
    DocError render_page_region(int index, float scale, PageBitmap& out,
                                int max_dimension, int rotation_deg,
                                PageScheme scheme, const TileRect* tile) noexcept;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace lr
