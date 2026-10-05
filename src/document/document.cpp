// document.cpp — lilithreader.document 的实现单元（Phase 2）
//
// 【模块边界纪律 · 必读】
//   MuPDF 的错误处理基于 setjmp/longjmp（见 mupdf/fitz/context.h 的 fz_try/fz_catch），
//   不是 C++ 异常。由此产生两条硬约束：
//     1. fz_try 块内**绝不能**构造带析构函数的 C++ 对象（std::string / std::vector …）——
//        longjmp 不会调用析构函数，会直接泄漏。同理，跨越 fz_try 存活的对象也会漏。
//        因此本文件所有 C++ 对象操作都放在 fz_try 之外，块内只出现 fz_* 裸指针
//        和 POD 缓冲（char[]）。
//     2. 跨 fz_try / fz_always / fz_catch 使用的局部变量必须用 fz_var() 标记，
//        否则可能被寄存器分配优化破坏（MuPDF 官方文档明确要求）。
//   这是相对 Lilith 原版的重做点之一，详见 docs/03-决策记录.md（ADR-009）。

module;

#define NOMINMAX
#include <windows.h>

#include <mupdf/fitz.h>

#include <cstring>
#include <memory>
#include <new>
#include <string>

// MSVC 会对 C++（/EHsc）中出现的任何 _setjmp 报 C4611「_setjmp 与 C++ 对象析构的
// 交互不可移植」，且**与函数内是否真有待析构对象无关**。已用最小样例验证：
// 一个只含 `jmp_buf b; if (_setjmp(b) == 0) …` 与 int 的函数同样报警（每次编译
// 单元只在首次出现处报一条）。MuPDF 的 fz_setjmp 在 MSVC 上展开为 _setjmp，
// 因此该警告在本文件必然出现，无法通过改写代码消除。
//
// 它也不代表文件头那两条纪律被破坏：本文件所有 fz_try 体内只有 fz_* 裸指针与
// POD 缓冲。关闭该警告后，纪律改由「文件头注释 + ADR-009 + 代码评审」把关；
// 新增 fz_* 调用时请自觉遵守，尤其不要在 fz_try 内引入 std::string/容器。
#pragma warning(disable : 4611)

module lilithreader.document;

