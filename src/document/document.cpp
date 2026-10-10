// document.cpp — lilithreader.document 的实现单元
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

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

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

// 扩展名 → 格式族的表在 utils（ADR-093）：document 与闸门白名单共用同一出处。
import lilithreader.utils;

namespace lr {
namespace {

// ---- 常量 ----

// MuPDF 内部资源存储上限。留一份解码缓存给它，同时保证进程内存有界。
// 页面级缓存不依赖它（由 render 层自己按字节预算管理）。
constexpr std::size_t kStoreBytes = 128u << 20;  // 128 MB

// pixmap 分量数：RGB + alpha ⇒ RGBA8
constexpr int kComponents = 4;

// 错误信息缓冲区（POD，可安全跨越 longjmp）
constexpr std::size_t kErrCap = 1024;

// 单页搜索命中上限：防止"文档里全是 e"这类病态查询把命中表撑到几十万条。
// 200 条/页 × 数千页仍可能有几十万条，故 render 层另设**全局**上限（见 render.cpp）。
constexpr int kMaxSearchHitsPerPage = 200;

// 结果列表里的上下文片段上限（字节）。按 UTF-8 边界截断，避免半个汉字。
constexpr std::size_t kSnippetCap = 160;

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
//
// FORMAT 分支也要看消息：MuPDF 把"压缩方式不受支持"（如 MOBI 的 HUFF/CDIC）也抛成
// FZ_ERROR_FORMAT，若一律归 Corrupt，用户会看到"文件已损坏"，而文件其实完好、只是这个
// 压缩没实现 —— 属于"打开失败文案不实"。命中特征词时改判 Unsupported（"暂不支持的格式"）。
DocError classify(fz_context* ctx) noexcept {
    switch (fz_caught(ctx)) {
    case FZ_ERROR_UNSUPPORTED: return DocError::Unsupported;
    case FZ_ERROR_LIMIT:       return DocError::TooLarge;
    case FZ_ERROR_FORMAT:
    case FZ_ERROR_SYNTAX: {
        const char* msg = fz_caught_message(ctx);
        if (msg && std::strstr(msg, "unknown compression")) return DocError::Unsupported;
        return DocError::Corrupt;
    }
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

// ---- 页面配色（按内容分层见 ADR-067）----
//
// 配色变换 = 一张逐通道 LUT，**只作用于"纸墨层"**（背景/文字/矢量/单色蒙版图），
// 照片与插图由分流设备留在另一层、原样叠回（见下面的 SplitDevice）。
//
//   mode 1（深色纸张）：**柔化**的暗色映射，而非纯黑底白字（纯反色刺眼）：
//                   白 → 深暖灰 #1F1D1B，黑 → 浅暖灰 #D7D5D3（= 纸色逐通道 + 184），
//                   中间调线性过渡；逐通道 LUT，等价于"先反相再把对比度压到 [暗,亮] 区间"。
//                   两端点**必须等斜率**，否则"越亮的那一端越暖"（见下面 kLight 处的说明）。
//   mode 2（暖色）：RGB 经 LUT 线性映射 黑→#2B2318、白→#F6EEDC（暖色纸张）
//
// 为什么不用 fz_invert_pixmap / fz_tint_pixmap：这两者面向 RGB/Gray，对 **RGBA（带 alpha）
// 的 4 分量 pixmap** 的行为不在公开契约里（tint 明确只写 RGB/BGR/Gray）。本函数只处理
// 已知的 RGBA8（n==4），逐像素改 RGB、**保留 alpha**，行为完全可控且可单测。
// 只做 POD 运算、不构造 C++ 对象，故可安全放在 fz_try 内调用。
void apply_scheme_lut(fz_context* ctx, fz_pixmap* pix, int mode) noexcept {
    if (pix == nullptr || mode == 0) return;
    if (fz_pixmap_components(ctx, pix) != kComponents) return;  // 只处理 RGBA8
    const int w = fz_pixmap_width(ctx, pix);
    const int h = fz_pixmap_height(ctx, pix);
    const int stride = fz_pixmap_stride(ctx, pix);
    unsigned char* p = fz_pixmap_samples(ctx, pix);
    if (p == nullptr || w <= 0 || h <= 0 || stride <= 0) return;

    if (mode == 1) {
        // 两个端点必须让**逐通道斜率相同**（kLight = kDark + 184），否则"越亮的那一端越暖"：
        // 原端点 #D8D4CE 的 r−b 差是 10，而纸面 #1F1D1B 只有 4 —— 正文比纸面暖 2.5 倍，
        // 深色纸张下正文于是发黄（与 ADR-068"亮、面积小、对比强的元素，一点色相就非常显眼"同一条规律）。
        // 等斜率后，整个映射等价于"按亮度一个标量 + 一个**恒定**暖偏"：两端与中间调暖度一致。
        constexpr unsigned char kDark[3]  = { 0x1F, 0x1D, 0x1B };  // 白 → 深暖灰
        constexpr unsigned char kLight[3] = { 0xD7, 0xD5, 0xD3 };  // 黑 → 浅暖灰（= kDark + 184）
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

// ---- 内容分流设备（ADR-067）----
//
// 页面绘制是"把页对象喂给一个 fz_device"（fz_run_page）。这里在页面与真正的 draw device
// 之间夹一个**转发设备**，按"这一笔是不是照片/插图"决定本层放不放行：
//
//   纸墨层（kSplitInk）  ：放行一切，**拦下 fill_image**（照片位置留白底）；
//   图像层（kSplitImage）：只放行 fill_image，并以 dim 降 alpha 压暗后叠回。
//
// 于是纸墨与照片分两遍绘到**同一张 pixmap**：先纸墨、套配色 LUT、再把照片叠回去 ——
// 照片因此不参与配色变换。这解决了"整张位图套一个 LUT"分不清纸墨与照片、
// 把照片一起变成负片（深色纸张）或一起染黄（暖色）的根因。
//
// 为什么自己写转发函数：MuPDF 没有内置的"过滤设备"。fz_device 的回调表虽是公开结构，
// 但**派生设备的状态就存在基础结构之后**（回调里把 dev 直接 cast 成自己的派生类型），
// 所以"拷贝一份回调表"会读到越界内存；只能逐个显式转发。每个转发函数只做两件事：
// 判断本层是否放行、放行就调对应的公开 fz_* 包装（包装内部对 NULL 回调有保护，
// 故 inner 未实现的调用自然成为空操作）。
//
// 分类规则（唯一需要拍板的一条）：只有 **fill_image（真正的位图绘制）** 算图像层；
// **fill_image_mask（单色蒙版图，如 logo/图标）算墨迹** —— 它画出来的是"墨色形状"，
// 深色纸张模式下必须跟着变浅，否则黑白 logo 会是一块白。
// 只裁剪不涂色的调用（clip_* / pop_clip / begin_mask / end_mask）与容器类调用
// （group / tile / layer / structure）**两层都转发**，保证两层看到同一套裁剪与容器状态。
// 蒙版内容（begin_mask 与 end_mask 之间）两层都放行：它不进画面、只是各自栅格化一份，
// 若只放行一层，另一层的软蒙版会是空的、被它蒙住的照片会整块消失。
enum { kSplitInk = 0, kSplitImage = 1 };

// 图像层压暗：照片以 (1 - dim) 的比例与纸面混合，避免暗环境里照片"发光"。
// 纸墨层不用它（纸墨是要被 LUT 整体重映射的）；整页扫描件也不用（见下面 scanned 分支）。
constexpr float kImageDim = 0.88f;

// 判定"整页扫描件"的两个阈值（两个条件同时成立才算，见 render_page）：
//   kScanCoverageFloor：图像覆盖面积 / 页面面积 的下限 —— 少于此比例说明页面还有别的版面。
//   kPaperBlankFloor  ：纸墨层墨迹覆盖率的上限 —— 高于它说明页面上有真正的文字/矢量，
//                       那种情况（如"整页底图 + 正文"的杂志封面）应当只变换文字、保留底图。
// 两个阈值都是抽样/几何估算，不追求精确；目的是把"扫描书"与"有插图的正文页"分开。
constexpr double kScanCoverageFloor = 0.55;
constexpr double kPaperBlankFloor = 0.005;

struct SplitDevice {
    fz_device  base;    // 必须是首成员：回调里按派生类型读回本结构
    fz_device* inner;   // 下游真正的 draw device
    int        pass;    // kSplitInk / kSplitImage
    int        mask_depth;  // >0 表示正在 begin_mask/end_mask 之间（此区间两层都放行）
    float      dim;     // 图像层的不透明度系数
    int        images;  // 本层放行过的图像数（调用方据此判断页面是否只有图像）
    float      image_area;  // 放行过的图像覆盖面积之和（像素²）：单位方格经 ctm 后的 |det|
};

// 本层是否放行该调用。is_image 只在 fill_image 上为真。
inline bool split_allow(const SplitDevice* d, bool is_image) noexcept {
    if (d->mask_depth > 0) return true;
    return d->pass == kSplitImage ? is_image : !is_image;
}

inline SplitDevice* as_split(fz_device* dev) noexcept {
    return reinterpret_cast<SplitDevice*>(dev);
}

// 关闭与释放。两条纪律，都是踩过坑才写下的：
//   1. **不碰 inner**：inner 由调用方显式 close/drop，避免双重关闭与双重释放；
//   2. **不释放自己**：设备结构由 `fz_drop_device` 统一回收（它在调用 drop_device 之后
//      自己 fz_free 掉整块内存）。这里再 fz_free 一次就是双重释放，实测症状是
//      0xC0000374 堆损坏 —— 而且**只在 drop 那一刻炸**，与页面内容无关，极易误判成渲染 bug。
void split_close(fz_context*, fz_device*) noexcept {}

void split_drop(fz_context*, fz_device*) noexcept {}

void split_fill_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd,
                     fz_matrix ctm, fz_colorspace* cs, const float* color, float alpha,
                     fz_color_params cp) {
    SplitDevice* d = as_split(dev);
    if (split_allow(d, false)) fz_fill_path(ctx, d->inner, path, even_odd, ctm, cs, color, alpha, cp);
}

void split_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path,
                       const fz_stroke_state* stroke, fz_matrix ctm, fz_colorspace* cs,
                       const float* color, float alpha, fz_color_params cp) {
    SplitDevice* d = as_split(dev);
    if (split_allow(d, false)) fz_stroke_path(ctx, d->inner, path, stroke, ctm, cs, color, alpha, cp);
}

void split_clip_path(fz_context* ctx, fz_device* dev, const fz_path* path, int even_odd,
                     fz_matrix ctm, fz_rect scissor) {
    SplitDevice* d = as_split(dev);
    fz_clip_path(ctx, d->inner, path, even_odd, ctm, scissor);
}

void split_clip_stroke_path(fz_context* ctx, fz_device* dev, const fz_path* path,
                            const fz_stroke_state* stroke, fz_matrix ctm, fz_rect scissor) {
    SplitDevice* d = as_split(dev);
    fz_clip_stroke_path(ctx, d->inner, path, stroke, ctm, scissor);
}

void split_fill_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                     fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    SplitDevice* d = as_split(dev);
    if (split_allow(d, false)) fz_fill_text(ctx, d->inner, text, ctm, cs, color, alpha, cp);
}

void split_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text,
                       const fz_stroke_state* stroke, fz_matrix ctm, fz_colorspace* cs,
                       const float* color, float alpha, fz_color_params cp) {
    SplitDevice* d = as_split(dev);
    if (split_allow(d, false)) fz_stroke_text(ctx, d->inner, text, stroke, ctm, cs, color, alpha, cp);
}

void split_clip_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm,
                     fz_rect scissor) {
    SplitDevice* d = as_split(dev);
    fz_clip_text(ctx, d->inner, text, ctm, scissor);
}

void split_clip_stroke_text(fz_context* ctx, fz_device* dev, const fz_text* text,
                            const fz_stroke_state* stroke, fz_matrix ctm, fz_rect scissor) {
    SplitDevice* d = as_split(dev);
    fz_clip_stroke_text(ctx, d->inner, text, stroke, ctm, scissor);
}

void split_ignore_text(fz_context* ctx, fz_device* dev, const fz_text* text, fz_matrix ctm) {
    SplitDevice* d = as_split(dev);
    fz_ignore_text(ctx, d->inner, text, ctm);
}

void split_fill_shade(fz_context* ctx, fz_device* dev, fz_shade* shd, fz_matrix ctm, float alpha,
                      fz_color_params cp) {
    SplitDevice* d = as_split(dev);
    if (split_allow(d, false)) fz_fill_shade(ctx, d->inner, shd, ctm, alpha, cp);
}

// 唯一归入图像层的一笔。叠回时按 dim 降不透明度，得到"保留色彩但压暗"的效果。
void split_fill_image(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm, float alpha,
                      fz_color_params cp) {
    SplitDevice* d = as_split(dev);
    // 统计**先于**放行判断：计数回答的是"这一页画了几笔图"（纸墨层要为它留白、
    // 还要据此判断是不是整页扫描件），与"本层放不放行"是两件事。
    ++d->images;
    // 单位方格经 ctm 仿射变换后的面积 = |det|（cm 里 a·d − b·c）。多次绘制会累加，
    // 用于判断"这一页是不是就一张图"（整页扫描件，见 render_page 的 scanned）。
    d->image_area += std::fabs(ctm.a * ctm.d - ctm.b * ctm.c);
    if (!split_allow(d, true)) return;
    const float a = (d->pass == kSplitImage && d->mask_depth == 0) ? alpha * d->dim : alpha;
    fz_fill_image(ctx, d->inner, img, ctm, a, cp);
}

void split_fill_image_mask(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm,
                           fz_colorspace* cs, const float* color, float alpha, fz_color_params cp) {
    SplitDevice* d = as_split(dev);
    if (split_allow(d, false)) fz_fill_image_mask(ctx, d->inner, img, ctm, cs, color, alpha, cp);
}

void split_clip_image_mask(fz_context* ctx, fz_device* dev, fz_image* img, fz_matrix ctm,
                           fz_rect scissor) {
    SplitDevice* d = as_split(dev);
    fz_clip_image_mask(ctx, d->inner, img, ctm, scissor);
}

void split_pop_clip(fz_context* ctx, fz_device* dev) {
    fz_pop_clip(ctx, as_split(dev)->inner);
}

void split_begin_mask(fz_context* ctx, fz_device* dev, fz_rect area, int luminosity,
                      fz_colorspace* cs, const float* bc, fz_color_params cp) {
    SplitDevice* d = as_split(dev);
    ++d->mask_depth;
    fz_begin_mask(ctx, d->inner, area, luminosity, cs, bc, cp);
}

void split_end_mask(fz_context* ctx, fz_device* dev, fz_function* fn) {
    SplitDevice* d = as_split(dev);
    if (d->mask_depth > 0) --d->mask_depth;
    fz_end_mask_tr(ctx, d->inner, fn);
}

void split_begin_group(fz_context* ctx, fz_device* dev, fz_rect area, fz_colorspace* cs,
                       int isolated, int knockout, int blendmode, float alpha) {
    fz_begin_group(ctx, as_split(dev)->inner, area, cs, isolated, knockout, blendmode, alpha);
}

void split_end_group(fz_context* ctx, fz_device* dev) {
    fz_end_group(ctx, as_split(dev)->inner);
}

int split_begin_tile(fz_context* ctx, fz_device* dev, fz_rect area, fz_rect view, float xstep,
                     float ystep, fz_matrix ctm, int id, int doc_id) {
    return fz_begin_tile_tid(ctx, as_split(dev)->inner, area, view, xstep, ystep, ctm, id, doc_id);
}

void split_end_tile(fz_context* ctx, fz_device* dev) {
    fz_end_tile(ctx, as_split(dev)->inner);
}

void split_render_flags(fz_context* ctx, fz_device* dev, int set, int clear) {
    fz_render_flags(ctx, as_split(dev)->inner, set, clear);
}

void split_set_default_colorspaces(fz_context* ctx, fz_device* dev, fz_default_colorspaces* cs) {
    fz_set_default_colorspaces(ctx, as_split(dev)->inner, cs);
}

void split_begin_layer(fz_context* ctx, fz_device* dev, const char* name) {
    fz_begin_layer(ctx, as_split(dev)->inner, name);
}

void split_end_layer(fz_context* ctx, fz_device* dev) {
    fz_end_layer(ctx, as_split(dev)->inner);
}

void split_begin_structure(fz_context* ctx, fz_device* dev, fz_structure standard, const char* raw,
                           int idx) {
    fz_begin_structure(ctx, as_split(dev)->inner, standard, raw, idx);
}

void split_end_structure(fz_context* ctx, fz_device* dev) {
    fz_end_structure(ctx, as_split(dev)->inner);
}

void split_begin_metatext(fz_context* ctx, fz_device* dev, fz_metatext meta, const char* text) {
    fz_begin_metatext(ctx, as_split(dev)->inner, meta, text);
}

void split_end_metatext(fz_context* ctx, fz_device* dev) {
    fz_end_metatext(ctx, as_split(dev)->inner);
}

// 建一个分流设备。可能抛异常（分配失败），故必须在 fz_try 内调用。
//
// 这里显式清零整份基础结构（不依赖 `fz_new_device_of_size` 是否已清零）：`container` /
// `container_len` / `d1_rect` 这些字段本项目不用，但 `fz_drop_device` 回收时会顺着
// `container` 走一遍，留脏值就等于把野指针交回去。
fz_device* split_new(fz_context* ctx, fz_device* inner, int pass, float dim) {
    // 注意用 reinterpret_cast：SplitDevice **不是**从 fz_device 继承的（首成员是 base），
    // 只是按"派生设备"惯例把额外状态接在基础结构之后。
    SplitDevice* d = reinterpret_cast<SplitDevice*>(
        fz_new_device_of_size(ctx, static_cast<int>(sizeof(SplitDevice))));
    std::memset(&d->base, 0, sizeof(d->base));
    d->base.refs = 1;
    d->base.hints = inner->hints;
    d->base.flags = inner->flags;
    d->base.close_device = split_close;
    d->base.drop_device = split_drop;
    d->base.fill_path = split_fill_path;
    d->base.stroke_path = split_stroke_path;
    d->base.clip_path = split_clip_path;
    d->base.clip_stroke_path = split_clip_stroke_path;
    d->base.fill_text = split_fill_text;
    d->base.stroke_text = split_stroke_text;
    d->base.clip_text = split_clip_text;
    d->base.clip_stroke_text = split_clip_stroke_text;
    d->base.ignore_text = split_ignore_text;
    d->base.fill_shade = split_fill_shade;
    d->base.fill_image = split_fill_image;
    d->base.fill_image_mask = split_fill_image_mask;
    d->base.clip_image_mask = split_clip_image_mask;
    d->base.pop_clip = split_pop_clip;
    d->base.begin_mask = split_begin_mask;
    d->base.end_mask = split_end_mask;
    d->base.begin_group = split_begin_group;
    d->base.end_group = split_end_group;
    d->base.begin_tile = split_begin_tile;
    d->base.end_tile = split_end_tile;
    d->base.render_flags = split_render_flags;
    d->base.set_default_colorspaces = split_set_default_colorspaces;
    d->base.begin_layer = split_begin_layer;
    d->base.end_layer = split_end_layer;
    d->base.begin_structure = split_begin_structure;
    d->base.end_structure = split_end_structure;
    d->base.begin_metatext = split_begin_metatext;
    d->base.end_metatext = split_end_metatext;
    d->inner = inner;
    d->pass = pass;
    d->mask_depth = 0;
    d->dim = dim;
    d->images = 0;
    d->image_area = 0.0f;
    return &d->base;
}

// 纸墨层是否"近乎空白"：抽样统计非纸色（白）像素的占比。整页扫描件为真。
bool paper_is_blank(fz_context* ctx, fz_pixmap* pix) noexcept {
    const int w = fz_pixmap_width(ctx, pix);
    const int h = fz_pixmap_height(ctx, pix);
    const int stride = fz_pixmap_stride(ctx, pix);
    const unsigned char* p = fz_pixmap_samples(ctx, pix);
    if (p == nullptr || w <= 0 || h <= 0 || stride <= 0) return false;
    constexpr int kStep = 2;          // 隔点抽样：分辨"整页扫描"与"少量文字"足够，代价 1/4
    constexpr int kInkDelta = 12;     // 与纸色（255）的差超过它才算墨
    long long total = 0;
    long long ink = 0;
    for (int y = 0; y < h; y += kStep) {
        const unsigned char* row = p + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride);
        for (int x = 0; x < w; x += kStep) {
            const unsigned char* q = row + static_cast<std::size_t>(x) * 4;
            if (255 - q[0] > kInkDelta || 255 - q[1] > kInkDelta || 255 - q[2] > kInkDelta) ++ink;
            ++total;
        }
    }
    return total == 0 || static_cast<double>(ink) < kPaperBlankFloor * static_cast<double>(total);
}

