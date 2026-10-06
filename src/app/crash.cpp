// crash.cpp — 进程级崩溃防线的实现（Phase 7 稳定性加固）
//
// 本文件的代码运行在"进程已经出问题"的时刻，因此书写约束比别处严格得多：
//   · 不分配堆内存（不用 std::string / std::vector / std::wstring 于崩溃路径）；
//   · 不调用 lr::log（它有自己的锁与后台线程，此时不可信）；
//   · 只用 Win32 原生 API + 栈上的定长缓冲；
//   · 用 InterlockedCompareExchange 做"只处理一次"的闸门，避免处理器自身崩溃后无限递归。
//
// 处理器**只记录、不恢复**：写完摘要与转储就让进程退出。语义上这比"带伤继续跑"安全得多
// —— 崩溃后的进程状态是未定义的，继续运行只会制造更难查的二次故障。

#include "crash.h"

#include "app_internal.h"   // lr::exe_dir()（import lilithreader.utils）

#include <dbghelp.h>

#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <stdexcept>

namespace lr::app::crash {
namespace {

// 自定义异常码：用于让 C++ 侧的问题也走 SEH 同一条记录路径（便于统一检索）。
constexpr DWORD kCppTerminateCode = 0xE0000001u;

constexpr std::size_t kPathCap = 512;
constexpr std::size_t kLogTailBytes = 4096;   // 摘要里附带的日志尾部长度

wchar_t g_dir[kPathCap] = {};       // <exe>\crash
wchar_t g_marker[kPathCap] = {};    // <exe>\crash\last_crash.txt
wchar_t g_reported[kPathCap] = {};  // <exe>\crash\last_crash.reported.txt
wchar_t g_running[kPathCap] = {};   // <exe>\crash\running.flag
wchar_t g_log[kPathCap] = {};       // <exe>\logs\LilithReader.log

volatile LONG g_installed = 0;
volatile LONG g_entered = 0;
std::atomic<const char*> g_phase{ "startup" };

// ---- 定长字符串小工具（崩溃路径专用，绝不分配）----

std::size_t append_w(wchar_t* dst, std::size_t cap, std::size_t pos,
                     const wchar_t* src) noexcept {
    if (dst == nullptr || cap == 0) return 0;
    if (pos >= cap) pos = cap - 1;
    if (src) {
        while (*src != L'\0' && pos + 1 < cap) dst[pos++] = *src++;
    }
    dst[pos] = L'\0';
    return pos;
}

std::size_t copy_a(char* dst, std::size_t cap, const char* src) noexcept {
    if (dst == nullptr || cap == 0) return 0;
    std::size_t n = 0;
    if (src) {
        while (src[n] != '\0' && n + 1 < cap) {
            dst[n] = src[n];
            ++n;
        }
    }
    dst[n] = '\0';
    return n;
}

bool file_exists(const wchar_t* path) noexcept {
    const DWORD a = ::GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 把 <exe>\crash\ 与 <exe>\logs\ 下的各个路径一次算好。
void ensure_paths() noexcept {
    if (g_marker[0] != L'\0') return;
    const std::wstring dir = lr::exe_dir();

    std::size_t n = append_w(g_dir, kPathCap, 0, dir.c_str());
    n = append_w(g_dir, kPathCap, n, L"crash\\");
    (void)n;
    append_w(g_marker, kPathCap, 0, g_dir);
    append_w(g_marker, kPathCap, wcslen(g_marker), L"last_crash.txt");
    append_w(g_reported, kPathCap, 0, g_dir);
    append_w(g_reported, kPathCap, wcslen(g_reported), L"last_crash.reported.txt");
    append_w(g_running, kPathCap, 0, g_dir);
    append_w(g_running, kPathCap, wcslen(g_running), L"running.flag");
    append_w(g_log, kPathCap, 0, dir.c_str());
    append_w(g_log, kPathCap, wcslen(g_log), L"logs\\LilithReader.log");
}

void timestamp(char* out, std::size_t cap) noexcept {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    std::snprintf(out, cap, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                  static_cast<unsigned>(st.wMilliseconds));
}

void timestamp_compact(char* out, std::size_t cap) noexcept {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    std::snprintf(out, cap, "%04u%02u%02u_%02u%02u%02u",
                  static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
}

// 把最后若干字节的日志追加到报告里：崩溃前几个事件往往就是根因。
void append_log_tail(HANDLE out) noexcept {
    if (!file_exists(g_log)) return;
    HANDLE in = ::CreateFileW(g_log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return;

    LARGE_INTEGER size{};
    if (::GetFileSizeEx(in, &size) && size.QuadPart > 0) {
        const LONGLONG skip =
            size.QuadPart > static_cast<LONGLONG>(kLogTailBytes)
                ? size.QuadPart - static_cast<LONGLONG>(kLogTailBytes)
                : 0;
        LARGE_INTEGER pos{};
        pos.QuadPart = skip;
        if (::SetFilePointerEx(in, pos, nullptr, FILE_BEGIN)) {
            char buf[kLogTailBytes + 1] = {};
            DWORD read = 0;
            if (::ReadFile(in, buf, static_cast<DWORD>(kLogTailBytes), &read, nullptr) && read > 0) {
                DWORD wrote = 0;
                ::WriteFile(out, buf, read, &wrote, nullptr);
            }
        }
    }
    ::CloseHandle(in);
}

// 写 MiniDump。dbghelp 动态加载且只从 System32 取，避免 DLL 劫持。
bool write_dump(wchar_t* out_path, std::size_t cap, EXCEPTION_POINTERS* ep) noexcept {
    if (out_path == nullptr || cap == 0) return false;
    out_path[0] = L'\0';

    HMODULE dbg = ::LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dbg == nullptr) dbg = ::LoadLibraryExW(L"dbghelp.dll", nullptr, 0);   // 老系统兜底
    if (dbg == nullptr) return false;

    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                              PMINIDUMP_EXCEPTION_INFORMATION,
                                              PMINIDUMP_USER_STREAM_INFORMATION,
                                              PMINIDUMP_CALLBACK_INFORMATION);
    const auto fn = reinterpret_cast<MiniDumpWriteDumpFn>(
        reinterpret_cast<void*>(::GetProcAddress(dbg, "MiniDumpWriteDump")));
    if (fn == nullptr) {
        ::FreeLibrary(dbg);
        return false;
    }

    char stamp[32] = {};
    timestamp_compact(stamp, sizeof stamp);
    std::size_t pos = append_w(out_path, cap, 0, g_dir);
    pos = append_w(out_path, cap, pos, L"LilithReader_");
    for (const char* p = stamp; *p != '\0' && pos + 1 < cap; ++p)
        out_path[pos++] = static_cast<wchar_t>(*p);
    out_path[pos] = L'\0';
    pos = append_w(out_path, cap, pos, L".dmp");
    (void)pos;

    HANDLE f = ::CreateFileW(out_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        ::FreeLibrary(dbg);
        out_path[0] = L'\0';
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION info{};
    MINIDUMP_EXCEPTION_INFORMATION* pinfo = nullptr;
    if (ep != nullptr) {
        info.ThreadId = ::GetCurrentThreadId();
        info.ExceptionPointers = ep;
        info.ClientPointers = FALSE;
        pinfo = &info;
    }
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithDataSegs |
                                                 MiniDumpWithThreadInfo |
                                                 MiniDumpWithUnloadedModules |
                                                 MiniDumpWithHandleData);
    const BOOL ok = fn(::GetCurrentProcess(), ::GetCurrentProcessId(), f, type, pinfo,
                       nullptr, nullptr);
    ::CloseHandle(f);
    ::FreeLibrary(dbg);
    if (!ok) {
        out_path[0] = L'\0';
        return false;
    }
    return true;
}

// 写人类可读摘要（下次启动据此提示用户）。
void write_report(const char* reason, DWORD code, const char* detail, const void* address,
                  const wchar_t* dump_path) noexcept {
    HANDLE f = ::CreateFileW(g_marker, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;

    char head[640] = {};
    char ts[40] = {};
    timestamp(ts, sizeof ts);
    const char* ph = g_phase.load(std::memory_order_relaxed);
    std::snprintf(head, sizeof head,
                  "==== Lilith Reader 异常退出 ====\n"
                  "time=%s\n"
                  "reason=%s\n"
                  "phase=%s\n"
                  "code=0x%08lX\n"
                  "address=%p\n"
                  "thread=%lu\n"
                  "process=%lu\n",
                  ts, reason ? reason : "?", ph ? ph : "?",
                  static_cast<unsigned long>(code), address,
                  static_cast<unsigned long>(::GetCurrentThreadId()),
                  static_cast<unsigned long>(::GetCurrentProcessId()));
    DWORD wrote = 0;
    ::WriteFile(f, head, static_cast<DWORD>(std::strlen(head)), &wrote, nullptr);

    if (detail != nullptr && detail[0] != '\0') {
        const char* p = "detail=";
        ::WriteFile(f, p, 7, &wrote, nullptr);
        ::WriteFile(f, detail, static_cast<DWORD>(std::strlen(detail)), &wrote, nullptr);
        const char* nl = "\n";
        ::WriteFile(f, nl, 1, &wrote, nullptr);
    }
    if (dump_path != nullptr && dump_path[0] != L'\0') {
        // dump 文件名是 ASCII，逐字符降位输出即可（避免再引一次宽转窄）。
        char name[64] = {};
        std::size_t i = 0;
        const wchar_t* base = wcsrchr(dump_path, L'\\');
        base = base ? base + 1 : dump_path;
        for (; *base != L'\0' && i + 1 < sizeof name; ++base)
            name[i++] = (*base < 128) ? static_cast<char>(*base) : '?';
        name[i] = '\0';
        ::WriteFile(f, "dump=", 5, &wrote, nullptr);
        ::WriteFile(f, name, static_cast<DWORD>(std::strlen(name)), &wrote, nullptr);
        ::WriteFile(f, "\n", 1, &wrote, nullptr);
    }

    const char* sep = "\n---- 日志尾部（最后 4096 字节）----\n";
    ::WriteFile(f, sep, static_cast<DWORD>(std::strlen(sep)), &wrote, nullptr);
    append_log_tail(f);

    ::FlushFileBuffers(f);
    ::CloseHandle(f);
}

// 所有崩溃路径的唯一入口：记录一次，然后结束进程。
// **不返回**（TerminateProcess 兜底），因此调用方无需考虑"处理器返回后怎么办"。
void handle(const char* reason, DWORD code, const char* detail, const void* address,
            EXCEPTION_POINTERS* ep) noexcept {
    if (::InterlockedCompareExchange(&g_entered, 1, 0) != 0) {
        // 处理器自身再次崩溃：立刻结束，不再尝试记录。
        ::TerminateProcess(::GetCurrentProcess(), code ? code : 1u);
        return;
    }
    if (g_dir[0] == L'\0') ensure_paths();
    if (g_dir[0] != L'\0') {
        wchar_t dump[kPathCap] = {};
        const bool have_dump = write_dump(dump, kPathCap, ep);
        write_report(reason, code, detail, address, have_dump ? dump : nullptr);
    }
    ::TerminateProcess(::GetCurrentProcess(), code ? code : 1u);
}

// ---- 处理器 ----

void __cdecl on_terminate() noexcept {
    char detail[256] = {};
    copy_a(detail, sizeof detail, "uncaught exception");
    try {
        if (std::current_exception() != nullptr) std::rethrow_exception(std::current_exception());
    } catch (const std::exception& ex) {
        copy_a(detail, sizeof detail, ex.what());
    } catch (...) {
        copy_a(detail, sizeof detail, "non-std exception");
    }
    handle("terminate", kCppTerminateCode, detail, nullptr, nullptr);
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* ep) noexcept {
    const EXCEPTION_RECORD* rec = (ep != nullptr) ? ep->ExceptionRecord : nullptr;
    const DWORD code = rec ? rec->ExceptionCode : 0u;
    const void* address = rec ? rec->ExceptionAddress : nullptr;
    char detail[64] = {};
    if (code == static_cast<DWORD>(0xC0000005u)) {
        const ULONG_PTR kind =
            (rec->ExceptionInformation[0] != 0) ? static_cast<ULONG_PTR>(1) : 0;   // 1=写 0=读
        std::snprintf(detail, sizeof detail, "access violation (%s) at %p",
                      kind ? "write" : "read", address);
    } else if (code == static_cast<DWORD>(0xC0000094u)) {
        copy_a(detail, sizeof detail, "integer divide by zero");
    } else if (code == static_cast<DWORD>(0xC00000FDu)) {
        copy_a(detail, sizeof detail, "stack overflow");
    } else if (code == kCppTerminateCode) {
        copy_a(detail, sizeof detail, "C++ terminate");
    }
    handle("SEH", code, detail, address, ep);
    return EXCEPTION_EXECUTE_HANDLER;   // 不会到达：handle() 已结束进程
}

void __cdecl on_purecall() noexcept {
    handle("purecall", 0xE0000002u, "pure virtual function call", nullptr, nullptr);
}

void __cdecl on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*,
                                  unsigned int, uintptr_t) noexcept {
    handle("invalid-parameter", 0xE0000003u, "CRT invalid parameter", nullptr, nullptr);
}

// abort() 在 MSVC 上不经过 std::terminate；而 STL 的契约检查、_STL_VERIFY 等大量走 abort()。
// 不接这一条，这些失败就会"无声消失"，与本次加固的目标正相反。
void __cdecl on_abort(int) noexcept {
    handle("abort", 0xE0000004u, "abort() called", nullptr, nullptr);
}

void write_running_flag() noexcept {
    HANDLE f = ::CreateFileW(g_running, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[128] = {};
    char ts[40] = {};
    timestamp(ts, sizeof ts);
    std::snprintf(buf, sizeof buf, "pid=%lu\nstarted=%s\n",
                  static_cast<unsigned long>(::GetCurrentProcessId()), ts);
    DWORD wrote = 0;
    ::WriteFile(f, buf, static_cast<DWORD>(std::strlen(buf)), &wrote, nullptr);
    ::CloseHandle(f);
}

}  // namespace

void set_phase(const char* p) noexcept {
    g_phase.store(p, std::memory_order_relaxed);
}

const char* phase() noexcept {
    return g_phase.load(std::memory_order_relaxed);
}

bool handling() noexcept {
    return ::InterlockedCompareExchange(&g_entered, 0, 0) != 0;
}

void install() noexcept {
    try {
        if (::InterlockedCompareExchange(&g_installed, 1, 0) != 0) return;
        ensure_paths();
        ::CreateDirectoryW(g_dir, nullptr);   // 已存在则失败，忽略

        // 缺省 CRT 无效参数处理器会直接终止进程并且不留下任何痕迹：换成我们的记录路径。
        ::_set_invalid_parameter_handler(on_invalid_parameter);
        ::_set_purecall_handler(on_purecall);
        std::set_terminate(on_terminate);
        ::SetUnhandledExceptionFilter(on_unhandled);

        // 让 CRT 在 abort() 时不要弹 "abort/retry/ignore"，统一走我们的记录路径。
        ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        ::signal(SIGABRT, on_abort);

        write_running_flag();
    } catch (...) {
    }
}

void mark_clean_exit() noexcept {
    if (g_running[0] != L'\0') ::DeleteFileW(g_running);
}

bool take_last_crash(std::string& summary_utf8) noexcept {
    summary_utf8.clear();
    ensure_paths();
    if (g_marker[0] == L'\0') return false;

    const bool have_report = file_exists(g_marker);
    const bool have_running = file_exists(g_running);

    if (!have_report) {
        if (!have_running) return false;

        // **自检**：标记若是**本次进程**自己写的（调用顺序不当就会这样），不能算"上次异常"。
        // install() 在启动时写 running.flag；若 take_last_crash 排在它之后，看到的永远是自己
        // —— 结果就是"每次启动都提示上次异常退出"（实测踩过）。故读出 pid 比对当前进程。
        {
            HANDLE f = ::CreateFileW(g_running, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                     OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                char buf[128] = {};
                DWORD read = 0;
                if (::ReadFile(f, buf, static_cast<DWORD>(sizeof buf - 1), &read, nullptr) && read > 0) {
                    buf[read] = '\0';
                    // 手工解析 "pid=%lu"：崩溃路径不引 CRT 的 scanf 家族（会触发 C4996 且无必要）。
                    const char* p = std::strstr(buf, "pid=");
                    unsigned long pid = 0;
                    if (p != nullptr) {
                        p += 4;
                        while (*p >= '0' && *p <= '9')
                            pid = pid * 10u + static_cast<unsigned long>(*p++ - '0');
                    }
                    if (pid != 0 &&
                        pid == static_cast<unsigned long>(::GetCurrentProcessId()))
                    {
                        ::CloseHandle(f);
                        return false;   // 是自己的标记：留给 mark_clean_exit() 去删
                    }
                }
                ::CloseHandle(f);
            }
        }

        // 上次在"运行中"状态下消失，却没有机会执行崩溃处理器（强杀 / 断电 / 蓝屏）。
        summary_utf8 =
            "上次运行未正常退出（进程被强制结束或系统异常）。\n"
            "这次没有留下崩溃转储，若反复出现请检查系统事件日志。";
        ::DeleteFileW(g_running);
        return true;
    }

    HANDLE f = ::CreateFileW(g_marker, GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        char buf[8192] = {};
        DWORD read = 0;
        if (::ReadFile(f, buf, static_cast<DWORD>(sizeof buf - 1), &read, nullptr) && read > 0) {
            buf[read] = '\0';
            try {
                summary_utf8.assign(buf, read);
            } catch (...) {
                summary_utf8.clear();
            }
        }
        ::CloseHandle(f);
    }
    if (summary_utf8.empty())
        summary_utf8 = "上次运行异常退出（详情见 crash\\last_crash.txt）。";

    // 归档，保证只提示一次；下一次崩溃会重新生成 last_crash.txt。
    ::DeleteFileW(g_reported);
    ::MoveFileExW(g_marker, g_reported, MOVEFILE_REPLACE_EXISTING);
    if (have_running) ::DeleteFileW(g_running);
    return true;
}

std::wstring report_dir() noexcept {
    ensure_paths();
    return std::wstring(g_dir);
}

#ifndef NDEBUG
void debug_trigger(int kind) {
    if (kind == 0) {
        // 不在任何 try/catch 内调用 → 触发 set_terminate。
        throw std::runtime_error("debug_trigger: uncaught C++ exception");
    }
    // 主动访问违规 → 触发 SetUnhandledExceptionFilter。
    volatile int* p = nullptr;
    *p = 1;
}
#endif

}  // namespace lr::app::crash
