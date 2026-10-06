#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

import lilithreader.document;

namespace fs = std::filesystem;

static std::wstring wide(const char* s) {
    wchar_t buf[4096] = {};
    MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 4096);
    return buf;
}

static unsigned long long ws_bytes() {
    PROCESS_MEMORY_COUNTERS_EX c{};
    c.cb = sizeof c;
    return GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c),
                                sizeof c) ? static_cast<unsigned long long>(c.WorkingSetSize) : 0;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::vector<fs::path> files;
    for (int ai = 1; ai < argc; ++ai) {
        const fs::path root = wide(argv[ai]);
        if (!fs::exists(root)) continue;
        if (fs::is_regular_file(root)) files.push_back(root);
        else for (const auto& e : fs::directory_iterator(root))
            if (e.is_regular_file()) files.push_back(e.path());
    }
    std::printf("file,pages,scale,page,open_ms,render_ms,width,height,bytes,working_set,mode,workers\n");
    for (const fs::path& path : files) {
        lr::Document doc;
        const auto t0 = std::chrono::steady_clock::now();
        const lr::DocError oe = doc.open(path.wstring());
        const auto t1 = std::chrono::steady_clock::now();
        lr::DocumentInfo info;
        if (oe != lr::DocError::Ok || doc.info(info) != lr::DocError::Ok || info.page_count <= 0) continue;
        const int picks[3] = { 0, info.page_count / 2, info.page_count - 1 };
        for (float scale : { 0.75f, 1.0f, 2.0f }) {
            for (int pi = 0; pi < 3; ++pi) {
                const int page = picks[pi];
                lr::PageBitmap bmp;
                const auto r0 = std::chrono::steady_clock::now();
                const lr::DocError re = doc.render_page(page, scale, bmp, 2048);
                const auto r1 = std::chrono::steady_clock::now();
                if (re != lr::DocError::Ok) continue;
                const auto open_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
                const auto render_ms = std::chrono::duration<double, std::milli>(r1 - r0).count();
                const unsigned long long bytes = static_cast<unsigned long long>(bmp.width()) *
                    static_cast<unsigned long long>(bmp.height()) * 4ull;
                std::printf("\"%ls\",%d,%.2f,%d,%.3f,%.3f,%d,%d,%llu,%llu,serial,%d\n",
                            path.filename().wstring().c_str(), info.page_count, scale, page,
                            open_ms, render_ms, bmp.width(), bmp.height(), bytes, ws_bytes(), 1);
            }
        }
        // 仅测“每线程独立 Document/context”的上限，不接入 UI/Renderer。
        // 这能回答并发是否值得接入，而不会把共享 fz_document 的不安全用法带进产品。
        if (info.page_count >= 3) {
            for (int workers : { 2, 3 }) {
                std::vector<lr::Document> docs(static_cast<std::size_t>(workers));
                bool opened = true;
                for (lr::Document& d : docs)
                    opened = opened && d.open(path.wstring()) == lr::DocError::Ok;
                if (!opened) continue;
                std::vector<lr::PageBitmap> bitmaps(static_cast<std::size_t>(workers));
                const auto p0 = std::chrono::steady_clock::now();
                std::vector<std::thread> ts;
                for (int wi = 0; wi < workers; ++wi) {
                    ts.emplace_back([&, wi] {
                        docs[static_cast<std::size_t>(wi)].render_page(
                            wi % info.page_count, 1.0f, bitmaps[static_cast<std::size_t>(wi)], 2048);
                    });
                }
                for (auto& t : ts) t.join();
                const auto p1 = std::chrono::steady_clock::now();
                const auto parallel_ms = std::chrono::duration<double, std::milli>(p1 - p0).count();
                std::printf("\"%ls\",%d,1.00,-1,0.000,%.3f,0,0,0,%llu,parallel,%d\n",
                            path.filename().wstring().c_str(), info.page_count, parallel_ms,
                            ws_bytes(), workers);
            }
        }
    }
    return 0;
}
