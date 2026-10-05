// doc_test.cpp — Lilith Reader 文档核心自动化测试（Phase 2 起常备）
//
// 做法：**直接链接项目自己的 lilithreader.utils / lilithreader.document 模块**，
// 对 tests/samples/ 里的样本逐一断言 open()/info()/authenticate() 的结果。
// 这样测的是真正要发货的代码，而不是测试里重写的一份逻辑。
//
// 期望值来源写死在下面的表里，**不是从实际输出回填的**：
//   · 合法文件 → 必须打开且页数正确；
//   · 改名可读 → 必须打开（内容优先，ADR-016），且 format_matches_extension 给出 false；
//   · 归档冒充 → 必须 Mismatched；
//   · 空/截断/纯文本/随机字节 → 必须 Corrupt；
//   · 加密 → 必须 NeedsPassword，且 authenticate("lilith") 后打开成功；
//   · 需修复型（junk/坏 startxref）→ 用"容错断言"：OPEN 或 Corrupt 都算通过（MuPDF 的
//     修复策略属实现细节，不该被我们钉死成规格），但**绝不允许崩溃或挂死**。
//
// 退出码：0 = 全部通过（含 SKIP），1 = 有 FAIL。缺失的可选样本（加密类，需 PyMuPDF 生成）
// 记为 SKIP 且不算失败，保证无 PyMuPDF 的环境也能跑。
//
// 用法： doc_test.exe <samples 目录>

#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

import lilithreader.utils;
import lilithreader.document;

