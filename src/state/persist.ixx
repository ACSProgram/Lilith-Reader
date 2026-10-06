// persist.ixx — 阅读状态异步持久化服务（ADR-082）
//
// 定位：把 reader_state.bin 的**写盘**从 UI 线程移出。UI 线程只产快照
//       （encode_state，纯序列化、不碰磁盘），服务在一条独立工作线程上按**防抖窗口**
//       落盘；退出路径 flush() 阻塞至最近一次快照写完。
//
// 纪律：
//   · 服务**不接触** ReaderState —— 只接受已编码字节，故与 UI 线程无共享可变状态、
//     无数据竞争（ReaderState 仍由 UI 线程独占）。
//   · 接口不抛异常（全部 noexcept，内部兜底）；路径为空或线程未起时安全退化。
//   · 只此一条线程，与 render 工作线程并列、互不交互（ADR-082）。

module;

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

export module lilithreader.persist;

export import lilithreader.reader_state;   // request_save 的入参是 ReaderState

export namespace lr {

class PersistService {
public:
    // path：reader_state.bin 的完整路径。debounce_ms：防抖窗口（默认 1000ms；测试可注入更小值）。
    explicit PersistService(std::wstring path, int debounce_ms = 1000) noexcept;
    ~PersistService();
    PersistService(const PersistService&) = delete;
    PersistService& operator=(const PersistService&) = delete;

    // 编码当前状态并交给工作线程（防抖后落盘）。可在任意线程调用。
    void request_save(const ReaderState& s) noexcept;
    // 阻塞至最近一次快照已写完（退出路径调用）。可在任意线程调用。
    void flush() noexcept;

    [[nodiscard]] bool last_save_failed() const noexcept;  // 最近一次落盘是否失败
    [[nodiscard]] bool dirty() const noexcept;             // 是否有尚未落盘的快照

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace lr