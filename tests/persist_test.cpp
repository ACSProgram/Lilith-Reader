// persist_test.cpp — 阅读状态异步持久化服务自动化测试（ADR-082）
//
// 做法：**直接链接 lilithreader.persist 与 lilithreader.reader_state 模块**，在真实线程边界上
// 断言服务的语义：防抖合并、flush 立即落盘、写失败上报、析构 flush、原子写入的字节往返。
//
// 为什么要真跑线程：持久化的价值就在"UI 线程不写盘、写盘在工作线程"，这是跨线程行为，
// 纯函数单测覆盖不到；这里让工作线程真实起停，用轮询把"什么时候落盘"变成可断言的现象。
//
// 退出码：0 = 全部通过，1 = 有 FAIL。
// 用法： persist_test.exe

#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

import lilithreader.reader_state;
import lilithreader.persist;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const char* name) {
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", name); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", name); }
}

// ---- 临时文件路径 ----
std::wstring temp_dir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetTempPathW(MAX_PATH, buf);
    return std::wstring(buf, n);
}

bool file_exists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 构造一份带单条记录的阅读状态
lr::ReaderState make_state(int page, int columns) {
    lr::ReaderState s;
    lr::DocRecord& r = s.upsert(0x1234ull);
    r.page = page;
    r.columns = columns;
    r.rotation = 90;
    r.bookmarks.push_back(lr::Bookmark{ 3, "记号" });
    return s;
}

// 轮询直到 dirty() 为假（或超时）。返回是否在超时前落定。
bool wait_written(lr::PersistService& svc, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (!svc.dirty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return !svc.dirty();
}

}  // namespace

int main() {
    const std::wstring dir = temp_dir();

    // ---- 1. write_state_bytes / load_state 字节往返 ----
    {
        std::printf("[1] write_state_bytes 与 load_state 往返\n");
        const std::wstring p = dir + L"lilithreader_persist_roundtrip.bin";
        const lr::ReaderState s = make_state(7, 3);
        check(lr::write_state_bytes(p, lr::encode_state(s)), "write_state_bytes 成功");
        const lr::ReaderState back = lr::load_state(p);
        const lr::DocRecord* r = back.find(0x1234ull);
        check(r != nullptr, "读回的记录存在");
        check(r && r->page == 7 && r->columns == 3 && r->rotation == 90, "读回字段一致");
        check(r && r->bookmarks.size() == 1 && r->bookmarks[0].label == "记号", "读回书签一致");
        DeleteFileW(p.c_str());
    }

    // ---- 2. 防抖：request_save 后经防抖窗口由工作线程落盘 ----
    {
        std::printf("[2] 防抖合并后落盘\n");
        const std::wstring p = dir + L"lilithreader_persist_debounce.bin";
        DeleteFileW(p.c_str());
        lr::PersistService svc(p, 60);
        svc.request_save(make_state(11, 2));
        check(svc.dirty(), "request_save 后为脏");
        check(wait_written(svc, 3000), "防抖窗口后自动落盘（dirty 转清）");
        check(file_exists(p), "文件已生成");
        const lr::ReaderState back = lr::load_state(p);
        const lr::DocRecord* r = back.find(0x1234ull);
        check(r && r->page == 11, "落盘内容正确");
        DeleteFileW(p.c_str());
    }

    // ---- 3. flush 立即落盘（越过长防抖窗口） ----
    {
        std::printf("[3] flush 立即落盘\n");
        const std::wstring p = dir + L"lilithreader_persist_flush.bin";
        DeleteFileW(p.c_str());
        lr::PersistService svc(p, 60000);   // 防抖 60s：不 flush 就当没写
        svc.request_save(make_state(21, 4));
        svc.flush();
        check(!svc.dirty(), "flush 后不再为脏");
        check(file_exists(p), "flush 后文件已生成");
        const lr::ReaderState back = lr::load_state(p);
        const lr::DocRecord* r = back.find(0x1234ull);
        check(r && r->page == 21 && r->columns == 4, "flush 落盘内容正确");
        DeleteFileW(p.c_str());
    }

    // ---- 4. 写失败上报：路径所在目录不存在 ----
    {
        std::printf("[4] 写失败上报\n");
        const std::wstring bad = dir + L"lilithreader_no_such_dir_zzz\\state.bin";
        lr::PersistService svc(bad, 60);
        svc.request_save(make_state(1, 1));
        svc.flush();
        check(svc.last_save_failed(), "写失败被如实上报");
        DeleteFileW((bad + L".tmp").c_str());
    }

    // ---- 5. 析构 flush：作用域退出即落盘 ----
    {
        std::printf("[5] 析构 flush\n");
        const std::wstring p = dir + L"lilithreader_persist_dtor.bin";
        DeleteFileW(p.c_str());
        {
            lr::PersistService svc(p, 60000);   // 长防抖：只有析构 flush 能救它
            svc.request_save(make_state(31, 1));
        }   // 析构
        check(file_exists(p), "析构后文件已生成");
        const lr::ReaderState back = lr::load_state(p);
        const lr::DocRecord* r = back.find(0x1234ull);
        check(r && r->page == 31, "析构 flush 内容正确");
        DeleteFileW(p.c_str());
    }

    // ---- 6. 空状态也能落盘（合法：LRS3 + 0 条记录） ----
    {
        std::printf("[6] 空状态落盘\n");
        const std::wstring p = dir + L"lilithreader_persist_empty.bin";
        DeleteFileW(p.c_str());
        lr::PersistService svc(p, 60);
        svc.request_save(lr::ReaderState{});
        svc.flush();
        check(!svc.last_save_failed() && file_exists(p), "空状态落盘成功");
        check(lr::load_state(p).docs.empty(), "空状态读回为空");
        DeleteFileW(p.c_str());
    }

    // ---- 7. 连续多次 request_save 合并为最终一次内容 ----
    {
        std::printf("[7] 多次快照合并为最终内容\n");
        const std::wstring p = dir + L"lilithreader_persist_coalesce.bin";
        DeleteFileW(p.c_str());
        lr::PersistService svc(p, 80);
        for (int i = 1; i <= 5; ++i) svc.request_save(make_state(i, 1));
        check(wait_written(svc, 3000), "合并后落定");
        const lr::ReaderState back = lr::load_state(p);
        const lr::DocRecord* r = back.find(0x1234ull);
        check(r && r->page == 5, "落盘的是最后一次快照");
        DeleteFileW(p.c_str());
    }

    std::printf("\npersist_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}