namespace {

// ---- 期望模型 ----

enum class Expect {
    Open,            // 必须打开
    Reject,          // 必须失败，且错误码等于 err
    OpenOrReject,    // 容错：打开或按 err 失败都算通过（修复型样本）
};

struct Case {
    const char*  name;      // 样本文件名
    Expect       expect;
    lr::DocError err;       // Reject / OpenOrReject 时的期望错误码
    int          pages;     // > 0 时校验页数
    const char*  fmt;       // 非空时校验 info.format 包含该子串
    int          ext_ok;    // -1 不校验；0/1 校验 format_matches_extension
    const char*  why;       // 这条用例在守护什么（打印在报告里）
};

constexpr int kNoCheck = -1;

const Case kCases[] = {
    // ---- 1. 合法文件 ----
    {"real.pdf",   Expect::Open, {}, 3, "PDF", 1, "PDF 页数与格式串"},
    {"real.epub",  Expect::Open, {}, 1, "EPUB", 1, "EPUB 打开"},
    {"real.fb2",   Expect::Open, {}, 2, "FictionBook", 1, "FB2 打开"},
    {"real.xps",   Expect::Open, {}, 1, "XPS", 1, "XPS 打开"},
    {"real.png",   Expect::Open, {}, 1, "Image", 1, "单页图片：白名单放行（ADR-017）"},
    {"comic.cbz",  Expect::Open, {}, 3, "zip", 1, "CBZ 页数=图片数"},
    {"outline.pdf", Expect::Open, {}, 3, "PDF", 1, "带目录的 PDF（Phase 5）"},

    // ---- 2. 改名但内容可读：内容优先，打开但标注不符（ADR-016） ----
    {"img_named_pdf.pdf",   Expect::Open, {}, 1, "Image", 0, "图片改 .pdf 仍打开"},
    {"img_named_epub.epub", Expect::Open, {}, 1, "Image", 0, "图片改 .epub 仍打开"},
    {"img_named_cbz.cbz",   Expect::Open, {}, 1, "Image", 0, "图片改 .cbz 仍打开"},
    {"epub_named_pdf.pdf",  Expect::Open, {}, 1, "EPUB", 0, "epub 改 .pdf 仍按 epub 读"},
    {"xps_named_epub.epub", Expect::Open, {}, 1, "XPS", 0, "xps 改 .epub 仍按 xps 读"},
    {"fb2_named_epub.epub", Expect::Open, {}, 2, "FictionBook", 0, "fb2 改 .epub 仍按 fb2 读"},
    {"pdf_named_cbz.cbz",   Expect::Open, {}, 1, "PDF", 0, "pdf 改 .cbz 仍按 pdf 读"},

    // ---- 3. 归档冒充单文档：唯一必须拒绝的错配（ADR-016） ----
    {"zipimg_named_epub.epub", Expect::Reject, lr::DocError::Mismatched, 0, nullptr,
     kNoCheck, "zip 图集改 .epub：不得显示成 N 页"},
    {"zipimg_named_pdf.pdf",   Expect::Reject, lr::DocError::Mismatched, 0, nullptr,
     kNoCheck, "zip 图集改 .pdf"},
    {"zipimg_named_fb2.fb2",   Expect::Reject, lr::DocError::Mismatched, 0, nullptr,
     kNoCheck, "zip 图集改 .fb2"},
    {"zippdf_named_cbz.cbz",   Expect::Reject, lr::DocError::Mismatched, 0, nullptr,
     kNoCheck, "zip 里没有图片页：不是漫画"},
    {"ziptxt_named_cbz.cbz",   Expect::Reject, lr::DocError::Mismatched, 0, nullptr,
     kNoCheck, "zip 里全是 txt：不是漫画"},

    // ---- 4. MuPDF 也认不出 → Corrupt ----
    {"empty.pdf",             Expect::Reject, lr::DocError::Corrupt, 0, nullptr,
     kNoCheck, "0 字节"},
    {"truncated.pdf",         Expect::Reject, lr::DocError::Corrupt, 0, nullptr,
     kNoCheck, "截断到 1/3"},
    {"txt_named_pdf.pdf",     Expect::Reject, lr::DocError::Corrupt, 0, nullptr,
     kNoCheck, "纯文本改名 .pdf（MuPDF 无文本处理器）"},
    {"random.pdf",            Expect::Reject, lr::DocError::Corrupt, 0, nullptr,
     kNoCheck, "随机字节"},
    {"empty_zip.epub",        Expect::Reject, lr::DocError::Corrupt, 0, nullptr,
     kNoCheck, "空 zip 改 .epub"},
    {"zippdf_named_epub.epub", Expect::Reject, lr::DocError::Corrupt, 0, nullptr,
     kNoCheck, "zip 改 .epub：epub 处理器校验 container.xml 失败"},

    // ---- 5. 需修复型：容错断言 ----
    {"junk_before_header.pdf", Expect::OpenOrReject, lr::DocError::Corrupt, 0, nullptr,
     kNoCheck, "头部有垃圾：MuPDF 可能修复后打开"},
    {"no_startxref.pdf",       Expect::OpenOrReject, lr::DocError::Corrupt, 0, nullptr,
     kNoCheck, "startxref 被破坏：MuPDF 可能重建 xref"},
};

// 加密类样本：需 PyMuPDF 生成，缺失则 SKIP
struct EncCase {
    const char* name;
    bool        needs_password;  // true: 必须要求密码；false: 必须能直接打开
    const char* why;
};

const EncCase kEncCases[] = {
    {"enc_aes128_user.pdf", true,  "AES-128 + user 密码 → NeedsPassword，authenticate 后打开"},
    {"enc_aes256_user.pdf", true,  "AES-256 + user 密码 → NeedsPassword，authenticate 后打开"},
    {"enc_owner_only.pdf",  false, "只设 owner 密码 → 应直接打开（权限受限但不锁）"},
};

// ---- 统计与输出 ----

int g_pass = 0, g_fail = 0, g_skip = 0;

void pass(const char* name, const char* why) {
    ++g_pass;
    std::printf("  [PASS] %-26s %s\n", name, why);
}

void fail(const char* name, const char* why, const char* detail) {
    ++g_fail;
    std::printf("  [FAIL] %-26s %s\n         -> %s\n", name, why, detail);
}

void skip(const char* name, const char* why) {
    ++g_skip;
    std::printf("  [SKIP] %-26s %s（样本不存在）\n", name, why);
}

bool file_exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}


// ---- 单个普通用例 ----