// ---- 目录遍历 ----
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
// 格式族枚举与"扩展名 → 族"的表**唯一出处是 utils.ixx**（ADR-093）；此处只做
// "MuPDF 上报串 → 族"的归类。

lr::FormatFamily classify_format(const char* fmt) noexcept {
    if (fmt == nullptr || fmt[0] == '\0') return lr::FormatFamily::Unknown;
    if (std::strncmp(fmt, "PDF", 3) == 0) return lr::FormatFamily::Pdf;
    if (std::strcmp(fmt, "EPUB") == 0) return lr::FormatFamily::Epub;
    if (std::strncmp(fmt, "FictionBook", 11) == 0) return lr::FormatFamily::Fb2;
    if (std::strcmp(fmt, "XPS") == 0) return lr::FormatFamily::Xps;
    if (std::strcmp(fmt, "zip") == 0 || std::strcmp(fmt, "tar") == 0)
        return lr::FormatFamily::Archive;  // CBZ 上报的是归档格式名
    if (std::strcmp(fmt, "Image") == 0) return lr::FormatFamily::Image;
    return lr::FormatFamily::Unknown;  // 不认识的上报值 ⇒ 不判定
}

// 扩展名所暗示的格式族（ext 为小写含点，取自 extension_magic）。表在 utils.ixx，
// 与闸门白名单**同一出处**（ADR-093），不再各自维护两份。
bool accepts_archive(const char* ext) noexcept {
    return ext != nullptr && lr::ext_family_of(ext) == lr::FormatFamily::Archive;
}

