// reader_state.ixx — Lilith Reader 阅读状态持久化（Phase 5）
//
// 职责：把"每本书读到哪、有哪些书签、用什么视图参数"存到 exe 同目录的
//       reader_state.bin。**纯序列化部分与磁盘无关**，可脱离 Win32 单测
//       （tests/reader_state_test.cpp）——与 canvas 布局纯函数化、page_cache 纯策略同一思路。
//
// 设计要点：
//   · **键 = 路径 + 大小 + 修改时间 的哈希**（见 document_key）。这样同一文件被替换/改名后
//     不会串到别人的位置上；哈希冲突只可能让"两个不同文档共享一条记录"，代价可接受
//     （不会误删用户文件，只是阅读位置可能错位）。
//   · **二进制格式带 magic + 版本号**：解析失败一律**安全拒绝**（返回空状态），
//     绝不因损坏的状态文件而崩溃或丢文档——宁可丢掉阅读位置，也不能影响打开文档。
//   · **原子写入**：先写临时文件再 MoveFileExW 替换，杜绝写坏。
//
// 本模块**零项目内依赖**（只依赖标准库与 Win32），不 import document/canvas/render。

module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

export module lilithreader.reader_state;

export namespace lr {

// ---- 书签 ----
struct Bookmark {
    int         page = 0;    // 0 基页号
    std::string label;       // UTF-8，可为空
};

// ---- 单文档的阅读状态 ----
//
// 未使用 `present` 标记：文档在不在由 ReaderState::find 是否返回 nullptr 表达。
struct DocRecord {
    int   page = 0;           // 阅读位置（0 基页号）
    float zoom = 1.0f;        // 固定缩放倍率（fit_width 时仍保留记忆值，切回固定缩放用）
    int   columns = 1;        // 1~4 列
    int   rotation = 0;       // 0/90/180/270
    bool  fit_width = true;   // 是否 fit-width 模式
    bool  spread = false;     // 双页对开（书籍模式）
    int   color_mode = 0;     // 0 正常 / 1 反色 / 2 护眼
    std::vector<Bookmark> bookmarks;
};

// ---- 全部文档记录 ----
struct ReaderState {
    std::vector<std::pair<std::uint64_t, DocRecord>> docs;

    // 查记录；不存在返回 nullptr。key 为 0（无效键）时恒返回 nullptr。
    [[nodiscard]] const DocRecord* find(std::uint64_t key) const noexcept;
    // 取或新建记录（返回可写引用）。key 为 0 时行为未定义，调用方须先判。
    DocRecord& upsert(std::uint64_t key);
    // 删除记录；返回是否删掉了。
    bool erase(std::uint64_t key) noexcept;
};

// 文档键：路径（UTF-8 字节）+ 文件大小 + 修改时间 的 FNV-1a 64 位哈希。
// 路径不存在/取不到属性时返回 0（视为无效键，不参与存取）。
[[nodiscard]] std::uint64_t document_key(const std::wstring& path) noexcept;

// ---- 纯序列化（不碰磁盘，可单测） ----
//
// 格式（全部小端）：
//   magic  'L''R''S''1'   | u32 version | u32 doc_count
//   每条记录： u64 key | i32 page | u32 zoom(bits) | i32 columns | i32 rotation
//              | u32 flags(bit0 fit_width, bit1 spread) | i32 color_mode
//              | u32 bookmark_count | 每个书签: i32 page | u32 label_len | label bytes
[[nodiscard]] std::vector<std::uint8_t> encode_state(const ReaderState& s);

// 解析。magic/版本不符、数据截断、超限一律返回 false 且**不改动 out**。
[[nodiscard]] bool decode_state(const std::uint8_t* data, std::size_t size,
                                ReaderState& out) noexcept;

// ---- 文件 I/O（原子替换） ----
// 读失败/文件不存在/内容损坏 → 返回空状态（不抛、不崩）。
[[nodiscard]] ReaderState load_state(const std::wstring& path) noexcept;
// 写临时文件 + MoveFileExW 替换。返回是否成功。
[[nodiscard]] bool save_state(const std::wstring& path, const ReaderState& s) noexcept;

// 解析上限（防御损坏/恶意文件把内存撑爆）
inline constexpr std::uint32_t kMaxDocs = 4096;
inline constexpr std::uint32_t kMaxBookmarksPerDoc = 1024;
inline constexpr std::uint32_t kMaxLabelBytes = 512;
inline constexpr std::uint32_t kStateVersion = 1;

}  // namespace lr