void run_case(const std::wstring& dir, const Case& c) {
    const std::wstring path = dir + L"\\" + std::wstring(c.name, c.name + std::strlen(c.name));
    if (!file_exists(path)) { skip(c.name, c.why); return; }

    lr::Document doc;
    const lr::DocError err = doc.open(path);

    if (c.expect == Expect::Reject) {
        if (err != c.err) {
            char d[256];
            std::snprintf(d, sizeof d, "期望 %s，实际 %s（%s）",
                          std::string(lr::to_string(c.err)).c_str(),
                          std::string(lr::to_string(err)).c_str(),
                          std::string(doc.last_error()).c_str());
            fail(c.name, c.why, d);
            return;
        }
        pass(c.name, c.why);
        return;
    }

    if (err != lr::DocError::Ok) {
        if (c.expect == Expect::OpenOrReject && err == c.err) { pass(c.name, c.why); return; }
        char d[256];
        std::snprintf(d, sizeof d, "期望打开，实际 %s（%s）",
                      std::string(lr::to_string(err)).c_str(),
                      std::string(doc.last_error()).c_str());
        fail(c.name, c.why, d);
        return;
    }

    lr::DocumentInfo info;
    const lr::DocError ierr = doc.info(info);
    if (ierr != lr::DocError::Ok) {
        fail(c.name, c.why, "open 成功但 info 失败");
        return;
    }
    if (c.pages > 0 && info.page_count != c.pages) {
        char d[128];
        std::snprintf(d, sizeof d, "页数期望 %d，实际 %d", c.pages, info.page_count);
        fail(c.name, c.why, d);
        return;
    }
    if (c.fmt != nullptr && info.format.find(c.fmt) == std::string::npos) {
        char d[160];
        std::snprintf(d, sizeof d, "格式串期望含 \"%s\"，实际 \"%s\"", c.fmt,
                      info.format.c_str());
        fail(c.name, c.why, d);
        return;
    }
    if (c.ext_ok != kNoCheck) {
        char ext[32] = {};
        const char* dot = std::strrchr(c.name, '.');
        std::snprintf(ext, sizeof ext, "%s", dot ? dot : "");
        const bool got = lr::format_matches_extension(info.format, ext);
        if (got != (c.ext_ok != 0)) {
            char d[160];
            std::snprintf(d, sizeof d, "format_matches_extension(\"%s\", \"%s\") 期望 %s，实际 %s",
                          info.format.c_str(), ext, c.ext_ok ? "true" : "false",
                          got ? "true" : "false");
            fail(c.name, c.why, d);
            return;
        }
    }
    pass(c.name, c.why);
}

// ---- 加密用例 ----

void run_enc_case(const std::wstring& dir, const EncCase& c, const char* password) {
    const std::wstring path = dir + L"\\" + std::wstring(c.name, c.name + std::strlen(c.name));
    if (!file_exists(path)) { skip(c.name, c.why); return; }

    lr::Document doc;
    const lr::DocError err = doc.open(path);

    if (!c.needs_password) {
        if (err != lr::DocError::Ok) {
            char d[200];
            std::snprintf(d, sizeof d, "期望直接打开，实际 %s（%s）",
                          std::string(lr::to_string(err)).c_str(),
                          std::string(doc.last_error()).c_str());
            fail(c.name, c.why, d);
            return;
        }
        lr::DocumentInfo info;
        if (doc.info(info) != lr::DocError::Ok || info.page_count != 1) {
            fail(c.name, c.why, "打开后 info/页数异常");
            return;
        }
        pass(c.name, c.why);
        return;
    }

    if (err != lr::DocError::NeedsPassword) {
        char d[200];
        std::snprintf(d, sizeof d, "期望 needs-password，实际 %s（%s）",
                      std::string(lr::to_string(err)).c_str(),
                      std::string(doc.last_error()).c_str());
        fail(c.name, c.why, d);
        return;
    }
    // 错误密码必须失败
    if (doc.authenticate("wrong-password") == lr::DocError::Ok) {
        fail(c.name, c.why, "错误密码竟然解开了");
        return;
    }
    // 正确密码必须成功，并能读出页数
    if (doc.authenticate(password) != lr::DocError::Ok) {
        char d[200];
        std::snprintf(d, sizeof d, "正确密码 %s 未解开（%s）", password,
                      std::string(doc.last_error()).c_str());
        fail(c.name, c.why, d);
        return;
    }
    lr::DocumentInfo info;
    if (doc.info(info) != lr::DocError::Ok || info.page_count != 1) {
        fail(c.name, c.why, "解锁后 info/页数异常");
        return;
    }
    pass(c.name, c.why);
}

// ---- 扩展名闸门（utils::is_supported）----

struct GateCase { const wchar_t* path; bool supported; const char* why; };