namespace lr {
namespace {

// ---- 常量 ----

// MuPDF 内部资源存储上限。留一份解码缓存给它，同时保证进程内存有界。
// 页面级缓存不依赖它（由 render 层自己按字节预算管理，Phase 4）。
constexpr std::size_t kStoreBytes = 128u << 20;  // 128 MB

// pixmap 分量数：RGB + alpha ⇒ RGBA8
constexpr int kComponents = 4;

// 错误信息缓冲区（POD，可安全跨越 longjmp）
constexpr std::size_t kErrCap = 1024;

// fz_context 的共享所有权句柄。
//
// MuPDF 没有 fz_keep_context（context 不可引用计数），而释放 fz_pixmap 必须用它
// 创建时所在的 context。若 PageBitmap 只存一个裸 fz_context*，一旦它比 Document
// 活得久就是 use-after-free。这里让 context 由 shared_ptr 持有：PageBitmap 与
// Document 各持一份，ctx 必定后于所有位图析构，从结构上消除悬垂。
// 代价：Document 关闭后，若仍有位图存活，ctx（含其内部 store）会延后释放。
struct CtxHandle {
    fz_context* ctx = nullptr;
    ~CtxHandle() {
        if (ctx) fz_drop_context(ctx);
    }
};

// ---- 小工具（全部平凡类型，不参与 fz_try 的析构语义） ----

bool is_regular_file(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// 取小写扩展名（含点，如 ".pdf"）写入 out。
// 非 ASCII 扩展名一律写空串，交由 MuPDF 按内容嗅探。
// MuPDF 的 magic 约定：可以是文件名扩展名（含点）或 MIME 类型。
void extension_magic(const std::wstring& path, char (&out)[16]) noexcept {
    out[0] = '\0';
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return;
    const std::size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos && dot < sep) return;  // 目录名里的点不算扩展名

    std::size_t n = 0;
    for (std::size_t i = dot; i < path.size() && n + 1 < sizeof(out); ++i) {
        const wchar_t wc = path[i];
        if (wc < 0x21 || wc > 0x7E) {  // 非 ASCII 可见字符
            out[0] = '\0';
            return;
        }
        char c = static_cast<char>(wc);
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out[n++] = c;
    }
    out[n] = '\0';
}

// 把 MuPDF 当前的错误信息拷进 POD 缓冲（fz_catch 内调用，只做记忆拷贝）
void copy_caught_message(fz_context* ctx, char* buf, std::size_t cap) noexcept {
    if (!buf || cap == 0) return;
    buf[0] = '\0';
    const char* msg = fz_caught_message(ctx);  // 需紧跟抛出的 fz_try/fz_catch
    if (!msg) return;
    std::size_t n = 0;
    while (msg[n] != '\0' && n + 1 < cap) ++n;
    std::memcpy(buf, msg, n);
    buf[n] = '\0';
}

// 错误码分类：优先用 MuPDF 的错误码，只在笼统错误（GENERIC）时回退到消息特征。
// 加密 PDF 在 1.26 上走的是 FZ_ERROR_GENERIC + "cannot authenticate password"，
// 因此消息判断不可省。
DocError classify(fz_context* ctx) noexcept {
    switch (fz_caught(ctx)) {
    case FZ_ERROR_UNSUPPORTED: return DocError::Unsupported;
    case FZ_ERROR_LIMIT:       return DocError::TooLarge;
    case FZ_ERROR_FORMAT:
    case FZ_ERROR_SYNTAX:      return DocError::Corrupt;
    case FZ_ERROR_SYSTEM:
    case FZ_ERROR_LIBRARY:
    case FZ_ERROR_ARGUMENT:    return DocError::Internal;
    default:                   break;  // GENERIC 等笼统错误 → 按消息细分
    }

    const char* msg = fz_caught_message(ctx);
    if (!msg) return DocError::Corrupt;
    if (std::strstr(msg, "password")) return DocError::NeedsPassword;
    if (std::strstr(msg, "document handler") || std::strstr(msg, "unsupported") ||
        std::strstr(msg, "unknown document") || std::strstr(msg, "cannot recognize"))
        return DocError::Unsupported;
    if (std::strstr(msg, "out of memory") || std::strstr(msg, "cannot allocate"))
        return DocError::Internal;
    return DocError::Corrupt;
}

// 尺寸钳制：只用请求 scale 算输出像素，超上限就等比下调（只缩不放）。
// 纯计算，不调用任何 fz_*，故可放在 fz_try 内。
float clamp_scale(fz_rect bounds, float scale, int max_dimension) noexcept {
    const float w = bounds.x1 - bounds.x0;
    const float h = bounds.y1 - bounds.y0;
    if (!(w > 0.0f) || !(h > 0.0f) || !(scale > 0.0f)) return scale > 0.0f ? scale : 1.0f;
    const float limit = static_cast<float>(max_dimension);
    float s = scale;
    if (w * s > limit) s = limit / w;
    if (h * s > limit) s = limit / h;
    return s;
}

// ---- 页面配色变换（Phase 5）----
//
// 为什么不用 fz_invert_pixmap / fz_tint_pixmap：这两者面向 RGB/Gray，对 **RGBA（带 alpha）
// 的 4 分量 pixmap** 的行为不在公开契约里（tint 明确只写 RGB/BGR/Gray）。本函数只处理
// 已知的 RGBA8（n==4），逐像素改 RGB、**保留 alpha**，行为完全可控且可单测。
// 只做 POD 运算、不构造 C++ 对象，故可安全放在 fz_try 内调用。
//
//   mode 1（反色）：**柔化**的暗色映射，而非纯黑底白字（纯反色刺眼）：
//                   白 → 深暖灰 #1F1D1B，黑 → 浅暖灰 #D8D4CE，中间调线性过渡；
//                   逐通道 LUT，等价于"先反相再把对比度压到 [暗,亮] 区间"。
//   mode 2（护眼）：RGB 经 LUT 线性映射 黑→#2B2318、白→#F6EEDC（暖色纸张）
void apply_color_transform(fz_context* ctx, fz_pixmap* pix, int mode) noexcept {
    if (pix == nullptr || mode == 0) return;
    if (fz_pixmap_components(ctx, pix) != kComponents) return;  // 只处理 RGBA8
    const int w = fz_pixmap_width(ctx, pix);
    const int h = fz_pixmap_height(ctx, pix);
    const int stride = fz_pixmap_stride(ctx, pix);
    unsigned char* p = fz_pixmap_samples(ctx, pix);
    if (p == nullptr || w <= 0 || h <= 0 || stride <= 0) return;

    if (mode == 1) {
        constexpr unsigned char kDark[3]  = { 0x1F, 0x1D, 0x1B };  // 白 → 深暖灰
        constexpr unsigned char kLight[3] = { 0xD8, 0xD4, 0xCE };  // 黑 → 浅暖灰
        unsigned char lut[3][256];
        for (int c = 0; c < 3; ++c)
            for (int v = 0; v < 256; ++v)
                lut[c][v] = static_cast<unsigned char>(
                    kDark[c] + (kLight[c] - kDark[c]) * (255 - v) / 255);
        for (int y = 0; y < h; ++y) {
            unsigned char* row = p + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            for (int x = 0; x < w; ++x) {
                row[0] = lut[0][row[0]];
                row[1] = lut[1][row[1]];
                row[2] = lut[2][row[2]];
                row += 4;  // alpha 不动
            }
        }
    } else if (mode == 2) {
        constexpr int kBlk[3] = { 0x2B, 0x23, 0x18 };  // 黑映射到的深暖褐
        constexpr int kWht[3] = { 0xF6, 0xEE, 0xDC };  // 白映射到的米黄
        unsigned char lut[3][256];
        for (int c = 0; c < 3; ++c)
            for (int v = 0; v < 256; ++v)
                lut[c][v] = static_cast<unsigned char>(
                    kBlk[c] + (kWht[c] - kBlk[c]) * v / 255);
        for (int y = 0; y < h; ++y) {
            unsigned char* row = p + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
            for (int x = 0; x < w; ++x) {
                row[0] = lut[0][row[0]];
                row[1] = lut[1][row[1]];
                row[2] = lut[2][row[2]];
                row += 4;  // alpha 不动
            }
        }
    }
}

// ---- 目录遍历（Phase 5）----
//
// fz_outline 是 next（同级）/ down（子级）构成的树。这里前序遍历并**展平**为
// OutlinePOD 数组（只含 POD，可安全在 fz_try 内写），返回后由调用方转成
// std::vector<OutlineItem>（C++ 对象一律在 fz_try 之外构造，ADR-009）。
constexpr int kMaxOutlineNodes = 2048;  // 真实书籍目录远小于此；超出则截断
constexpr int kMaxOutlineDepth = 32;    // 防病态文档的深树导致递归爆栈

struct OutlinePOD {
    char title[384];
    int  page;   // -1 = 无内部目标
    int  depth;
};

void walk_outline(fz_context* ctx, fz_document* doc, fz_outline* node, int depth,
                  OutlinePOD* buf, int cap, int& count, int& truncated) noexcept {
    if (depth > kMaxOutlineDepth) return;
    for (fz_outline* n = node; n != nullptr; n = n->next) {
        if (count >= cap) { truncated = 1; return; }
        OutlinePOD& e = buf[count++];
        e.depth = depth;
        e.page = -1;
        e.title[0] = '\0';
        if (n->title != nullptr) {
            std::size_t k = 0;
            while (n->title[k] != '\0' && k + 1 < sizeof e.title) {
                e.title[k] = n->title[k];
                ++k;
            }
            e.title[k] = '\0';
        }

        // 目标页：优先用 MuPDF 已解析的 fz_location；无目标时退回按 uri 解析
        // （PDF 目录常见 "page=5" 这类内部链接）。两者都解析不出则留 -1。
        int pg = -1;
        if (n->page.chapter >= 0 || n->page.page >= 0) {
            pg = fz_page_number_from_location(ctx, doc, n->page);
        } else if (n->uri != nullptr) {
            float xp = 0.0f, yp = 0.0f;
            const fz_location loc = fz_resolve_link(ctx, doc, n->uri, &xp, &yp);
            pg = fz_page_number_from_location(ctx, doc, loc);
        }
        if (pg >= 0) e.page = pg;

        if (n->down != nullptr)
            walk_outline(ctx, doc, n->down, depth + 1, buf, cap, count, truncated);
    }
}

// 单页尺寸（点）。失败返回 false，调用方把该页留成 {0,0} 由上层回退。
//
// 每页一次独立的 fz_try：页树里混入坏对象（实测有 "non-page object in page tree"）
// 只让**那一页**探测失败，不会把整篇 info() 拖成错误 —— 与渲染路径的容错粒度一致。
// 块内只有 fz_* 裸指针 / fz_rect / int 等平凡类型，符合 ADR-009 的 setjmp 纪律。
bool bound_page_pt(fz_context* ctx, fz_document* doc, int index,
                   float& w, float& h) noexcept {
    fz_page* page = nullptr;
    fz_rect  bounds{};
    int      ok = 0;
    fz_try(ctx) {
        fz_var(page);
        fz_var(bounds);
        fz_var(ok);
        page = fz_load_page(ctx, doc, index);
        bounds = fz_bound_page(ctx, page);
        ok = 1;
    }
    fz_always(ctx) {
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        ok = 0;
    }
    if (ok != 0) {
        w = bounds.x1 - bounds.x0;
        h = bounds.y1 - bounds.y0;
    }
    return ok != 0;
}

// 逐页尺寸探测的页数上限。
//
// 探测是 O(页数) 的（每页一次 fz_load_page + fz_bound_page，不跑内容流）。
// 实测代价约 20µs/页（1000 页 ≈ 18ms），正常书籍可忽略；但**损坏或构造的文档**
// 可能上报天文数字的页数（页树随文件大小膨胀），不设上限就会在"打开"阶段长时间卡住。
// 超过上限时**不填** page_sizes，上层自然回退到"统一尺寸"布局（该文档的形变防线失效，
// 但真实书籍远小于此阈值）。
constexpr int kMaxSizeProbePages = 10000;

// ---- 格式实测（ADR-016） ----
//
// MuPDF 的识别比扩展名"聪明"得多：它给每个处理器同时算**内容分**与**扩展名分**，
// 内容分来自各处理器的 recognize_content，扩展名命中只给 1 分（弱信任）；
// 两者都命中才是 100+。实测（1.26.10）由此出现一批"认出来就能开"的情况：
//   · 单张图片内容分高 ⇒ 图片改名 .pdf/.epub 都以"1 页图片文档"打开（视觉上无害）；
//   · epub/xps/fb2 改名 .pdf ⇒ 照原格式正常打开，只是后缀不实；
//   · zip 里只要有图片，CBZ 处理器就得 25 分，压过 epub 的 1 分
//     ⇒ **zip 图集改名 .epub/.pdf 也会打开，但页数=图片条目数、类别完全不对**。
//
// 因此策略不是"扩展名契约"，而是"内容优先、只拦会骗人的"（ADR-016）：
//   · 能认出格式 ⇒ 打开，UI 如实显示实际格式（DocumentInfo::format）；
//   · 唯一拒绝的情况：**归档（zip/tar）冒充单文档**——页数是处理器按条目编出来的，
//     用户以为在读电子书，实际拿到的是漫画条目列表，这种"有影响的"错配必须拦。
//
// 判定依据是 MuPDF 自己的结论 fz_lookup_metadata(doc, FZ_META_FORMAT)（实测值）：
//   PDF → "PDF 1.4"   EPUB → "EPUB"   FB2 → "FictionBook2"
//   XPS → "XPS"       CBZ  → 归档格式 "zip"/"tar"   单页图片 → "Image"
// 处理器没实现该元数据时返回 -1，此时不做任何判定（宁可不拦，也不误伤）。
enum class FormatFamily : int { Unknown, Pdf, Epub, Fb2, Xps, Archive, Image };

FormatFamily classify_format(const char* fmt) noexcept {
    if (fmt == nullptr || fmt[0] == '\0') return FormatFamily::Unknown;
    if (std::strncmp(fmt, "PDF", 3) == 0) return FormatFamily::Pdf;
    if (std::strcmp(fmt, "EPUB") == 0) return FormatFamily::Epub;
    if (std::strncmp(fmt, "FictionBook", 11) == 0) return FormatFamily::Fb2;
    if (std::strcmp(fmt, "XPS") == 0) return FormatFamily::Xps;
    if (std::strcmp(fmt, "zip") == 0 || std::strcmp(fmt, "tar") == 0)
        return FormatFamily::Archive;  // CBZ 上报的是归档格式名
    if (std::strcmp(fmt, "Image") == 0) return FormatFamily::Image;
    return FormatFamily::Unknown;  // 不认识的上报值 ⇒ 不判定
}

// 扩展名所暗示的格式族（ext 为小写含点，取自 extension_magic）。
// 用途：① 判断"归档冒充单文档"；② UI 提示"扩展名与实际格式是否相符"。
// 白名单本体在 utils.ixx（含单页图片，ADR-017），两处需保持同步。
FormatFamily extension_family(const char* ext) noexcept {
    if (ext == nullptr || ext[0] == '\0') return FormatFamily::Unknown;
    if (std::strcmp(ext, ".pdf") == 0) return FormatFamily::Pdf;
    if (std::strcmp(ext, ".epub") == 0) return FormatFamily::Epub;
    if (std::strcmp(ext, ".fb2") == 0) return FormatFamily::Fb2;
    if (std::strcmp(ext, ".xps") == 0) return FormatFamily::Xps;
    if (std::strcmp(ext, ".cbz") == 0) return FormatFamily::Archive;
    if (std::strcmp(ext, ".png") == 0 || std::strcmp(ext, ".jpg") == 0 ||
        std::strcmp(ext, ".jpeg") == 0 || std::strcmp(ext, ".gif") == 0 ||
        std::strcmp(ext, ".bmp") == 0 || std::strcmp(ext, ".tif") == 0 ||
        std::strcmp(ext, ".tiff") == 0)
        return FormatFamily::Image;
    // .mobi 等未实测的格式串：返回 Unknown ⇒ 既不拦，UI 也不报"不符"（见 ADR-016 已知缺口）
    return FormatFamily::Unknown;
}

bool accepts_archive(const char* ext) noexcept {
    return extension_family(ext) == FormatFamily::Archive;
}

// 把 string_view 拷进固定缓冲（截断 + NUL 结尾），避免在 noexcept 接口里分配内存
void copy_truncated(std::string_view src, char* dst, std::size_t cap) noexcept {
    if (cap == 0) return;
    std::size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    if (n > 0) std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

}  // namespace

// ---- 错误码文案 ----

std::string_view to_string(DocError e) noexcept {
    switch (e) {
    case DocError::Ok:            return "ok";
    case DocError::NotFound:      return "not-found";
    case DocError::NotOpen:       return "not-open";
    case DocError::Unsupported:   return "unsupported";
    case DocError::NeedsPassword: return "needs-password";
    case DocError::Mismatched:    return "mismatched";
    case DocError::Corrupt:       return "corrupt";
    case DocError::TooLarge:      return "too-large";
    case DocError::Internal:      return "internal";
    }
    return "unknown";
}

std::string_view describe(DocError e) noexcept {
    switch (e) {
    case DocError::Ok:            return "正常";
    case DocError::NotFound:      return "文件不存在";
    case DocError::NotOpen:       return "文档未打开";
    case DocError::Unsupported:   return "暂不支持的格式";
    case DocError::NeedsPassword: return "文档已加密，需要密码";
    case DocError::Mismatched:    return "这是压缩包，不是单个文档";
    case DocError::Corrupt:       return "文件已损坏或无法解析";
    case DocError::TooLarge:      return "页面尺寸超出渲染上限";
    case DocError::Internal:      return "文档解析内部错误";
    }
    return "未知错误";
}

// ---- 格式与扩展名是否相符（供 UI 如实提示，不参与拒绝判定） ----
//
// 与拒绝逻辑无关（拒绝只看"归档冒充单文档"）：这里纯粹为了 UI 能说清
// "你打开的实际是什么"。任一侧无法判定（含 .mobi 这类未实测格式串）⇒ 视为相符，
// 不打扰用户。
bool format_matches_extension(std::string_view format_utf8, std::string_view ext_utf8) noexcept {
    char fmt[64] = {};
    char ext[16] = {};
    copy_truncated(format_utf8, fmt, sizeof fmt);
    copy_truncated(ext_utf8, ext, sizeof ext);
    for (char* p = ext; *p != '\0'; ++p)
        if (*p >= 'A' && *p <= 'Z') *p = static_cast<char>(*p - 'A' + 'a');

    const FormatFamily actual = classify_format(fmt);
    const FormatFamily declared = extension_family(ext);
    if (actual == FormatFamily::Unknown || declared == FormatFamily::Unknown) return true;
    return actual == declared;
}

// ---- PageBitmap ----
struct PageBitmap::Impl {
    std::shared_ptr<CtxHandle> ch;   // ctx 的共享所有权（见 CtxHandle 说明）
    fz_pixmap* pix = nullptr;
    float      scale = 1.0f;