// 把 string_view 拷进固定缓冲（截断 + NUL 结尾），避免在 noexcept 接口里分配内存
void copy_truncated(std::string_view src, char* dst, std::size_t cap) noexcept {
    if (cap == 0) return;
    std::size_t n = src.size() < cap - 1 ? src.size() : cap - 1;
    if (n > 0) std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
}

// ---- MuPDF 边界函数（ADR-083）----
//
// 文件头铁律第 1 条：任何 fz_* 调用都必须被 fz_try/fz_catch 包住。资源属性读取与资源释放
// 集中到下面这几个边界函数里，其余代码只经它们接触 MuPDF 的资源对象 —— 于是"longjmp 不
// 穿透 C++ 生命周期"成为**结构保证**，而不是"这些函数恰好不抛"的经验假设。
// 块内只出现 POD（PixmapAttrs / 裸指针），符合铁律第 1 条对 fz_try 体的约束。

// pixmap 属性快照（POD，可安全跨越 longjmp）。
struct PixmapAttrs {
    int                  w = 0;
    int                  h = 0;
    int                  stride = 0;
    int                  components = 0;
    const unsigned char* samples = nullptr;
};

// 一次性读出 pixmap 的全部属性（构造 PageBitmap 时调用）。失败归零。
PixmapAttrs pixmap_attrs(fz_context* ctx, fz_pixmap* pix) noexcept {
    PixmapAttrs a{};
    if (ctx == nullptr || pix == nullptr) return a;
    fz_var(a);
    fz_try(ctx) {
        a.w = fz_pixmap_width(ctx, pix);
        a.h = fz_pixmap_height(ctx, pix);
        a.stride = fz_pixmap_stride(ctx, pix);
        a.components = fz_pixmap_components(ctx, pix);
        a.samples = fz_pixmap_samples(ctx, pix);
    }
    fz_catch(ctx) {
        a = PixmapAttrs{};
    }
    return a;
}