const GateCase kGateCases[] = {
    {L"a.pdf",         true,  "受支持"},
    {L"a.PDF",         true,  "大小写不敏感"},
    {L"a.epub",        true,  "受支持"},
    {L"a.cbz",         true,  "受支持"},
    {L"a.png",         true,  "图片已放行（ADR-017）"},
    {L"a.JPG",         true,  "图片大小写不敏感"},
    {L"a.webp",        false, "MuPDF 无 webp 解码器，仍在清单外"},
    {L"a.txt",         false, "清单外"},
    {L"a.xyz",         false, "清单外"},
    {L"noext",         false, "无扩展名"},
    {L"dir.d/a",       false, "目录名里的点不算扩展名"},
    {L"a.tar.gz",      false, "多段扩展名只看最后一段"},
};

void run_gate_cases() {
    std::printf("\n-- 扩展名闸门（A-2-14/2-15，ADR-013/017）--\n");
    for (const GateCase& g : kGateCases) {
        const bool got = lr::is_supported(g.path);
        if (got == g.supported) pass("is_supported", g.why);
        else {
            char d[200];
            std::snprintf(d, sizeof d, "is_supported 期望 %s，实际 %s",
                          g.supported ? "true" : "false", got ? "true" : "false");
            fail("is_supported", g.why, d);
        }
    }
}

// ---- 逐页尺寸（异构页尺寸 → 形变的防线）----
//
// PDF 允许各页尺寸/纵横比不同。画布按**逐页**尺寸布局；若 document 层只报首页尺寸，
// 尺寸不一致的 PDF 会被拉伸成首页纵横比而形变（第三轮调试实测）。
// 这里把"逐页尺寸"本身钉成规格：长度必须等于页数，每页一次 fz_bound_page，
// 探测失败的页为 {0,0}（上层回退到首页尺寸）。
struct SizeExpect { float w, h; };

struct SizeCase {
    const char* name;
    const char* why;
    int         count;
    SizeExpect  pages[8];
};

const SizeCase kSizeCases[] = {
    {"real.pdf", "均匀页：各页尺寸 = MediaBox 200x300", 3,
     {{200, 300}, {200, 300}, {200, 300}}},
    {"mixed_size.pdf", "异构页：三种纵横比，逐页尺寸必须各不相同", 4,
     {{612, 792}, {792, 612}, {400, 600}, {612, 792}}},
};

void run_size_cases(const std::wstring& dir) {
    std::printf("\n-- 逐页尺寸（异构页尺寸形变防线，%zu 例）--\n",
                sizeof kSizeCases / sizeof kSizeCases[0]);
    for (const SizeCase& c : kSizeCases) {
        const std::wstring path = dir + L"\\" + std::wstring(c.name, c.name + std::strlen(c.name));
        if (!file_exists(path)) { skip(c.name, c.why); continue; }

        lr::Document doc;
        if (doc.open(path) != lr::DocError::Ok) { fail(c.name, c.why, "打不开"); continue; }
        lr::DocumentInfo info;
        if (doc.info(info) != lr::DocError::Ok) { fail(c.name, c.why, "info 失败"); continue; }

        if (info.page_count != c.count ||
            static_cast<int>(info.page_sizes.size()) != info.page_count) {
            char d[200];
            std::snprintf(d, sizeof d, "逐页尺寸条目数 %zu / 页数 %d，期望 %d",
                          info.page_sizes.size(), info.page_count, c.count);
            fail(c.name, c.why, d);
            continue;
        }
        bool ok = true;
        char d[256] = {};
        for (int i = 0; i < c.count; ++i) {
            const float w = info.page_sizes[static_cast<std::size_t>(i)].width_pt;
            const float h = info.page_sizes[static_cast<std::size_t>(i)].height_pt;
            if (std::fabs(w - c.pages[i].w) > 0.5f || std::fabs(h - c.pages[i].h) > 0.5f) {
                std::snprintf(d, sizeof d, "第 %d 页 %.1fx%.1f，期望 %.1fx%.1f",
                              i, (double)w, (double)h,
                              (double)c.pages[i].w, (double)c.pages[i].h);
                ok = false;
                break;
            }
        }
        // 首页尺寸字段必须与 page_sizes[0] 一致（画布的 0 尺寸回退依赖它）
        if (ok && (std::fabs(info.page_width_pt - c.pages[0].w) > 0.5f ||
                   std::fabs(info.page_height_pt - c.pages[0].h) > 0.5f)) {
            std::snprintf(d, sizeof d, "首页尺寸字段 %.1fx%.1f 与 page_sizes[0] 不一致",
                          (double)info.page_width_pt, (double)info.page_height_pt);
            ok = false;
        }
        if (ok) pass(c.name, c.why);
        else    fail(c.name, c.why, d);
    }
}

