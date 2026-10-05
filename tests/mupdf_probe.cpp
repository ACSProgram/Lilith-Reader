// mupdf_probe.cpp — MuPDF 识别行为诊断探针（常备排查工具）
//
// 与 doc_test.cpp 的区别：doc_test 断言**本项目**的行为，本探针直接问 MuPDF 本身，
// 打印它对每个样本的真实判定（不经过 Lilith Reader 的任何逻辑）：
//   1) 用扩展名 magic 打开（与主程序一致），打印 FZ_META_FORMAT / 页数 / 首页尺寸；
//   2) 再用 magic="" 打开（纯内容嗅探）做对照。
// 排查"某个文件为什么被认成 X"时先跑它，能立刻区分"MuPDF 的判定"与"我们的策略"。
//
// 用法： mupdf_probe.exe <samples 目录>
// 纪律与 document.cpp 相同：fz_try 内只有 POD 与裸 fz_* 指针。

#define NOMINMAX
#include <windows.h>

#include <mupdf/fitz.h>

#include <cstdio>
#include <cstring>
#include <string>

#pragma warning(disable : 4611)

namespace {

struct Result {
    bool  opened = false;
    int   pages = 0;
    int   caught = 0;
    char  format[64] = {};
    char  message[256] = {};
    float w = 0.0f, h = 0.0f;
};

constexpr std::size_t kFormatCap = sizeof(Result::format);
constexpr std::size_t kMessageCap = sizeof(Result::message);

// 返回 void + 出参：避免 setjmp 干扰 MSVC 的返回值分析（曾导致 C4715 与运行时崩溃）
void probe(fz_context* ctx, const wchar_t* path, const char* magic, Result& out) noexcept {
    fz_stream*   stm = nullptr;
    fz_document* doc = nullptr;
    Result       r;

    fz_try(ctx) {
        fz_var(stm);
        fz_var(doc);
        fz_var(r);

        stm = fz_open_file_w(ctx, path);
        doc = fz_open_document_with_stream(ctx, magic, stm);
        if (doc == nullptr)
            fz_throw(ctx, FZ_ERROR_GENERIC, "null document");
        r.opened = true;
        if (fz_lookup_metadata(ctx, doc, FZ_META_FORMAT, r.format, kFormatCap) < 0)
            std::strncpy(r.format, "(unsupported)", kFormatCap - 1);
        r.pages = fz_count_pages(ctx, doc);
        if (r.pages > 0) {
            fz_page* p = fz_load_page(ctx, doc, 0);
            const fz_rect b = fz_bound_page(ctx, p);
            fz_drop_page(ctx, p);
            r.w = b.x1 - b.x0;
            r.h = b.y1 - b.y0;
        }
    }
    fz_always(ctx) {
        if (stm) {
            fz_drop_stream(ctx, stm);
            stm = nullptr;
        }
    }
    fz_catch(ctx) {
        r.caught = fz_caught(ctx);
        const char* m = fz_caught_message(ctx);
        std::snprintf(r.message, kMessageCap, "%s", m ? m : "(null)");
    }

    if (doc) fz_drop_document(ctx, doc);
    out = r;
}

void report(const char* magic, const Result& r) {
    char m2[16] = {};
    std::snprintf(m2, sizeof m2, "%s", magic[0] ? magic : "\"\"");
    if (r.opened) {
        std::printf("      magic=%-7s => OPEN  format=%-16s pages=%-4d %.0fx%.0f pt\n",
                    m2, r.format, r.pages, r.w, r.h);
    } else {
        std::printf("      magic=%-7s => FAIL  err[%d] %s\n", m2, r.caught, r.message);
    }
    std::fflush(stdout);
}

// 与主程序一致：小写扩展名（含点），非 ASCII 扩展名返回空串（交给内容嗅探）
void extension_magic(const std::wstring& path, char (&out)[16]) {
    out[0] = '\0';
    const std::size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return;
    const std::size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos && dot < sep) return;

    std::size_t n = 0;
    for (std::size_t i = dot; i < path.size() && n + 1 < sizeof(out); ++i) {
        const wchar_t wc = path[i];
        if (wc < 0x21 || wc > 0x7E) { out[0] = '\0'; return; }
        char c = static_cast<char>(wc);
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out[n++] = c;
    }
    out[n] = '\0';
}

}  // namespace

int main(int argc, char** argv) {
    std::wstring wdir = L"samples";
    if (argc > 1) {
        wchar_t buf[1024] = {};
        MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, buf, 1024);
        buf[1023] = L'\0';
        wdir = buf;
    }

    fz_context* ctx = fz_new_context(nullptr, nullptr, 64u << 20);
    if (ctx == nullptr) { std::printf("no context\n"); return 1; }

    std::printf("=== MuPDF 识别探针（绕过 Lilith Reader，直接问 MuPDF）===\n");
    fz_try(ctx) {
        fz_register_document_handlers(ctx);
    }
    fz_catch(ctx) { std::printf("register handlers failed\n"); }

    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = wdir + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("no samples in %ls\n", wdir.c_str());
        return 1;
    }

    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::wstring path = wdir + L"\\" + fd.cFileName;
        char name[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, nullptr, nullptr);
        if (name[0] == '_') continue;  // 跳过 _manifest.txt

        std::printf("  %s\n", name);
        std::fflush(stdout);
        char magic[16] = {};
        extension_magic(path, magic);
        Result a, b;
        probe(ctx, path.c_str(), magic, a);
        report(magic, a);
        probe(ctx, path.c_str(), "", b);
        report("", b);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    fz_drop_context(ctx);
    std::printf("=== done ===\n");
    return 0;
}
