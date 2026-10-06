// log.cpp — lilithreader.log 的实现单元（Phase 7 稳定性加固）
//
// 结构：写入侧只做「拼行 + 入队 + 通知」，落盘侧是唯一的后台线程（轮转也在这里做）。
// 因此调用线程永远不碰磁盘，满足 UI 线程零阻塞。
//
// 一切失败都静默退化：日志文件打不开就不记录，队列溢出就丢最旧的并计数，
// 写盘失败连续多次就停用文件输出 —— 日志层自身绝不能成为新的崩溃源。
// 单文件上限 2 MiB，保留 1 份历史（`.1`），故 logs\ 目录最大占用约 4 MiB。

module;

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

module lilithreader.log;

namespace lr::log {
namespace {

constexpr std::size_t kMaxBytes = 2u * 1024u * 1024u;   // 单文件上限
constexpr std::size_t kMaxQueue = 4096;                 // 待写队列上限（超出丢最旧）
constexpr int         kMaxWriteFailures = 8;            // 连续失败上限，超过即停用文件输出
// 轮转失败后的退避步长：另一实例仍持有日志句柄时改名会失败（未开 FILE_SHARE_DELETE），
// 此时不能每条日志都重试一次改名（纯浪费）；等文件再长出这么多再试。
constexpr std::size_t kRotateRetrySlack = 1024u * 1024u;

constexpr const wchar_t* kLogFileName = L"LilithReader.log";

struct State {
    std::mutex                       mtx;
    std::condition_variable_any      cv;
    std::deque<std::string>          q;
    std::atomic<Level>               min_level{ Level::Info };
    std::atomic<unsigned long long>  dropped{ 0 };
    std::atomic<bool>                file_ok{ false };
    std::jthread                     writer;
    HANDLE                           file = INVALID_HANDLE_VALUE;
    std::wstring                     dir;    // ...\logs
    std::wstring                     path;   // ...\logs\LilithReader.log
    std::size_t                      written = 0;
    std::size_t                      rotate_retry_from = 0;  // 轮转失败后的下次尝试点
    int                              write_failures = 0;
};

// 进程生命周期内只创建一次、**永不销毁**：避免静态析构顺序问题
// （崩溃处理器或在其他静态析构里写日志时，state 仍然有效）。
State*& slot() noexcept {
    static State* p = nullptr;
    return p;
}
State* state() noexcept { return slot(); }

constexpr Level default_level() noexcept {
#ifdef _DEBUG
    return Level::Debug;
#else
    return Level::Info;
#endif
}

char level_char(Level lv) noexcept {
    switch (lv) {
    case Level::Debug: return 'D';
    case Level::Info:  return 'I';
    case Level::Warn:  return 'W';
    case Level::Error: return 'E';
    }
    return '?';
}

// 模块名归一成 8 列（超长截断、不足补空格），让日志肉眼可对齐、也便于 grep。
void copy_module_padded(char (&out)[9], const char* module) noexcept {
    std::size_t n = 0;
    if (module) {
        while (module[n] != '\0' && n < 8) {
            out[n] = module[n];
            ++n;
        }
    }
    while (n < 8) out[n++] = ' ';
    out[8] = '\0';
}

std::string format_line(Level lv, const char* module, std::string_view msg) noexcept {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char ts[32] = {};
    std::snprintf(ts, sizeof ts, "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                  static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                  static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond),
                  static_cast<unsigned>(st.wMilliseconds));

    char mod[9] = {};
    copy_module_padded(mod, module);

    // pid 进行头：本应用是单进程单文档，但**多实例并存是允许的**（没有任何单实例闸），
    // 此时两个进程写同一个日志文件、行会交错——没有 pid 就无法区分哪行属于哪个实例。
    char head[96] = {};
    std::snprintf(head, sizeof head, "%s [%c] [%s] pid=%lu tid=",
                  ts, level_char(lv), mod,
                  static_cast<unsigned long>(GetCurrentProcessId()));

    std::string line;
    try {
        const std::string tid = std::to_string(static_cast<unsigned long>(GetCurrentThreadId()));
        line.reserve(std::strlen(head) + tid.size() + msg.size() + 2);
        line.append(head);
        line.append(tid);
        line.push_back(' ');
        line.append(msg.data(), msg.size());
        line.push_back('\n');
    } catch (...) {
        return {};   // 拼行失败：放弃这一条，绝不影响调用方
    }
    return line;
}

void output_debugger(const char* module, std::string_view msg) noexcept {
    if (!::IsDebuggerPresent()) return;
    std::string s;
    try {
        s.reserve(msg.size() + 24);
        s.append("[lilith:").append(module ? module : "?").append("] ");
        s.append(msg.data(), msg.size());
        s.push_back('\n');
    } catch (...) {
        return;
    }
    ::OutputDebugStringA(s.c_str());
}

bool open_log_file(State* s) noexcept {
    s->path = s->dir + L"\\" + kLogFileName;
    s->file = ::CreateFileW(s->path.c_str(), FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s->file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    s->written = ::GetFileSizeEx(s->file, &sz) ? static_cast<std::size_t>(sz.QuadPart) : 0;
    s->write_failures = 0;
    return true;
}

void close_log_file(State* s) noexcept {
    if (s->file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(s->file);
        s->file = INVALID_HANDLE_VALUE;
    }
}

// 大小轮转：LilithReader.log → LilithReader.log.1（覆盖旧的 .1）。
// **跨进程语义**：日志以 FILE_APPEND_DATA + 共享读写打开，多实例同时运行时各进程的追加
// 在 NTFS 上按单次 WriteFile 原子落到文件尾，行不会被撕开；但**轮转**是竞争点——改名要求
// 所有句柄都以 FILE_SHARE_DELETE 打开，而另一实例没这么开，改名会失败。失败的处置是退避：
// 记下本次大小，等文件再长出 kRotateRetrySlack 再试（本会话内日志可能暂时超过上限，
// 但不会反复空转改名，也不会丢行）。
void rotate_if_needed(State* s) noexcept {
    if (s->file == INVALID_HANDLE_VALUE || s->written < kMaxBytes) return;
    if (s->written < s->rotate_retry_from) return;   // 上次失败后仍在退避期内
    close_log_file(s);
    std::wstring backup = s->path;
    backup += L".1";
    ::DeleteFileW(backup.c_str());
    if (!::MoveFileExW(s->path.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING))
        s->rotate_retry_from = s->written + kRotateRetrySlack;
    open_log_file(s);
}

void write_raw(State* s, const char* data, std::size_t n) noexcept {
    if (s->file == INVALID_HANDLE_VALUE || n == 0) return;
    std::size_t off = 0;
    while (off < n) {
        const std::size_t left = n - off;
        DWORD chunk = static_cast<DWORD>(left > 0x7FFFFFFFu ? 0x7FFFFFFFu : left);
        DWORD wrote = 0;
        if (!::WriteFile(s->file, data + off, chunk, &wrote, nullptr) || wrote == 0) {
            if (++s->write_failures >= kMaxWriteFailures) {
                close_log_file(s);   // 磁盘满 / 权限变更：放弃文件输出，不再重试
                s->file_ok.store(false, std::memory_order_relaxed);
            }
            return;
        }
        s->write_failures = 0;
        off += wrote;
        s->written += wrote;
    }
}

void flush_batch(State* s, std::vector<std::string>& batch) noexcept {
    if (s->file == INVALID_HANDLE_VALUE) {
        batch.clear();
        return;
    }
    for (const std::string& line : batch) {
        rotate_if_needed(s);
        write_raw(s, line.data(), line.size());
        if (s->file == INVALID_HANDLE_VALUE) break;
    }
    if (s->file != INVALID_HANDLE_VALUE)
        ::FlushFileBuffers(s->file);   // 每批冲洗一次：崩溃前最后几行必须落地
    batch.clear();
}

void writer_loop(std::stop_token st, State* s) noexcept {
    for (;;) {
        std::vector<std::string> batch;
        {
            std::unique_lock lock(s->mtx);
            s->cv.wait(lock, st, [s] { return !s->q.empty(); });
            if (s->q.empty() && st.stop_requested()) return;
            while (!s->q.empty()) {
                batch.push_back(std::move(s->q.front()));
                s->q.pop_front();
            }
        }
        flush_batch(s, batch);   // 不持锁碰磁盘
    }
}

std::string to_utf8(const std::wstring& w) noexcept {
    if (w.empty()) return {};
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out;
    try {
        out.resize(static_cast<std::size_t>(need));
    } catch (...) {
        return {};
    }
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                          out.data(), need, nullptr, nullptr);
    return out;
}

bool needs_quotes(std::string_view v) noexcept {
    if (v.empty()) return true;
    for (const char c : v)
        if (c == ' ' || c == '"' || c == '\t' || c == '\n' || c == '\r') return true;
    return false;
}

std::string kv_impl(const char* name, std::string_view value_utf8) {
    std::string out;
    out.reserve(value_utf8.size() + 16);
    out.append(name ? name : "?");
    out.push_back('=');
    if (needs_quotes(value_utf8)) {
        out.push_back('"');
        for (const char c : value_utf8) {
            if (c == '"') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('"');
    } else {
        out.append(value_utf8);
    }
    return out;
}

}  // namespace

void init(const std::wstring& exe_dir) noexcept {
    try {
        if (slot() != nullptr) return;   // 已初始化
        std::unique_ptr<State> s(new (std::nothrow) State());
        if (!s) return;
        s->min_level.store(default_level(), std::memory_order_relaxed);
        s->dir = exe_dir;
        if (!s->dir.empty() && (s->dir.back() == L'\\' || s->dir.back() == L'/'))
            s->dir.pop_back();
        s->dir += L"\\logs";
        ::CreateDirectoryW(s->dir.c_str(), nullptr);

        State* raw = s.release();
        slot() = raw;   // 先发布状态：即便后面打开文件失败，write() 也有安全的落点

        if (open_log_file(raw)) {
            raw->file_ok.store(true, std::memory_order_relaxed);
            raw->writer = std::jthread([raw](std::stop_token st) { writer_loop(st, raw); });
            write(Level::Info, "log",
                  std::string("log session start ") + kv("file", raw->path));
        } else {
            close_log_file(raw);
        }
    } catch (...) {
        // 初始化失败不致命：write() 退化为调试器输出。
    }
}

void shutdown() noexcept {
    try {
        State* s = state();
        if (!s) return;
        if (s->writer.joinable()) {
            s->writer.request_stop();
            s->cv.notify_all();
            s->writer.join();   // writer_loop 退出前会冲洗剩余队列
        }
        std::lock_guard lock(s->mtx);
        close_log_file(s);
        s->file_ok.store(false, std::memory_order_relaxed);
    } catch (...) {
    }
}

bool enabled(Level lv) noexcept {
    State* s = state();
    const Level min = s ? s->min_level.load(std::memory_order_relaxed) : default_level();
    return static_cast<int>(lv) >= static_cast<int>(min);
}

void set_min_level(Level lv) noexcept {
    if (State* s = state()) s->min_level.store(lv, std::memory_order_relaxed);
}

Level min_level() noexcept {
    State* s = state();
    return s ? s->min_level.load(std::memory_order_relaxed) : default_level();
}

void write(Level lv, const char* module, std::string_view msg) noexcept {
    try {
        State* s = state();
        const char* mod = module ? module : "?";
        if (s == nullptr) {
            output_debugger(mod, msg);
            return;
        }
        if (static_cast<int>(lv) < static_cast<int>(s->min_level.load(std::memory_order_relaxed)))
            return;
        if (!s->file_ok.load(std::memory_order_relaxed)) {
            output_debugger(mod, msg);
            return;
        }
        std::string line = format_line(lv, mod, msg);
        if (line.empty()) return;
        {
            std::lock_guard lock(s->mtx);
            while (s->q.size() >= kMaxQueue) {
                s->q.pop_front();
                s->dropped.fetch_add(1, std::memory_order_relaxed);
            }
            s->q.push_back(std::move(line));
        }
        s->cv.notify_one();
    } catch (...) {
    }
}

// ---- 字段拼接 ----

std::string kv(const char* name, std::string_view value) { return kv_impl(name, value); }
std::string kv(const char* name, const std::string& value) { return kv_impl(name, value); }
std::string kv(const char* name, const char* value) { return kv_impl(name, value ? value : ""); }
std::string kv(const char* name, const std::wstring& value) { return kv_impl(name, to_utf8(value)); }

std::string kv(const char* name, long long value) { return kv_impl(name, std::to_string(value)); }
std::string kv(const char* name, unsigned long long value) { return kv_impl(name, std::to_string(value)); }
std::string kv(const char* name, int value) { return kv_impl(name, std::to_string(value)); }
std::string kv(const char* name, unsigned int value) { return kv_impl(name, std::to_string(value)); }

std::string kv(const char* name, double value) {
    char buf[40] = {};
    std::snprintf(buf, sizeof buf, "%.4g", value);
    return kv_impl(name, buf);
}

std::wstring log_path() noexcept {
    State* s = state();
    return s ? s->path : std::wstring{};
}

std::wstring log_dir() noexcept {
    State* s = state();
    return s ? s->dir : std::wstring{};
}

unsigned long long dropped_count() noexcept {
    State* s = state();
    return s ? s->dropped.load(std::memory_order_relaxed) : 0;
}

}  // namespace lr::log