    [[nodiscard]] fz_context* ctx() const noexcept { return ch ? ch->ctx : nullptr; }
};

PageBitmap::PageBitmap(Impl* impl) noexcept : impl_(impl) {}

PageBitmap::~PageBitmap() { reset(); }

PageBitmap::PageBitmap(PageBitmap&& other) noexcept : impl_(other.impl_) {
    other.impl_ = nullptr;
}

PageBitmap& PageBitmap::operator=(PageBitmap&& other) noexcept {
    if (this != &other) {
        reset();
        impl_ = other.impl_;
        other.impl_ = nullptr;
    }
    return *this;
}

void PageBitmap::reset() noexcept {
    if (!impl_) return;
    if (impl_->pix) {
        if (fz_context* ctx = impl_->ctx()) fz_drop_pixmap(ctx, impl_->pix);
        impl_->pix = nullptr;
    }
    delete impl_;
    impl_ = nullptr;
}

bool PageBitmap::valid() const noexcept { return impl_ != nullptr && impl_->pix != nullptr; }

int PageBitmap::width() const noexcept {
    return valid() ? fz_pixmap_width(impl_->ctx(), impl_->pix) : 0;
}

int PageBitmap::height() const noexcept {
    return valid() ? fz_pixmap_height(impl_->ctx(), impl_->pix) : 0;
}

int PageBitmap::stride() const noexcept {
    return valid() ? fz_pixmap_stride(impl_->ctx(), impl_->pix) : 0;
}

const std::uint8_t* PageBitmap::samples() const noexcept {
    return valid() ? fz_pixmap_samples(impl_->ctx(), impl_->pix) : nullptr;
}

float PageBitmap::effective_scale() const noexcept {
    return impl_ ? impl_->scale : 0.0f;
}

// ---- Document ----

struct Document::Impl {
    std::shared_ptr<CtxHandle> ch;      // ctx 的共享所有权（见 CtxHandle 说明）
    fz_context*  ctx = nullptr;         // == ch->ctx，热路径上的快捷别名
    fz_document* doc = nullptr;
    int          page_count = 0;
    // 是否仍需密码。**只由 open()/authenticate() 维护、绝不在认证后再问 MuPDF**：
    // MuPDF 1.26.10 上，认证成功后调用 fz_needs_password() 会破坏文档的解密状态
    // （内容流静默解密失败 → 整页空白，见 ADR-041）。info() 因此只能读这个缓存值。
    int          needs_password = 0;
    std::string  last_error;

