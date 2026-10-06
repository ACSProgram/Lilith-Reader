// persist.cpp — lilithreader.persist 的实现单元（ADR-082）
//
// 一条工作线程 + 防抖 + 退出 flush。线程循环只有一个共享状态块（Impl 的 mtx 保护的字段），
// 落盘经 reader_state 的原子写入（write_state_bytes），本文件不直接碰 Win32。
//
// 纪律：本文件对外函数都不抛异常；OOM 等异常在 request_save 里就地转为 failed 标记，
//       绝不逃出 noexcept。

module;

#include <chrono>
#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

module lilithreader.persist;

import lilithreader.reader_state;
import lilithreader.log;

namespace lr {

struct PersistService::Impl {
    std::wstring path;
    int          debounce_ms = 1000;
    bool         enabled = false;     // 路径有效且线程已起

    std::mutex              mtx;
    std::condition_variable cv;
    std::vector<std::uint8_t> buf;    // 最新快照
    std::uint64_t           version = 0;   // buf 的版本（每次 request_save +1）
    std::uint64_t           written = 0;   // 已落盘的版本
    bool                    flush_requested = false;
    bool                    stop = false;
    bool                    failed = false;

    std::thread worker;

    // 工作线程主循环：无待写则睡；有待写则等一个"安静窗口"（期间有新快照即重新计时）；
    // flush_requested 时立即落盘。落盘在锁外进行，避免长时间持锁。
    void run() noexcept {
        std::unique_lock lk(mtx);
        for (;;) {
            cv.wait(lk, [this] { return stop || flush_requested || version != written; });
            if (stop) return;

            if (!flush_requested) {
                std::uint64_t last = version;
                for (;;) {
                    cv.wait_for(lk, std::chrono::milliseconds(debounce_ms));
                    if (stop) return;
                    if (flush_requested) break;   // flush：立即落盘
                    if (version == last) break;   // 安静窗口已满
                    last = version;               // 仍有改动：重新计时
                }
            }

            const std::uint64_t v = version;
            flush_requested = false;
            std::vector<std::uint8_t> bytes = buf;   // 复制快照（解锁后 buf 可能被覆盖）
            lk.unlock();

            const bool ok = write_state_bytes(path, bytes);
            if (!ok)
                lr::log::error("state", "persist write failed " + lr::log::kv("path", path));

            lk.lock();
            written = v;
            failed = !ok;
            cv.notify_all();
        }
    }
};

PersistService::PersistService(std::wstring path, int debounce_ms) noexcept
    : impl_(new (std::nothrow) Impl{}) {
    if (!impl_) return;
    impl_->path = std::move(path);
    impl_->debounce_ms = debounce_ms > 0 ? debounce_ms : 1;
    if (impl_->path.empty()) return;   // 无路径：安全退化（request_save/flush 均无操作）
    impl_->enabled = true;
    impl_->worker = std::thread([p = impl_.get()] { p->run(); });
}

PersistService::~PersistService() {
    if (!impl_) return;
    flush();   // 退出前把在途快照写完（ADR-082）
    {
        std::lock_guard lk(impl_->mtx);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->worker.joinable()) impl_->worker.join();
}

void PersistService::request_save(const ReaderState& s) noexcept {
    if (!impl_ || !impl_->enabled) return;
    // 纯序列化在**调用线程**完成（UI 线程只产快照，不碰磁盘）。
    std::vector<std::uint8_t> bytes;
    try {
        bytes = encode_state(s);
    } catch (...) {
        std::lock_guard lk(impl_->mtx);
        impl_->failed = true;
        return;
    }
    {
        std::lock_guard lk(impl_->mtx);
        impl_->buf = std::move(bytes);
        ++impl_->version;
    }
    impl_->cv.notify_all();
}

void PersistService::flush() noexcept {
    if (!impl_ || !impl_->enabled) return;
    std::unique_lock lk(impl_->mtx);
    const std::uint64_t target = impl_->version;
    if (target == 0 || impl_->written >= target) return;   // 无待写
    impl_->flush_requested = true;
    impl_->cv.notify_all();
    impl_->cv.wait(lk, [this, target] {
        return impl_->written >= target || impl_->stop;
    });
}

bool PersistService::last_save_failed() const noexcept {
    if (!impl_) return false;
    std::lock_guard lk(impl_->mtx);
    return impl_->failed;
}

bool PersistService::dirty() const noexcept {
    if (!impl_) return false;
    std::lock_guard lk(impl_->mtx);
    return impl_->version != impl_->written;
}

}  // namespace lr