// ---- 目录（outline）解析（Phase 5）----
//
// outline.pdf 的目录结构（前序）：Chapter 1(→页0, 有子项) / Section 1.1(→页1) / Chapter 2(→页2)。
// 断言展平顺序、层级（depth）与页号解析都正确；无目录文档返回 Ok 且空表（不是错误）。
struct OutlineExpect { const char* title; int page; int depth; };

void run_outline_cases(const std::wstring& dir) {
    std::printf("\n-- 目录（outline）解析（Phase 5）--\n");
    const std::wstring path = dir + L"\\outline.pdf";
    if (!file_exists(path)) { skip("outline.pdf", "目录解析"); return; }

    lr::Document doc;
    if (doc.open(path) != lr::DocError::Ok) { fail("outline.pdf", "目录解析", "打不开"); return; }
    lr::DocumentInfo info;
    if (doc.info(info) != lr::DocError::Ok || !info.has_outline) {
        fail("outline.pdf", "目录解析", "info 失败或 has_outline=false");
        return;
    }
    std::vector<lr::OutlineItem> items;
    if (doc.outline(items) != lr::DocError::Ok) {
        fail("outline.pdf", "目录解析", "outline 失败");
        return;
    }

    const OutlineExpect want[] = {
        { "Chapter 1",   0, 0 },
        { "Section 1.1", 1, 1 },
        { "Chapter 2",   2, 0 },
    };
    const int n = static_cast<int>(sizeof want / sizeof want[0]);
    if (static_cast<int>(items.size()) != n) {
        char d[128];
        std::snprintf(d, sizeof d, "目录项数 %zu，期望 %d", items.size(), n);
        fail("outline.pdf", "目录解析", d);
        return;
    }
    for (int i = 0; i < n; ++i) {
        char name[64];
        std::snprintf(name, sizeof name, "目录项 %d", i);
        if (items[i].title != want[i].title || items[i].page != want[i].page ||
            items[i].depth != want[i].depth) {
            char d[220];
            std::snprintf(d, sizeof d,
                          "第 %d 项 \"%s\" page=%d depth=%d，期望 \"%s\" page=%d depth=%d",
                          i, items[i].title.c_str(), items[i].page, items[i].depth,
                          want[i].title, want[i].page, want[i].depth);
            fail("outline.pdf", name, d);
            return;
        }
        pass("outline.pdf", name);
    }

    // 无目录文档：返回 Ok 且空表
    const std::wstring plain = dir + L"\\real.pdf";
    if (file_exists(plain)) {
        lr::Document d2;
        if (d2.open(plain) == lr::DocError::Ok) {
            std::vector<lr::OutlineItem> o2;
            const lr::DocError oe = d2.outline(o2);
            if (oe == lr::DocError::Ok && o2.empty())
                pass("real.pdf", "无目录返回 Ok 且空表");
            else
                fail("real.pdf", "无目录返回 Ok 且空表", "结果不符");
        }
    }
}