    ~Impl() { destroy(); }

    // 释放 MuPDF 资源。doc 必须先于 ctx 释放。fz_drop_* 不抛异常。
    void destroy() noexcept {
        if (doc && ctx) {
            fz_drop_document(ctx, doc);
            doc = nullptr;
        }
        // 若仍有 PageBitmap 存活，这里只是放下自己那一份所有权，ctx 延后释放
        ch.reset();
        ctx = nullptr;
        page_count = 0;
        needs_password = 0;
    }
};

namespace {

// 记录失败原因（截断到 1024 字节）。OOM 时直接终止进程属可接受后果。
// 注意：这里只接 std::string&，不接 Document::Impl& —— 模块并不会让自由函数
// 获得类的私有嵌套类型访问权（C2248），而把 Impl 暴露成公开类型会破坏 PIMPL 封装。
void set_error(std::string& dest, const char* msg) noexcept {
    if (!msg) { dest.clear(); return; }
    std::size_t n = 0;
    while (msg[n] != '\0' && n < kErrCap) ++n;
    dest.assign(msg, n);
}

// 创建并初始化一个 MuPDF 单线程上下文，写入 out（共享句柄持有所有权）。
//
// 之所以单独成函数、且用出参而不是返回值：
//   ① open() 体内不应出现任何带析构函数的东西（包括返回 shared_ptr 这种临时量），
//      否则 MSVC 会对 setjmp 报 C4611「setjmp 与 C++ 对象析构的交互不可移植」；
//      保留该警告可见，正是我们 setjmp 纪律的有效守卫（见 ADR-009）。
//   ② ctx 创建失败与后续 fz_try 的关注点分离。
bool create_context(std::shared_ptr<CtxHandle>& out) noexcept {
    std::shared_ptr<CtxHandle> ch;
    try {
        ch = std::make_shared<CtxHandle>();
    } catch (...) {
        return false;  // 控制块都分配不出来，进程已无意义
    }
    ch->ctx = fz_new_context(nullptr, nullptr, kStoreBytes);
    if (ch->ctx == nullptr) return false;  // ch 析构，ctx 为 nullptr 无需释放
    out = std::move(ch);
    return true;
}

}  // namespace

Document::Document() noexcept {
    // 极小分配；失败时所有方法都会走 NotOpen 分支
    impl_.reset(new (std::nothrow) Impl());
}

Document::~Document() = default;

Document::Document(Document&& other) noexcept : impl_(std::move(other.impl_)) {}

Document& Document::operator=(Document&& other) noexcept {
    if (this != &other) impl_ = std::move(other.impl_);
    return *this;
}

void Document::close() noexcept {
    if (impl_) impl_->destroy();
}

bool Document::is_open() const noexcept {
    return impl_ != nullptr && impl_->doc != nullptr;
}

std::string_view Document::last_error() const noexcept {
    return impl_ ? std::string_view(impl_->last_error) : std::string_view{};
}

DocError Document::open(const std::wstring& path) noexcept {
    close();
    if (!impl_) return DocError::Internal;
    impl_->last_error.clear();

    if (path.empty()) {
        set_error(impl_->last_error, "empty path");
        return DocError::NotFound;
    }
    if (!is_regular_file(path)) {
        set_error(impl_->last_error, "file not found or not a regular file");
        return DocError::NotFound;
    }

    Impl& s = *impl_;

    // ---- 1. 创建 MuPDF 上下文（单线程：locks = nullptr）----
    if (!create_context(s.ch)) {
        set_error(s.last_error, "failed to create MuPDF context");
        return DocError::Internal;
    }
    s.ctx = s.ch->ctx;

    // ---- 2. 打开文档 ----
    // 用 fz_open_file_w 走宽字符接口，避免非 ASCII 路径在 Windows 上踩 ANSI 代码页。
    // magic 用扩展名（含点）帮助 MuPDF 选择处理器；认不出时会自动退回内容嗅探。
    char magic[16] = {};
    extension_magic(path, magic);

    fz_stream*   stm = nullptr;
    fz_document* doc = nullptr;
    DocError result = DocError::Ok;
    char err[kErrCap] = {};

    fz_try(s.ctx) {
        fz_var(stm);
        fz_var(doc);
        fz_register_document_handlers(s.ctx);  // 可分配内存，故放在 fz_try 内
        stm = fz_open_file_w(s.ctx, path.c_str());
        doc = fz_open_document_with_stream(s.ctx, magic, stm);
        if (doc == nullptr)
            fz_throw(s.ctx, FZ_ERROR_GENERIC, "open failed: null document");
    }
    fz_always(s.ctx) {
        // 文档若需要流会自行 keep 一份引用，我们这里只释放自己的引用
        if (stm) fz_drop_stream(s.ctx, stm);
    }
    fz_catch(s.ctx) {
        doc = nullptr;
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    if (result != DocError::Ok) {
        s.destroy();
        set_error(s.last_error, err);
        return result;
    }
    s.doc = doc;

    // ---- 3. 让 MuPDF 报告它实际认出的格式（供后续判定与 UI 显示）----
    // 放在密码/页数之前：查询很便宜，且加密文档的 format 串照常可读。
    char fmt[64] = {};
    int  have_fmt = 0;
    fz_try(s.ctx) {
        fz_var(fmt);
        fz_var(have_fmt);
        have_fmt = fz_lookup_metadata(s.ctx, s.doc, FZ_META_FORMAT, fmt, sizeof fmt) >= 0 ? 1 : 0;
    }
    fz_catch(s.ctx) {
        have_fmt = 0;  // 查询失败不致命：退化为"不判定"
    }

    // 归档冒充单文档：zip 图集改名 .pdf/.epub 会被 CBZ 处理器接手，页数=图片条目数，
    // 内容类别与用户预期完全错位 —— 这是唯一必须拦的"有影响的"错配（ADR-016）。
    if (!accepts_archive(magic) && classify_format(fmt) == FormatFamily::Archive) {
        // 错误信息在 fz_try 之外组装，可以放心用 std::string
        std::string detail = "archive file (";
        detail += fmt;
        detail += ") opened as a document under extension ";
        detail += magic[0] ? magic : "(none)";
        detail += "; it is a zip/tar archive, not a single document";
        s.destroy();
        set_error(s.last_error, detail.c_str());
        return DocError::Mismatched;
    }

    // ---- 4. 加密检测 ----
    int needs_password = 0;
    fz_try(s.ctx) {
        fz_var(needs_password);
        needs_password = fz_needs_password(s.ctx, s.doc);
    }
    fz_catch(s.ctx) {
        needs_password = 0;  // 探测失败不致命，下一步 count_pages 会再兜一次
    }
    if (needs_password) {
        s.needs_password = 1;  // 缓存：认证后不得再问 MuPDF（ADR-041）
        set_error(s.last_error, "document is encrypted and requires a password");
        return DocError::NeedsPassword;
    }

    // ---- 5. 页数（顺带验证内容真的可读，可挡住"MuPDF 也认不出"的文件）----
    // 注意：这里挡不住"改名的图片/文档"—— 那些是照内容正常打开的（ADR-016）；
    // 纯文本改名 .pdf 会在这一步失败（MuPDF 无纯文本处理器）。
    int pages = 0;
    fz_try(s.ctx) {
        fz_var(pages);
        pages = fz_count_pages(s.ctx, s.doc);
    }
    fz_catch(s.ctx) {
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }
    if (result == DocError::NeedsPassword) {
        set_error(s.last_error, err);  // 保留打开状态，允许上层调 authenticate
        return DocError::NeedsPassword;
    }
    if (result != DocError::Ok) {
        s.destroy();
        set_error(s.last_error, err);
        return result;
    }
    if (pages <= 0) {
        s.destroy();
        // .cbz 这类归档本来就该按"图片条目=页"来读；若一个图片页都没有，
        // 说明这个压缩包不是漫画。给 Mismatched 而不是"损坏"，UI 才说得清原因。
        if (accepts_archive(magic) && classify_format(fmt) == FormatFamily::Archive) {
            set_error(s.last_error,
                      "archive contains no displayable image pages (comic archive expected)");
            return DocError::Mismatched;
        }
        set_error(s.last_error, "document contains no pages");
        return DocError::Corrupt;
    }

    s.page_count = pages;
    return DocError::Ok;
}

DocError Document::authenticate(std::string_view utf8_password) noexcept {
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;
    Impl& s = *impl_;

    // 密钥拷进 POD 缓冲：MuPDF 需要 NUL 结尾，且不能有 C++ 对象跨越 fz_try
    char password[256] = {};
    const std::size_t n = utf8_password.size() < sizeof(password) - 1
                              ? utf8_password.size()
                              : sizeof(password) - 1;
    if (n > 0) std::memcpy(password, utf8_password.data(), n);

    int ok = 0;
    DocError result = DocError::Ok;
    char err[kErrCap] = {};

    // 注意：认证成功后**不得**再调用 fz_needs_password() —— MuPDF 1.26.10 上会破坏
    // 解密状态（内容流静默失败 → 整页空白，见 ADR-041）。需要该信息时读 s.needs_password。
    fz_try(s.ctx) {
        fz_var(ok);
        ok = fz_authenticate_password(s.ctx, s.doc, password);
    }
    fz_catch(s.ctx) {
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    if (result != DocError::Ok) {
        set_error(s.last_error, err);
        return result;
    }
    if (ok == 0) {
        set_error(s.last_error, "invalid password");
        return DocError::NeedsPassword;
    }
    s.needs_password = 0;  // 已解锁（ok 非 0 = 用户或所有者口令通过）

    // 解锁后重新取页数
    int pages = 0;
    fz_try(s.ctx) {
        fz_var(pages);
        pages = fz_count_pages(s.ctx, s.doc);
    }
    fz_catch(s.ctx) {
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }
    if (result != DocError::Ok) {
        set_error(s.last_error, err);
        return result;
    }
    if (pages <= 0) {
        set_error(s.last_error, "document contains no pages");
        return DocError::Corrupt;
    }

    s.page_count = pages;
    s.last_error.clear();
    return DocError::Ok;
}

DocError Document::info(DocumentInfo& out) const noexcept {
    out = DocumentInfo{};
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;

    Impl& s = *impl_;

    int      pages = 0;
    int      needs_password = 0;
    int      has_outline = 0;
    int      can_copy = 1;
    int      can_print = 1;
    fz_page* first = nullptr;
    fz_rect  bounds{};
    fz_outline* outline = nullptr;
    char     title[512] = {};
    char     format[64] = {};
    DocError result = DocError::Ok;
    char     err[kErrCap] = {};

    fz_try(s.ctx) {
        fz_var(pages);
        fz_var(needs_password);
        fz_var(has_outline);
        fz_var(can_copy);
        fz_var(can_print);
        fz_var(first);
        fz_var(bounds);
        fz_var(outline);
        fz_var(title);
        fz_var(format);

        pages = fz_count_pages(s.ctx, s.doc);
        // 读缓存值而非调 fz_needs_password()：认证后再问 MuPDF 会破坏解密（ADR-041）
        needs_password = s.needs_password;
        if (pages <= 0) fz_throw(s.ctx, FZ_ERROR_FORMAT, "document contains no pages");

        // 权限位：非加密文档恒为允许；查询失败不应影响打开，故各自独立容错由返回值表达。
        can_copy = fz_has_permission(s.ctx, s.doc, FZ_PERMISSION_COPY);
        can_print = fz_has_permission(s.ctx, s.doc, FZ_PERMISSION_PRINT);

        if (fz_lookup_metadata(s.ctx, s.doc, FZ_META_INFO_TITLE, title, sizeof title) < 0)
            title[0] = '\0';
        if (fz_lookup_metadata(s.ctx, s.doc, FZ_META_FORMAT, format, sizeof format) < 0)
            format[0] = '\0';

        outline = fz_load_outline(s.ctx, s.doc);
        has_outline = outline != nullptr ? 1 : 0;
        if (outline) {
            fz_drop_outline(s.ctx, outline);
            outline = nullptr;
        }

        first = fz_load_page(s.ctx, s.doc, 0);
        bounds = fz_bound_page(s.ctx, first);
    }
    fz_always(s.ctx) {
        if (outline) fz_drop_outline(s.ctx, outline);
        if (first) fz_drop_page(s.ctx, first);
    }
    fz_catch(s.ctx) {
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    if (result != DocError::Ok) {
        set_error(s.last_error, err);
        return result;
    }

    out.page_count = pages;
    out.needs_password = needs_password != 0;
    out.has_outline = has_outline != 0;
    out.can_copy = can_copy != 0;
    out.can_print = can_print != 0;
    out.page_width_pt = bounds.x1 - bounds.x0;
    out.page_height_pt = bounds.y1 - bounds.y0;
    out.title.assign(title);    // C++ 对象操作一律放在 fz_try 之外
    out.format.assign(format);

    // ---- 逐页尺寸 ----
    // 放在 fz_try 之外：这里要构造 std::vector（longjmp 不会析构它）。
    // 页 0 直接复用上面已取到的 bounds，省一次 load_page。
    // 探测失败的页留 {0,0}，上层按首页尺寸回退 —— 画布对 0 尺寸页已有回退逻辑。
    // 页数超过上限则整表留空（上层回退统一尺寸），理由见 kMaxSizeProbePages。
    if (pages > 0 && pages <= kMaxSizeProbePages) {
        out.page_sizes.assign(static_cast<std::size_t>(pages), PageSize{});
        if (bounds.x1 > bounds.x0 && bounds.y1 > bounds.y0)
            out.page_sizes[0] = PageSize{ bounds.x1 - bounds.x0, bounds.y1 - bounds.y0 };
        for (int i = 1; i < pages; ++i) {
            float w = 0.0f, h = 0.0f;
            if (bound_page_pt(s.ctx, s.doc, i, w, h))
                out.page_sizes[static_cast<std::size_t>(i)] = PageSize{ w, h };
        }
    }
    return DocError::Ok;
}

DocError Document::outline(std::vector<OutlineItem>& out) const noexcept {
    out.clear();
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;
    Impl& s = *impl_;

    // 缓冲在 fz_try 之前分配：raw 指针（POD）跨 fz_try 是安全的（无析构函数可被 longjmp 跳过）。
    // 不用 std::unique_ptr，是为了严格贴合"fz_try 内不得出现需析构的 C++ 对象"这条纪律
    // 的强解读：这里连跨块存活的对象也不引入。
    OutlinePOD* buf = new (std::nothrow) OutlinePOD[kMaxOutlineNodes];
    if (buf == nullptr) return DocError::Internal;

    fz_outline* root = nullptr;
    int count = 0;
    int truncated = 0;
    DocError result = DocError::Ok;
    char err[kErrCap] = {};

    fz_try(s.ctx) {
        fz_var(root);
        fz_var(count);
        fz_var(truncated);
        root = fz_load_outline(s.ctx, s.doc);  // 无目录返回 nullptr（不是错误）
        if (root != nullptr)
            walk_outline(s.ctx, s.doc, root, 0, buf, kMaxOutlineNodes, count, truncated);
    }
    fz_always(s.ctx) {
        if (root) fz_drop_outline(s.ctx, root);
    }
    fz_catch(s.ctx) {
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    if (result != DocError::Ok) {
        delete[] buf;
        set_error(s.last_error, err);
        return result;
    }

    // C++ 对象一律在 fz_try 之外构造
    out.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        OutlineItem it;
        it.title.assign(buf[i].title);
        it.page = buf[i].page;
        it.depth = buf[i].depth;
        out.push_back(std::move(it));
    }
    delete[] buf;

    s.last_error.clear();
    return DocError::Ok;
}

DocError Document::render_page(int index, float scale, PageBitmap& out,
                               int max_dimension, int rotation_deg,
                               ColorMode color_mode) noexcept {
    out.reset();
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;
    if (index < 0 || !(scale > 0.0f)) return DocError::Internal;
    if (max_dimension < 64) max_dimension = 64;

    // 旋转归一到 0/90/180/270（接受负值与 >360）
    int rot = rotation_deg % 360;
    if (rot < 0) rot += 360;
    rot = (rot / 90) * 90;

    Impl& s = *impl_;

    fz_page*   page = nullptr;
    fz_pixmap* pix = nullptr;
    fz_device* dev = nullptr;
    float      used = scale;
    int        mode = static_cast<int>(color_mode);
    DocError   result = DocError::Ok;
    char       err[kErrCap] = {};

    fz_try(s.ctx) {
        fz_var(page);
        fz_var(pix);
        fz_var(dev);
        fz_var(used);

        page = fz_load_page(s.ctx, s.doc, index);
        const fz_rect bounds = fz_bound_page(s.ctx, page);

        // 尺寸钳制按**旋转后**包围盒：90/270 时宽高互换。若按未旋转的 bounds 钳制，
        // 一张 8000×100 的横幅旋转后会得到 8000 高，仍然爆内存。
        const fz_rect rotated = fz_transform_rect(bounds, fz_rotate(static_cast<float>(rot)));
        used = clamp_scale(rotated, scale, max_dimension);

        // 旋转与缩放都是线性变换，且缩放是等比的（标量×单位矩阵）⇒ 二者可交换，顺序无关。
        const fz_matrix ctm = fz_pre_rotate(fz_scale(used, used), static_cast<float>(rot));
        const fz_irect  bbox = fz_round_rect(fz_transform_rect(bounds, ctm));

        // RGB + alpha ⇒ 4 分量 RGBA8，可直接上传 DXGI_FORMAT_R8G8B8A8_UNORM
        pix = fz_new_pixmap_with_bbox(s.ctx, fz_device_rgb(s.ctx), bbox, nullptr, 1);
        // 不透明白底：所有分量填 0xFF。之后页面内容以 SRC_OVER 合成，
        // 因目标 alpha 恒为 255，结果 alpha 也恒为 255，无需再做反预乘。
        fz_clear_pixmap_with_value(s.ctx, pix, 0xFF);

        dev = fz_new_draw_device(s.ctx, ctm, pix);
        fz_run_page(s.ctx, page, dev, fz_identity, nullptr);
        // fz_drop_device 不会隐式关闭设备，必须显式 close（Lilith 原版漏了这步）
        fz_close_device(s.ctx, dev);
        fz_drop_device(s.ctx, dev);
        dev = nullptr;

        // 配色变换：设备已关闭、内容全部落盘后再做（POD 运算，无 C++ 对象）。
        apply_color_transform(s.ctx, pix, mode);

        fz_drop_page(s.ctx, page);
        page = nullptr;
    }
    fz_always(s.ctx) {
        // 出错路径：dev 未 close 就 drop，MuPDF 会给出 "dropping unclosed device" 警告，
        // 但不会泄漏 —— fz_always 内不得调用可能抛异常的 fz_close_device。
        if (dev) fz_drop_device(s.ctx, dev);
        if (page) fz_drop_page(s.ctx, page);
    }
    fz_catch(s.ctx) {
        if (pix) {
            fz_drop_pixmap(s.ctx, pix);
            pix = nullptr;
        }
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    if (result != DocError::Ok) {
        set_error(s.last_error, err);
        return result;
    }

    // 防御性校验：分量数不是 4 说明 MuPDF 行为与预期不符，
    // 宁可报错也不能把格式不符的缓冲交给纹理层。
    if (fz_pixmap_components(s.ctx, pix) != kComponents) {
        fz_drop_pixmap(s.ctx, pix);
        set_error(s.last_error, "unexpected pixmap component count");
        return DocError::Internal;
    }

    auto* impl = new (std::nothrow) PageBitmap::Impl{ s.ch, pix, used };
    if (impl == nullptr) {
        fz_drop_pixmap(s.ctx, pix);
        set_error(s.last_error, "out of memory allocating page bitmap");
        return DocError::Internal;
    }

    out = PageBitmap(impl);
    return DocError::Ok;
}

}  // namespace lr