// 释放类边界：fz_drop_* 在 MuPDF 里约定不抛，但 ADR-083 要求它们同样被 fz_try/fz_catch 覆盖。
// 入参先取本地副本并置空调用方的指针：即便 drop 内部 longjmp，调用方也不会留下悬垂指针。
void drop_pixmap_safe(fz_context* ctx, fz_pixmap*& pix) noexcept {
    fz_pixmap* p = pix;
    pix = nullptr;
    if (ctx == nullptr || p == nullptr) return;
    fz_var(p);
    fz_try(ctx) { fz_drop_pixmap(ctx, p); }
    fz_catch(ctx) {}
}

void drop_stext_safe(fz_context* ctx, fz_stext_page*& st) noexcept {
    fz_stext_page* p = st;
    st = nullptr;
    if (ctx == nullptr || p == nullptr) return;
    fz_var(p);
    fz_try(ctx) { fz_drop_stext_page(ctx, p); }
    fz_catch(ctx) {}
}

void drop_document_safe(fz_context* ctx, fz_document*& doc) noexcept {
    fz_document* p = doc;
    doc = nullptr;
    if (ctx == nullptr || p == nullptr) return;
    fz_var(p);
    fz_try(ctx) { fz_drop_document(ctx, p); }
    fz_catch(ctx) {}
}

// 把 RGBA8 页面 pixmap 转成**不透明 RGB8**（导出 JPEG 用）。
//
// 为什么不用 fz_convert_pixmap：源有 alpha、目标无 alpha 时它直接抛
// "cannot drop alpha when converting pixmap"（实测，MuPDF 1.26）。而 JPEG 的 4 分量会被
// MuPDF 当作 **CMYK**，所以必须真的降到 3 分量。页面渲染时已用不透明白底清屏、alpha 恒为 255，
// 故"去 alpha"就是取 RGB 三通道，无需 alpha 合成。
// 返回的 pixmap 由调用方 drop；失败返回 nullptr。只做 POD 运算，可安全放在 fz_try 内。
fz_pixmap* rgb_pixmap_from_rgba(fz_context* ctx, fz_pixmap* src) noexcept {
    if (ctx == nullptr || src == nullptr) return nullptr;
    fz_pixmap* dst = nullptr;
    fz_var(dst);
    fz_try(ctx) {
        const int w = fz_pixmap_width(ctx, src);
        const int h = fz_pixmap_height(ctx, src);
        const int ss = fz_pixmap_stride(ctx, src);
        const unsigned char* sp = fz_pixmap_samples(ctx, src);
        if (w <= 0 || h <= 0 || ss <= 0 || sp == nullptr)
            fz_throw(ctx, FZ_ERROR_ARGUMENT, "bad source pixmap for rgb conversion");
        dst = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), fz_pixmap_bbox(ctx, src),
                                      nullptr, 0);
        const int ds = fz_pixmap_stride(ctx, dst);
        unsigned char* dp = fz_pixmap_samples(ctx, dst);
        for (int y = 0; y < h; ++y) {
            const unsigned char* s =
                sp + static_cast<std::size_t>(y) * static_cast<std::size_t>(ss);
            unsigned char* d =
                dp + static_cast<std::size_t>(y) * static_cast<std::size_t>(ds);
            for (int x = 0; x < w; ++x) {
                d[x * 3 + 0] = s[x * 4 + 0];
                d[x * 3 + 1] = s[x * 4 + 1];
                d[x * 3 + 2] = s[x * 4 + 2];
            }
        }
    }
    fz_catch(ctx) {
        if (dst != nullptr) { fz_drop_pixmap(ctx, dst); dst = nullptr; }
    }
    return dst;
}

// 把一段内存按 Windows 原生宽字符路径写出（导出为图片用）。
//
// 为什么不用 fz_save_pixmap_as_png(ctx, pix, filename)：那些接口收 const char*，
// 走 MuPDF 的 UTF-8 文件名入口；中文/非 ASCII 路径在 Windows 上会被 ANSI 代码页截断
// 或写出失败。这里先编码进 fz_buffer（内存），再用 CreateFileW 按宽路径写出，
// 与项目既有的"宽字符路径优先"约定一致（见 open 用 fz_open_file_w）。
//
// 分块写入：单次 WriteFile 上限按 DWORD 计，超大图（几百 MB）一次写不完。
bool write_bytes_to_wide_path(const std::wstring& path, const unsigned char* data,
                              std::size_t n) noexcept {
    if (path.empty() || data == nullptr || n == 0) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    const unsigned char* p = data;
    std::size_t remaining = n;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(remaining > 0x40000000u ? 0x40000000u : remaining);
        DWORD written = 0;
        if (!WriteFile(h, p, chunk, &written, nullptr) || written == 0) { ok = false; break; }
        p += written;
        remaining -= written;
    }
    CloseHandle(h);
    return ok;
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

    const lr::FormatFamily actual = classify_format(fmt);
    const lr::FormatFamily declared = lr::ext_family_of(ext);
    if (actual == lr::FormatFamily::Unknown || declared == lr::FormatFamily::Unknown) return true;
    return actual == declared;
}

// ---- PageBitmap ----
struct PageBitmap::Impl {
    std::shared_ptr<CtxHandle> ch;   // ctx 的共享所有权（见 CtxHandle 说明）
    fz_pixmap* pix = nullptr;
    float      scale = 1.0f;
    // ADR-083：像素属性在构造时快照，访问器不再回调 MuPDF（零 fz_* 的纯访问器）。
    PixmapAttrs attrs{};

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
    drop_pixmap_safe(impl_->ctx(), impl_->pix);   // ADR-083：销毁走统一边界函数
    delete impl_;
    impl_ = nullptr;
}

bool PageBitmap::valid() const noexcept { return impl_ != nullptr && impl_->pix != nullptr; }

int PageBitmap::width() const noexcept { return impl_ ? impl_->attrs.w : 0; }

int PageBitmap::height() const noexcept { return impl_ ? impl_->attrs.h : 0; }

int PageBitmap::stride() const noexcept { return impl_ ? impl_->attrs.stride : 0; }

