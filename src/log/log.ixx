// log.ixx — Lilith Reader 轻量日志模块
//
// 定位：**只用于定位问题**。记录"发生了什么事、发生在哪个模块、带上哪些可检索字段"，
//       不承担崩溃转储职责（那一层见 src/app/crash.cpp）。
//
// 为什么自实现而不用 spdlog（ADR-077）：
//   · 体积与依赖：spdlog 即使只编核心、内嵌 fmt，Release 静态链接下也是数百 KB 量级，
//     而本项目真正需要的只有"等级 + 时间 + 线程 + 模块 + 一行文本"这一种能力；
//     单 exe、静态链接、少依赖是本项目的既定纪律（迁移计划 §0.2）。
//   · 崩溃路径不能用它：进程崩溃时堆与运行库都可能已损坏，普通日志模块不可信，
//     因此崩溃转储用 Win32 原生 API 单独实现，与日志彻底解耦。
//   · UI 线程零阻塞：本模块写入只入队，落盘在后台线程，天然满足"UI 线程零阻塞"。
//
// 纪律（调用方请一并遵守）：
//   1. 本模块任何导出函数**不抛异常**（全部 noexcept，内部兜底）；未调用 init() 时
//      write() 安全退化（Debug 走 OutputDebugString，Release 丢弃），故测试与任何
//      未初始化的宿主都不会因日志而崩。
//   2. 调用点只允许放**低频事件**：打开/关闭文档、认证、单页渲染失败、重试、线程异常、
//      设备错误、状态落盘失败、退出。**不得**放进每帧 / 每页 / 每像素 / 每个 tile 的热路径。
//   3. 日志内容**不写用户隐私正文**：只写路径、页码、错误码、尺寸这类定位所需字段。
//
// 跨进程语义（多实例并存是允许的，本应用没有单实例闸）：
//   · 文件以 FILE_APPEND_DATA + 共享读写打开，NTFS 上单次 WriteFile 的追加按字节落到
//     文件尾，行不会被撕开；每行头部带 pid，交错的行可以归属到具体实例。
//   · **轮转**是竞争点：改名要求所有句柄以 FILE_SHARE_DELETE 打开，另一实例没有，改名会
//     失败。处置是退避重试（见 log.cpp），代价是该会话内日志可能暂时超过单文件上限。

module;

#include <cstdint>
#include <string>
#include <string_view>

export module lilithreader.log;

export namespace lr::log {

enum class Level : std::int32_t {
    Debug = 0,
    Info  = 1,
    Warn  = 2,
    Error = 3,
};

// 初始化：在 exe_dir 下建立 logs\ 目录并打开日志文件，启动后台写入线程。
// 可重复调用（第二次无操作）；任何失败都静默退化，不影响主流程。
// 只应由 main.cpp 的入口在**创建窗口之前**调用一次。
void init(const std::wstring& exe_dir) noexcept;

// 关闭：冲洗队列、停止后台线程、关闭文件。进程退出前调用；可重复调用。
void shutdown() noexcept;

// 当前是否记录该等级（用于调用方在拼串前短路，避免无谓分配）。
[[nodiscard]] bool enabled(Level lv) noexcept;

// 运行期调级（默认 Info）。线程安全。
void set_min_level(Level lv) noexcept;
[[nodiscard]] Level min_level() noexcept;

// 写一条日志。线程安全、非阻塞（仅入队）。未初始化时安全退化。
void write(Level lv, const char* module, std::string_view msg) noexcept;

// 便捷入口。module 用固定短名：app / render / document / platform / ui / state / log。
inline void debug(const char* module, std::string_view msg) noexcept {
    if (enabled(Level::Debug)) write(Level::Debug, module, msg);
}
inline void info(const char* module, std::string_view msg) noexcept {
    if (enabled(Level::Info)) write(Level::Info, module, msg);
}
inline void warn(const char* module, std::string_view msg) noexcept {
    if (enabled(Level::Warn)) write(Level::Warn, module, msg);
}
inline void error(const char* module, std::string_view msg) noexcept {
    if (enabled(Level::Error)) write(Level::Error, module, msg);
}

// ---- 结构化字段拼接 ----
//
// 统一产出 `name=value` 片段，调用方用空格串起来即可：
//   log::error("render", "render failed " + kv("page", 3) + " " + kv("err", "Corrupt"));
// 字符串值若含空格会被引号包起来，保证一行日志仍可被按空格切分解析。
[[nodiscard]] std::string kv(const char* name, std::string_view value);
[[nodiscard]] std::string kv(const char* name, const std::string& value);
[[nodiscard]] std::string kv(const char* name, const char* value);
[[nodiscard]] std::string kv(const char* name, long long value);
[[nodiscard]] std::string kv(const char* name, unsigned long long value);
[[nodiscard]] std::string kv(const char* name, int value);
[[nodiscard]] std::string kv(const char* name, unsigned int value);
[[nodiscard]] std::string kv(const char* name, double value);
// 宽字符串转 UTF-8 后再拼（路径用它；Windows 路径是 UTF-16）。
[[nodiscard]] std::string kv(const char* name, const std::wstring& value);

// 当前日志文件完整路径（供"打开日志所在目录"用）；未初始化时为空。
[[nodiscard]] std::wstring log_path() noexcept;
// 当前日志目录（未初始化时为空）。日志写入失败时为空。
[[nodiscard]] std::wstring log_dir() noexcept;

// 已丢弃的日志条数（后台队列溢出时丢弃最旧的一条并计数）。仅用于自诊断。
[[nodiscard]] unsigned long long dropped_count() noexcept;

}  // namespace lr::log