// ---- 渲染：旋转与配色（Phase 5）----
//
// real.pdf 每页 200x300 pt。断言：0°/180° 输出尺寸不变，90° 宽高互换；
// 反色把白色背景变黑且保留 alpha；护眼把白色背景映射为暖色（R>B）。
// 这些都能在无窗口环境下由 MuPDF 直接渲染，属"可程序判定的证据"。
void run_render_cases(const std::wstring& dir) {
    std::printf("\n-- 渲染：旋转与配色（Phase 5）--\n");
    const std::wstring path = dir + L"\\real.pdf";
    if (!file_exists(path)) { skip("real.pdf", "旋转/配色渲染"); return; }

    lr::Document doc;
    if (doc.open(path) != lr::DocError::Ok) { fail("real.pdf", "旋转渲染", "打不开"); return; }
    lr::DocumentInfo info;
    if (doc.info(info) != lr::DocError::Ok) { fail("real.pdf", "旋转渲染", "info 失败"); return; }
    const int pw = static_cast<int>(info.page_width_pt + 0.5f);
    const int ph = static_cast<int>(info.page_height_pt + 0.5f);

    auto dims_ok = [](const lr::PageBitmap& b, int w, int h) {
        const int dw = b.width() - w;
        const int dh = b.height() - h;
        return dw >= -2 && dw <= 2 && dh >= -2 && dh <= 2;
    };

    lr::PageBitmap b0, b90, b180;
    if (doc.render_page(0, 1.0f, b0, 8192, 0) != lr::DocError::Ok ||
        doc.render_page(0, 1.0f, b90, 8192, 90) != lr::DocError::Ok ||
        doc.render_page(0, 1.0f, b180, 8192, 180) != lr::DocError::Ok) {
        fail("real.pdf", "旋转渲染", "render 失败");
        return;
    }
    if (dims_ok(b0, pw, ph)) pass("real.pdf", "旋转 0° 尺寸 = 原尺寸");
    else fail("real.pdf", "旋转 0° 尺寸 = 原尺寸", "尺寸不符");
    if (dims_ok(b90, ph, pw)) pass("real.pdf", "旋转 90° 宽高互换");
    else fail("real.pdf", "旋转 90° 宽高互换", "尺寸不符");
    if (dims_ok(b180, pw, ph)) pass("real.pdf", "旋转 180° 尺寸 = 原尺寸");
    else fail("real.pdf", "旋转 180° 尺寸 = 原尺寸", "尺寸不符");

    lr::PageBitmap bn, bi, be;
    if (doc.render_page(0, 1.0f, bn, 8192, 0, lr::ColorMode::Normal) != lr::DocError::Ok ||
        doc.render_page(0, 1.0f, bi, 8192, 0, lr::ColorMode::Invert) != lr::DocError::Ok ||
        doc.render_page(0, 1.0f, be, 8192, 0, lr::ColorMode::EyeCare) != lr::DocError::Ok) {
        fail("real.pdf", "配色渲染", "render 失败");
        return;
    }
    const std::uint8_t* pn = bn.samples();
    const std::uint8_t* pi = bi.samples();
    const std::uint8_t* pe = be.samples();
    if (pn[0] > 200) pass("real.pdf", "正常：背景为白");
    else fail("real.pdf", "正常：背景为白", "像素不符");
    if (pi[0] < 55) pass("real.pdf", "反色：背景变黑");
    else fail("real.pdf", "反色：背景变黑", "像素不符");
    if (pi[3] == 255 && pn[3] == 255) pass("real.pdf", "反色保留 alpha=255");
    else fail("real.pdf", "反色保留 alpha=255", "alpha 被改");
    if (pe[0] > pe[2]) pass("real.pdf", "护眼：背景为暖色（R>B）");
    else fail("real.pdf", "护眼：背景为暖色（R>B）", "像素不符");
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring dir;
    if (argc > 1) {
        wchar_t buf[1024] = {};
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, buf, 1024);
        buf[1023] = L'\0';
        dir = buf;
    } else {
        dir = L"samples";
    }

    std::printf("=== Lilith Reader 文档核心自动化测试 ===\n样本目录：");
    std::printf("%s\n", argc > 1 ? argv[1] : "samples");

    std::printf("\n-- 合法 / 改名 / 归档冒充 / 损坏（%zu 例）--\n",
                sizeof kCases / sizeof kCases[0]);
    for (const Case& c : kCases) run_case(dir, c);

    std::printf("\n-- 加密 PDF（%zu 例，需 PyMuPDF 生成样本）--\n",
                sizeof kEncCases / sizeof kEncCases[0]);
    for (const EncCase& e : kEncCases) run_enc_case(dir, e, "lilith");

    run_gate_cases();
    run_size_cases(dir);
    run_outline_cases(dir);
    run_render_cases(dir);

    std::printf("\n=== 结果：%d 通过 / %d 失败 / %d 跳过 ===\n", g_pass, g_fail, g_skip);
    if (g_fail > 0) {
        std::printf("有失败用例，详见上面的 [FAIL] 行。\n");
        return 1;
    }
    std::printf("OK\n");
    return 0;
}