const std::uint8_t* PageBitmap::samples() const noexcept {
    return impl_ ? impl_->attrs.samples : nullptr;
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

    // ---- 文本抽取缓存 ----
    // 只缓存**一页**：选择/复制会在同一页上反复触发（每次重建 stext 与渲染一页同价），
    // 而交互始终集中在鼠标所在的那一页。跨页时重建一次（毫秒级），内存因此有界。
    // 归属：stext 由本 ctx 创建，必须先于 ctx 释放。
    fz_stext_page* stext_ = nullptr;
    int            stext_page_ = -1;

    ~Impl() { destroy(); }

    // 丢弃文本抽取缓存（ADR-083：释放走统一边界函数，fz_try/fz_catch 覆盖）
    void drop_stext() noexcept {
        drop_stext_safe(ctx, stext_);
        stext_page_ = -1;
    }

    // 释放 MuPDF 资源。doc 必须先于 ctx 释放（ADR-083：销毁统一走边界函数）。
    void destroy() noexcept {
        drop_stext();
        drop_document_safe(ctx, doc);
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

// ---- 文本抽取（选择/复制/搜索共用）----

// 取某页的 stext（必要时重建并写入 cache）。
//
// 为什么带缓存：建一次 stext 与渲染一页同价（要跑内容流 + 逐字形定位），而选择/复制/
// 搜索会在同一页上反复触发 —— 不缓存的话每次鼠标移动都要重跑一遍内容流。
// 为什么只缓存一页：交互始终集中在鼠标所在的那一页；跨页重建一次（毫秒级）可接受，
// 而缓存多页会让内存随页数增长（stext 含每字符的 quad + 字体引用）。
//
// cache / cache_page 由调用方（Document::Impl）持有，本函数只做"取或建"。
// 失败返回 nullptr 并把原因写进 err_out；此时 cache 保持原样（不破坏已有缓存）。
fz_stext_page* acquire_stext(fz_context* ctx, fz_document* doc, int index,
                             fz_stext_page*& cache, int& cache_page,
                             std::string& err_out) noexcept {
    if (cache != nullptr && cache_page == index) return cache;
    if (ctx == nullptr || doc == nullptr || index < 0) return nullptr;

    fz_page*         page = nullptr;
    fz_stext_page*   st = nullptr;
    DocError         result = DocError::Ok;
    char             err[kErrCap] = {};
    fz_stext_options opts = {};
    // 必须带 PRESERVE_IMAGES：否则 stext 里没有 IMAGE 块，"复制鼠标下的嵌入图片"
    // 就无从定位图片对象。代价只是多存几个块头，图片像素并不因此解码。
    opts.flags = FZ_STEXT_PRESERVE_IMAGES;

    fz_try(ctx) {
        fz_var(page);
        fz_var(st);
        page = fz_load_page(ctx, doc, index);
        st = fz_new_stext_page_from_page(ctx, page, &opts);
    }
    fz_always(ctx) {
        if (page) fz_drop_page(ctx, page);
    }
    fz_catch(ctx) {
        if (st) { fz_drop_stext_page(ctx, st); st = nullptr; }
        result = classify(ctx);
        copy_caught_message(ctx, err, kErrCap);
    }

    if (result != DocError::Ok || st == nullptr) {
        set_error(err_out, err);
        return nullptr;
    }
    if (cache != nullptr) fz_drop_stext_page(ctx, cache);
    cache = st;
    cache_page = index;
    return st;
}

// 解析内部链接的目标页（0 基）。外部 URL 或无法解析返回 -1。
// 失败**不视为错误**：链接热区仍要显示，只是点击时不跳转（按外部处理）。
int resolve_link_target(fz_context* ctx, fz_document* doc, const char* uri) noexcept {
    if (ctx == nullptr || doc == nullptr || uri == nullptr) return -1;
    int page = -1;
    fz_try(ctx) {
        fz_var(page);
        const fz_link_dest d = fz_resolve_link_dest(ctx, doc, uri);
        if (d.loc.page >= 0) page = fz_page_number_from_location(ctx, doc, d.loc);
    }
    fz_catch(ctx) { page = -1; }
    return page;
}

// 取命中所在行的文本，作为结果列表的上下文片段（按 UTF-8 边界截断）。
// 找不到行 / 复制失败返回空串 —— 结果列表宁可不显示上下文，也不能因此丢命中。
void line_snippet(fz_context* ctx, fz_stext_page* st, fz_quad q, std::string& out) {
    out.clear();
    if (ctx == nullptr || st == nullptr) return;

    // 命中四边形的中心；跨行命中时中心可能落在两行之间，故带"最近行"兜底。
    const float cx = (q.ul.x + q.ur.x + q.ll.x + q.lr.x) * 0.25f;
    const float cy = (q.ul.y + q.ur.y + q.ll.y + q.lr.y) * 0.25f;

    fz_rect best{};
    float   best_d = 0.0f;
    int     found = 0;
    for (fz_stext_block* b = st->first_block; b != nullptr; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* ln = b->u.t.first_line; ln != nullptr; ln = ln->next) {
            const fz_rect r = ln->bbox;
            if (cx >= r.x0 && cx <= r.x1 && cy >= r.y0 && cy <= r.y1) {
                best = r;
                found = 1;
                best_d = 0.0f;
                break;
            }
            const float d = std::fabs(cy - (r.y0 + r.y1) * 0.5f);
            if (!found || d < best_d) { best = r; best_d = d; found = 1; }
        }
        if (found && best_d == 0.0f) break;
    }
    if (!found) return;

    char* text = nullptr;
    fz_try(ctx) {
        fz_var(text);
        text = fz_copy_rectangle(ctx, st, best, 0);
    }
    fz_catch(ctx) { text = nullptr; }
    if (text == nullptr) return;

    std::size_t n = 0;
    while (text[n] != '\0') ++n;
    out.assign(text, n);
    fz_free(ctx, text);

    // 折叠换行（行文本本不该有，防御性），再按 UTF-8 边界截断
    for (char& c : out) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (out.size() > kSnippetCap) {
        std::size_t cut = kSnippetCap;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0u) == 0x80u) --cut;
        out.resize(cut);
        out += "…";
    }
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
    if (!accepts_archive(magic) && classify_format(fmt) == lr::FormatFamily::Archive) {
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
        if (accepts_archive(magic) && classify_format(fmt) == lr::FormatFamily::Archive) {
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

// ---- 文本 / 图片 / 链接 ----

DocError Document::page_content(int index, PageContent& out) const noexcept {
    out = PageContent{};
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;
    if (index < 0) return DocError::Internal;
    Impl& s = *impl_;

    // 1) 文本布局（带缓存）。无文本层的页（扫描件）得到空 stext，不是错误。
    fz_stext_page* st = acquire_stext(s.ctx, s.doc, index, s.stext_, s.stext_page_, s.last_error);
    if (st == nullptr) return DocError::Internal;

    // 2) 链接。fz_load_links 的入口是 fz_page 而不是 stext，故需再取一次页
    //    （fz_load_page 只读页字典、不跑内容流，成本可忽略）。
    //    链接加载失败**不影响文本**：复制/选择比链接重要，故失败只丢链接。
    fz_page* page = nullptr;
    fz_link* links = nullptr;
    DocError link_result = DocError::Ok;
    char     err[kErrCap] = {};
    fz_try(s.ctx) {
        fz_var(page);
        fz_var(links);
        page = fz_load_page(s.ctx, s.doc, index);
        links = fz_load_links(s.ctx, page);
    }
    fz_always(s.ctx) {
        if (page) fz_drop_page(s.ctx, page);
    }
    fz_catch(s.ctx) {
        links = nullptr;
        link_result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    // 3) 遍历（fz_try 之外：可安全构造 std::vector/std::string）
    for (fz_stext_block* b = st->first_block; b != nullptr; b = b->next) {
        if (b->type == FZ_STEXT_BLOCK_TEXT) {
            for (fz_stext_line* ln = b->u.t.first_line; ln != nullptr; ln = ln->next) {
                TextLine tl;
                tl.x0 = ln->bbox.x0;
                tl.y0 = ln->bbox.y0;
                tl.x1 = ln->bbox.x1;
                tl.y1 = ln->bbox.y1;
                const int li = static_cast<int>(out.lines.size());
                out.lines.push_back(tl);

                for (fz_stext_char* c = ln->first_char; c != nullptr; c = c->next) {
                    if (c->c <= 0) continue;   // MuPDF 用非正码点标记换行/占位
                    TextChar tc;
                    tc.cp = static_cast<std::uint32_t>(c->c);
                    tc.quad.ulx = c->quad.ul.x; tc.quad.uly = c->quad.ul.y;
                    tc.quad.urx = c->quad.ur.x; tc.quad.ury = c->quad.ur.y;
                    tc.quad.llx = c->quad.ll.x; tc.quad.lly = c->quad.ll.y;
                    tc.quad.lrx = c->quad.lr.x; tc.quad.lry = c->quad.lr.y;
                    tc.line = li;
                    out.chars.push_back(tc);
                }
            }
        } else if (b->type == FZ_STEXT_BLOCK_IMAGE) {
            PageImageRect r;
            r.x0 = b->bbox.x0;
            r.y0 = b->bbox.y0;
            r.x1 = b->bbox.x1;
            r.y1 = b->bbox.y1;
            out.images.push_back(r);
        }
    }

    if (links != nullptr) {
        for (fz_link* l = links; l != nullptr; l = l->next) {
            // 退化热区（宽或高为 0）没有可点面积，跳过 —— 否则会命中测试到一个看不见的链接。
            if (!(l->rect.x1 > l->rect.x0) || !(l->rect.y1 > l->rect.y0)) continue;
            PageLink pl;
            pl.x0 = l->rect.x0;
            pl.y0 = l->rect.y0;
            pl.x1 = l->rect.x1;
            pl.y1 = l->rect.y1;
            if (l->uri != nullptr) {
                pl.uri.assign(l->uri);
                pl.target_page = resolve_link_target(s.ctx, s.doc, l->uri);
            }
            out.links.push_back(std::move(pl));
        }
        fz_drop_link(s.ctx, links);
    }

    if (link_result != DocError::Ok) set_error(s.last_error, err);
    else                              s.last_error.clear();
    return DocError::Ok;
}

DocError Document::copy_text(int index, float ax, float ay, float bx, float by,
                             std::string& out) noexcept {
    out.clear();
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;
    if (index < 0) return DocError::Internal;
    Impl& s = *impl_;

    fz_stext_page* st = acquire_stext(s.ctx, s.doc, index, s.stext_, s.stext_page_, s.last_error);
    if (st == nullptr) return DocError::Internal;

    fz_point a{};   // POD：可安全跨越 longjmp
    fz_point b{};
    a.x = ax; a.y = ay;
    b.x = bx; b.y = by;

    char*    text = nullptr;
    DocError result = DocError::Ok;
    char     err[kErrCap] = {};
    fz_try(s.ctx) {
        fz_var(text);
        // crlf = 1：剪贴板面向 Windows 应用（记事本/Word），用 \r\n 更省事。
        text = fz_copy_selection(s.ctx, st, a, b, 1);
    }
    fz_catch(s.ctx) {
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    if (result != DocError::Ok) {
        set_error(s.last_error, err);
        return result;
    }
    if (text != nullptr) {
        out.assign(text);
        fz_free(s.ctx, text);
    }
    s.last_error.clear();
    return DocError::Ok;
}

DocError Document::image_at(int index, float x, float y, ImageData& out) noexcept {
    out = ImageData{};
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;
    if (index < 0) return DocError::Internal;
    Impl& s = *impl_;

    fz_stext_page* st = acquire_stext(s.ctx, s.doc, index, s.stext_, s.stext_page_, s.last_error);
    if (st == nullptr) return DocError::Internal;

    // 取**最后**一个命中（块按绘制顺序排列，后画的在上层）。
    // fz_image 是 stext 借出的指针，寿命同 stext（本函数内有效）。
    fz_image* img = nullptr;
    for (fz_stext_block* b = st->first_block; b != nullptr; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_IMAGE) continue;
        if (x >= b->bbox.x0 && x <= b->bbox.x1 && y >= b->bbox.y0 && y <= b->bbox.y1)
            img = b->u.i.image;
    }
    if (img == nullptr) return DocError::NotFound;

    fz_pixmap* src = nullptr;
    fz_pixmap* conv = nullptr;
    DocError   result = DocError::Ok;
    char       err[kErrCap] = {};
    fz_try(s.ctx) {
        fz_var(src);
        fz_var(conv);
        int iw = 0, ih = 0;
        // subarea / ctm 传 nullptr：要整张原图、不做子采样
        src = fz_get_pixmap_from_image(s.ctx, img, nullptr, nullptr, &iw, &ih);
        // 统一到 RGB（keep_alpha=1 ⇒ 原图带 alpha 时得到 RGBA，否则 RGB）。
        // 不直接用 src：嵌入图可能是 CMYK / ICC / 灰度 / 单色蒙版，分量数不定，
        // 后面按 3/4 分量展开即可覆盖，其余交给 MuPDF 的色彩管理。
        conv = fz_convert_pixmap(s.ctx, src, fz_device_rgb(s.ctx), nullptr, nullptr,
                                 fz_default_color_params, 1);
    }
    fz_always(s.ctx) {
        if (src) fz_drop_pixmap(s.ctx, src);
    }
    fz_catch(s.ctx) {
        if (conv) { fz_drop_pixmap(s.ctx, conv); conv = nullptr; }
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    if (result != DocError::Ok) {
        set_error(s.last_error, err);
        return result;
    }
    if (conv == nullptr) {
        set_error(s.last_error, "image conversion returned null");
        return DocError::Internal;
    }

    const int w = fz_pixmap_width(s.ctx, conv);
    const int h = fz_pixmap_height(s.ctx, conv);
    const int n = fz_pixmap_components(s.ctx, conv);
    const int stride = fz_pixmap_stride(s.ctx, conv);
    const std::uint8_t* px = fz_pixmap_samples(s.ctx, conv);
    if (w <= 0 || h <= 0 || px == nullptr || n < 3) {
        fz_drop_pixmap(s.ctx, conv);
        set_error(s.last_error, "unexpected image pixmap format");
        return DocError::Internal;
    }

    // 展开为 RGBA8（fz_try 之外）
    out.w = w;
    out.h = h;
    out.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    for (int yy = 0; yy < h; ++yy) {
        const std::uint8_t* srow = px + static_cast<std::size_t>(yy) * static_cast<std::size_t>(stride);
        std::uint8_t* drow = out.rgba.data() +
                             static_cast<std::size_t>(yy) * static_cast<std::size_t>(w) * 4u;
        for (int xx = 0; xx < w; ++xx) {
            drow[xx * 4 + 0] = srow[xx * n + 0];
            drow[xx * 4 + 1] = srow[xx * n + 1];
            drow[xx * 4 + 2] = srow[xx * n + 2];
            drow[xx * 4 + 3] = (n >= 4) ? srow[xx * n + 3] : 255;
        }
    }
    fz_drop_pixmap(s.ctx, conv);
    s.last_error.clear();
    return DocError::Ok;
}

namespace {

// 命中框裁剪用的一行文字外接框（未旋转 pt）。
struct HitLineBand { float x0, y0, x1, y1; };

// 把命中矩形的纵向裁进"行距"以内（居中）：重排文本（EPUB）里 fz_stext_char 的 quad 纵向是
// **整行行框（ascent+descent）**，常大于实际行距 —— 直接拿它当命中框会偏高、相邻两行的命中
// 会上下重叠。行距 = 与**横向有交叠**的最近邻行的中心距（多列版面靠横向交叠区分，并排行带
// 本可重叠，不能当行距）。
// 与 app 层选区高亮（text_interaction.cpp 的 clamp_rect_to_pitch / line_pitch）是同一规则、
// 同一根源问题；两处各自实现，是因为命中框在 document 层产生、选区框在 app 层产生，数据源不同。
void trim_hit_to_line_pitch(float& y0, float& y1, float x0, float x1,
                            const std::vector<HitLineBand>& lines) {
    if (lines.empty()) return;
    const float cy = (y0 + y1) * 0.5f;
    float pitch = 0.0f;
    for (const HitLineBand& L : lines) {
        if (L.x1 <= x0 || L.x0 >= x1) continue;   // 横向无交叠 = 另一列，跳过
        float d = (L.y0 + L.y1) * 0.5f - cy;
        if (d < 0.0f) d = -d;
        if (d > 0.0f && (pitch == 0.0f || d < pitch)) pitch = d;
    }
    if (pitch <= 0.0f || (y1 - y0) <= pitch) return;
    y0 = cy - pitch * 0.5f;
    y1 = cy + pitch * 0.5f;
}

}  // namespace

DocError Document::search_page(int index, std::string_view utf8_needle, int max_hits,
                               std::vector<SearchHit>& out) noexcept {
    out.clear();
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;
    if (index < 0) return DocError::Internal;
    if (utf8_needle.empty()) return DocError::Ok;
    Impl& s = *impl_;

    fz_stext_page* st = acquire_stext(s.ctx, s.doc, index, s.stext_, s.stext_page_, s.last_error);
    if (st == nullptr) return DocError::Internal;

    if (max_hits <= 0) max_hits = 1;
    if (max_hits > kMaxSearchHitsPerPage) max_hits = kMaxSearchHitsPerPage;

    // 关键字与命中缓冲都在 fz_try 之外备好（std::string 的 c_str 保证 '\0' 结尾）
    const std::string needle(utf8_needle);
    const std::size_t cap = static_cast<std::size_t>(max_hits);
    fz_quad* quads = new (std::nothrow) fz_quad[cap];
    int*     marks = new (std::nothrow) int[cap];
    if (quads == nullptr || marks == nullptr) {
        delete[] quads;
        delete[] marks;
        set_error(s.last_error, "out of memory allocating search buffers");
        return DocError::Internal;
    }

    int      n = 0;
    DocError result = DocError::Ok;
    char     err[kErrCap] = {};
    fz_try(s.ctx) {
        fz_var(n);
        n = fz_search_stext_page(s.ctx, st, needle.c_str(), marks, quads, max_hits);
    }
    fz_catch(s.ctx) {
        result = classify(s.ctx);
        copy_caught_message(s.ctx, err, kErrCap);
    }

    if (result != DocError::Ok) {
        delete[] quads;
        delete[] marks;
        set_error(s.last_error, err);
        return result;
    }
    if (n < 0) n = 0;
    if (n > max_hits) n = max_hits;

    // 收集本页行框：把每个命中框裁进行距（见 trim_hit_to_line_pitch）。
    std::vector<HitLineBand> lines;
    for (fz_stext_block* b = st->first_block; b != nullptr; b = b->next) {
        if (b->type != FZ_STEXT_BLOCK_TEXT) continue;
        for (fz_stext_line* ln = b->u.t.first_line; ln != nullptr; ln = ln->next)
            lines.push_back(HitLineBand{ ln->bbox.x0, ln->bbox.y0, ln->bbox.x1, ln->bbox.y1 });
    }

    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const fz_quad& q = quads[i];
        SearchHit h;
        h.page = index;
        h.x0 = std::min(std::min(q.ul.x, q.ur.x), std::min(q.ll.x, q.lr.x));
        h.y0 = std::min(std::min(q.ul.y, q.ur.y), std::min(q.ll.y, q.lr.y));
        h.x1 = std::max(std::max(q.ul.x, q.ur.x), std::max(q.ll.x, q.lr.x));
        h.y1 = std::max(std::max(q.ul.y, q.ur.y), std::max(q.ll.y, q.lr.y));
        trim_hit_to_line_pitch(h.y0, h.y1, h.x0, h.x1, lines);
        line_snippet(s.ctx, st, q, h.snippet);
        out.push_back(std::move(h));
    }
    delete[] quads;
    delete[] marks;
    s.last_error.clear();
    return DocError::Ok;
}

DocError Document::render_page(int index, float scale, PageBitmap& out,
                               int max_dimension, int rotation_deg,
                               PageScheme scheme) noexcept {
    return render_page_region(index, scale, out, max_dimension, rotation_deg, scheme, nullptr);
}

DocError Document::render_page_tile(int index, float scale, TileRect tile, PageBitmap& out,
                                    int max_dimension, int rotation_deg,
                                    PageScheme scheme) noexcept {
    return render_page_region(index, scale, out, max_dimension, rotation_deg, scheme, &tile);
}

DocError Document::render_page_region(int index, float scale, PageBitmap& out,
                                      int max_dimension, int rotation_deg,
                                      PageScheme scheme, const TileRect* tile) noexcept {
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
    fz_device* dev = nullptr;        // 当前的分流设备（转发壳）
    fz_device* dev_inner = nullptr;  // 它的下游 draw device
    float      used = scale;
    int        mode = static_cast<int>(scheme);
    DocError   result = DocError::Ok;
    char       err[kErrCap] = {};

    fz_try(s.ctx) {
        fz_var(page);
        fz_var(pix);
        fz_var(dev);
        fz_var(dev_inner);
        fz_var(used);

        page = fz_load_page(s.ctx, s.doc, index);
        const fz_rect bounds = fz_bound_page(s.ctx, page);

        // 尺寸钳制按**旋转后**包围盒：90/270 时宽高互换。若按未旋转的 bounds 钳制，
        // 一张 8000×100 的横幅旋转后会得到 8000 高，仍然爆内存。
        const fz_rect rotated = fz_transform_rect(bounds, fz_rotate(static_cast<float>(rot)));
        // tile 模式按请求倍率渲染；只为当前 tile 分配 pixmap，不能按整页尺寸把倍率压低。
        used = tile ? scale : clamp_scale(rotated, scale, max_dimension);

        // 旋转与缩放都是线性变换，且缩放是等比的（标量×单位矩阵）⇒ 二者可交换，顺序无关。
        const fz_matrix ctm = fz_pre_rotate(fz_scale(used, used), static_cast<float>(rot));
        const fz_irect  full_bbox = fz_round_rect(fz_transform_rect(bounds, ctm));
        fz_irect bbox = full_bbox;
        if (tile) {
            int tx = tile->x;
            int ty = tile->y;
            int tw = tile->w;
            int th = tile->h;
            if (max_dimension > 0 && tw > max_dimension) tw = max_dimension;
            if (max_dimension > 0 && th > max_dimension) th = max_dimension;
            if (tx < 0) tx = 0;
            if (ty < 0) ty = 0;
            if (tw < 1) tw = 1;
            if (th < 1) th = 1;
            const int full_w = full_bbox.x1 - full_bbox.x0;
            const int full_h = full_bbox.y1 - full_bbox.y0;
            if (tx > full_w - 1) tx = full_w - 1;
            if (ty > full_h - 1) ty = full_h - 1;
            if (tx + tw > full_w) tw = full_w - tx;
            if (ty + th > full_h) th = full_h - ty;
            bbox.x0 = full_bbox.x0 + tx;
            bbox.y0 = full_bbox.y0 + ty;
            bbox.x1 = bbox.x0 + tw;
            bbox.y1 = bbox.y0 + th;
        }

        // RGB + alpha ⇒ 4 分量 RGBA8，可直接上传 DXGI_FORMAT_R8G8B8A8_UNORM
        pix = fz_new_pixmap_with_bbox(s.ctx, fz_device_rgb(s.ctx), bbox, nullptr, 1);
        // 不透明白底（即"纸色"）：所有分量填 0xFF。之后页面内容以 SRC_OVER 合成，
        // 因目标 alpha 恒为 255，结果 alpha 也恒为 255，无需再做反预乘。
        fz_clear_pixmap_with_value(s.ctx, pix, 0xFF);

        if (mode == 0) {
            // 原色：单遍直绘，与分层之前的路径逐字节等价（也是绝大多数时候的快路径）
            dev_inner = fz_new_draw_device(s.ctx, ctm, pix);
            fz_run_page(s.ctx, page, dev_inner, fz_identity, nullptr);
            fz_close_device(s.ctx, dev_inner);
            fz_drop_device(s.ctx, dev_inner);
            dev_inner = nullptr;
        } else {
            // 第一遍：纸墨层（拦掉照片）。此时若有照片，它的位置只是白纸。
            dev_inner = fz_new_draw_device(s.ctx, ctm, pix);
            dev = split_new(s.ctx, dev_inner, kSplitInk, 1.0f);
            fz_run_page(s.ctx, page, dev, fz_identity, nullptr);
            const int   images     = as_split(dev)->images;
            const float image_area = as_split(dev)->image_area;
            fz_close_device(s.ctx, dev);
            fz_drop_device(s.ctx, dev);        // 只回收转发壳，inner 由下一行负责
            dev = nullptr;
            fz_close_device(s.ctx, dev_inner);
            fz_drop_device(s.ctx, dev_inner);
            dev_inner = nullptr;

            // 整页扫描件：图像几乎铺满整页、且纸墨层没画什么东西（扫描书、扫描证件…）。
            // 这类页面的内容**全在图像里**，若照常"纸墨层套配色、照片原样叠回"，深色纸张模式下
            // 会得到一整页白 —— 等于没开。故改走：图像原样铺满 → 最后整体套一次配色。
            // 反例（不触发）：整页底图 + 正文的杂志封面 —— 纸墨层有文字，就该只变换文字。
            const float page_area =
                static_cast<float>(fz_pixmap_width(s.ctx, pix)) *
                static_cast<float>(fz_pixmap_height(s.ctx, pix));
            const bool scanned = (images > 0) && page_area > 0.0f &&
                                 image_area >= static_cast<float>(kScanCoverageFloor) * page_area &&
                                 paper_is_blank(s.ctx, pix);
            if (!scanned) apply_scheme_lut(s.ctx, pix, mode);

            // 第二遍：图像层（只放行照片）。压暗后叠回，照片保留原本的色彩。
            if (images > 0) {
                dev_inner = fz_new_draw_device(s.ctx, ctm, pix);
                dev = split_new(s.ctx, dev_inner, kSplitImage, scanned ? 1.0f : kImageDim);
                fz_run_page(s.ctx, page, dev, fz_identity, nullptr);
                fz_close_device(s.ctx, dev);
                fz_drop_device(s.ctx, dev);
                dev = nullptr;
                fz_close_device(s.ctx, dev_inner);
                fz_drop_device(s.ctx, dev_inner);
                dev_inner = nullptr;
                if (scanned) apply_scheme_lut(s.ctx, pix, mode);
            }
        }

        fz_drop_page(s.ctx, page);
        page = nullptr;
    }
    fz_always(s.ctx) {
        // 出错路径：dev 未 close 就 drop，MuPDF 会给出 "dropping unclosed device" 警告，
        // 但不会泄漏 —— fz_always 内不得调用可能抛异常的 fz_close_device。
        if (dev) fz_drop_device(s.ctx, dev);
        if (dev_inner) fz_drop_device(s.ctx, dev_inner);
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
    // ADR-083：属性读取经边界函数 pixmap_attrs（fz_try/fz_catch 覆盖），并顺带快照给 PageBitmap。
    const PixmapAttrs attrs = pixmap_attrs(s.ctx, pix);
    if (attrs.components != kComponents || attrs.samples == nullptr) {
        drop_pixmap_safe(s.ctx, pix);
        set_error(s.last_error, "unexpected pixmap component count");
        return DocError::Internal;
    }

    auto* impl = new (std::nothrow) PageBitmap::Impl{ s.ch, pix, used, attrs };
    if (impl == nullptr) {
        drop_pixmap_safe(s.ctx, pix);
        set_error(s.last_error, "out of memory allocating page bitmap");
        return DocError::Internal;
    }

    out = PageBitmap(impl);
    return DocError::Ok;
}

// ---- 导出为图片 ----

DocError Document::save_page_as_image(int index, float scale, ImageFormat fmt, int quality,
                                      int max_dimension, int rotation_deg, PageScheme scheme,
                                      const std::wstring& path,
                                      float* out_effective_scale) noexcept {
    if (out_effective_scale != nullptr) *out_effective_scale = 0.0f;
    if (!impl_ || !impl_->doc || !impl_->ctx) return DocError::NotOpen;
    if (path.empty()) return DocError::Internal;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    // 1) 渲染整页：完全复用 render_page 的路径（旋转 / 纸张方案 / 尺寸钳制都在里面）。
    //    PageBitmap 是带析构函数的 C++ 对象，构造在 fz_try 之外，符合 ADR-009。
    PageBitmap bmp;
    const DocError render_err =
        render_page(index, scale, bmp, max_dimension, rotation_deg, scheme);
    if (render_err != DocError::Ok) return render_err;
    if (!bmp.valid()) {
        set_error(impl_->last_error, "render returned an empty bitmap");
        return DocError::Internal;
    }
    if (out_effective_scale != nullptr) *out_effective_scale = bmp.effective_scale();

    // Document 是 PageBitmap 的友元，可直接取内部的 fz_pixmap（编码需要它，而不是裸像素）。
    fz_context* ctx = impl_->ctx;
    fz_pixmap*  pix = (bmp.impl_ != nullptr) ? bmp.impl_->pix : nullptr;
    if (pix == nullptr) {
        set_error(impl_->last_error, "render produced a null pixmap");
        return DocError::Internal;
    }

    // 2) 编码进内存缓冲。JPEG 需要先转成不透明 RGB（见 rgb_pixmap_from_rgba 的说明），
    //    转换与编码各自独立，释放路径清晰。
    fz_pixmap* rgb = nullptr;
    fz_buffer* buf = nullptr;
    DocError   result = DocError::Ok;
    char       err[kErrCap] = {};

    if (fmt == ImageFormat::Jpeg) {
        rgb = rgb_pixmap_from_rgba(ctx, pix);
        if (rgb == nullptr) {
            set_error(impl_->last_error, "failed to convert page to RGB for JPEG");
            return DocError::Internal;
        }
    }

    fz_try(ctx) {
        fz_var(buf);
        if (fmt == ImageFormat::Png)
            buf = fz_new_buffer_from_pixmap_as_png(ctx, pix, fz_default_color_params);
        else
            buf = fz_new_buffer_from_pixmap_as_jpeg(ctx, rgb, fz_default_color_params,
                                                    quality, 0);
    }
    fz_catch(ctx) {
        if (buf != nullptr) { fz_drop_buffer(ctx, buf); buf = nullptr; }
        result = classify(ctx);
        copy_caught_message(ctx, err, kErrCap);
    }
    drop_pixmap_safe(ctx, rgb);   // 无论成功失败都释放（ADR-083 边界函数）

    if (result != DocError::Ok) {
        set_error(impl_->last_error, err);
        return result;
    }
    if (buf == nullptr) {
        set_error(impl_->last_error, "image encode returned a null buffer");
        return DocError::Internal;
    }

    // 3) 取缓冲数据（fz_try 覆盖），随后在 fz_try 之外按宽字符路径写出。
    // 注意 fz_buffer_storage 的签名：**返回**字节数，指针经出参给出。
    unsigned char* data = nullptr;
    std::size_t    len = 0;
    fz_try(ctx) {
        fz_var(data);
        fz_var(len);
        len = fz_buffer_storage(ctx, buf, &data);
    }
    fz_catch(ctx) {
        data = nullptr;
        len = 0;
    }

    const bool wrote = (data != nullptr && len > 0) &&
                       write_bytes_to_wide_path(path, data, len);
    fz_drop_buffer(ctx, buf);

    if (!wrote) {
        set_error(impl_->last_error, "failed to write image file");
        return DocError::Internal;
    }
    impl_->last_error.clear();
    return DocError::Ok;
}

}  // namespace lr
