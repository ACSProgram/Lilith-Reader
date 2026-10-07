// reader_state.ixx — Lilith Reader 阅读状态持久化（身份分层见 ADR-062）
//
// 职责：把"每本书读到哪、有哪些书签、用什么视图参数"存到 exe 同目录的
//       reader_state.bin。**纯序列化部分与磁盘无关**，可脱离 Win32 单测
//       （tests/reader_state_test.cpp）——与 canvas 布局纯函数化、page_cache 纯策略同一思路。
//
// 设计要点：
//   · **主键 = 内容指纹**（ADR-062）：打开时算一份"稀疏采样指纹"（lr::file_fingerprint，
//     只读头/中/尾 ≤320KB，与文件大小无关），同一份文件被复制/移动/重命名后指纹不变，
//     阅读位置与书签随之沿用 —— 旧的"路径+大小+修改时间"键降级为**兜底与迁移**用的
//     `path_key`（路径没变时仍按它命中，命中后把记录改挂到内容指纹下）。
//   · **分层命中**（`locate`）：内容指纹 → 路径键；命中即返回下标，并标出
//     `relocated`（记录里记的路径与当前路径不同 = 文件被移动过，交给 UI 决定是否询问）。
//   · **二进制格式带 magic + 版本号**：解析失败一律**安全拒绝**（返回空状态），
//     绝不因损坏的状态文件而崩溃或丢文档——宁可丢掉阅读位置，也不能影响打开文档。
//     旧版（`LRS1` / `LRS2`）都能解码并自动迁移：v2 的单条路径填进 locations[0]，
//     v1 连路径都没存过（键里只有哈希）→ locations 为空 = "位置未知"。
//   · **原子写入**：先写临时文件再 MoveFileExW 替换，杜绝写坏。
//
// 本模块**零项目内依赖**（只依赖标准库与 Win32），不 import document/canvas/render；
// 内容指纹本身放在 utils（render 的工作线程要用它，utils 谁都能 import）。

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
    int   scheme = 0;     // 纸张方案：0 原色 / 1 深色纸张 / 2 暖色（lr::PageScheme）
    std::vector<Bookmark> bookmarks;

    // ---- 身份（ADR-062 / ADR-065）----
    std::uint64_t path_key = 0;   // 最近位置的"路径+大小+修改时间"键（v1 迁移 / 兜底命中用）
    int           page_count = 0; // 记录时刻的页数（0 = 未知，不参与继承校验）
    // 已知位置（UTF-8），**最近打开的在前**。同一份内容在多个地方各有一份时，这里会有多条
    // ——这正是"一份阅读数据、若干路径"的表达（管理窗口按它画路径树）。
    // 空 = 位置未知（升级前的 v1 记录没存过路径，无从还原）。
    std::vector<std::string> locations;
};

// 每份文档最多记多少个已知位置：超过就丢最旧的（防止"来回复制"把文件撑大）。
inline constexpr std::size_t kMaxLocations = 8;

// 记住一个位置：已记过的挪到最前，新的插到最前，超过 kMaxLocations 丢最旧的。
// 空串忽略（位置未知时不往里塞空条目）。
void remember_location(DocRecord& r, const std::string& path_u8);

// 最近一次打开的位置；空串 = 位置未知。调用方拿到的引用在 r.locations 变动前有效。
[[nodiscard]] const std::string& last_location(const DocRecord& r) noexcept;

// 该位置是否已记过（判"换没换地方"用）。
[[nodiscard]] bool has_location(const DocRecord& r, const std::string& path_u8) noexcept;

// ---- 文档身份：打开时算一次，用来在库里定位记录 ----
struct DocIdentity {
    std::uint64_t content = 0;    // 稀疏采样内容指纹（跨路径稳定）；0 = 取不到
    std::uint64_t path = 0;       // 路径 + 大小 + 修改时间键；0 = 取不到
    int           page_count = 0; // 当前页数（继承校验用）
    std::string   path_u8;        // 当前路径（UTF-8）：判"是否换过位置"、刷新 last_path 用
};

// ---- 命中结果 ----
struct DocMatch {
    int  index = -1;         // 命中记录在 ReaderState::docs 里的下标；-1 = 未命中
    bool relocated = false;  // 命中，但记录里记的路径 ≠ 当前路径（文件被移动/复制过）
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

// 把第 idx 条记录改挂到 new_key（键本身变了，记录内容不动）。idx 越界、new_key 为 0、
// 或 new_key 已被**别的**记录占用 → 返回 false 什么都不做。
// 用途：把一份共享的阅读数据拆成两条各自独立的记录（ADR-065「另起一份」）。
[[nodiscard]] bool rekey(ReaderState& s, std::size_t idx, std::uint64_t new_key) noexcept;

// 文档键：路径（UTF-16 码元）+ 文件大小 + 修改时间 的 FNV-1a 64 位哈希（ADR-034 的旧键）。
// 路径不存在/取不到属性时返回 0（视为无效键，不参与存取）。
[[nodiscard]] std::uint64_t document_key(const std::wstring& path) noexcept;

// 主键：优先内容指纹（跨路径稳定），取不到才退回路径键。两者都取不到返回 0。
[[nodiscard]] std::uint64_t primary_key(const DocIdentity& id) noexcept;

// 分层定位记录：smart=true 时先按内容指纹找（文件被移动/复制/重新解压后仍能命中），
// 再按路径键找（同一路径直接沿用，含 v1 记录迁移）。页数已知且与记录不符的跳过
// —— 页数都变了就不是同一版，书签会错位。
// `relocated` = 记录里**没有**当前这个位置（确实是换了个地方打开，值得问一句）；
// 路径早就记过（比如同一份书在两处各存一份）就不算。
[[nodiscard]] DocMatch locate(const ReaderState& s, const DocIdentity& id, bool smart) noexcept;

// 让 idx 处的记录归属 id：主键改挂到 content（可用时），刷新 path_key / page_count，
// 并把当前路径记进 locations。返回落定后的主键（供调用方存为"当前文档键"）。
[[nodiscard]] std::uint64_t adopt(ReaderState& s, std::size_t idx,
                                  const DocIdentity& id) noexcept;

// ---- 纯序列化（不碰磁盘，可单测） ----
//
// 格式（全部小端）：
//   v3：magic 'L''R''S''3' | u32 version=3 | u32 doc_count
//       每条记录： u64 key | u64 path_key | i32 page_count
//                  | u32 loc_count | loc_count × (u32 len | bytes)
//                  | i32 page | u32 zoom(bits) | i32 columns | i32 rotation
//                  | u32 flags(bit0 fit_width, bit1 spread) | i32 scheme
//                  | u32 bookmark_count | 每个书签: i32 page | u32 label_len | label bytes
//   v2：与 v3 同构，但"位置"只有一个（u32 len | bytes）→ 非空时填进 locations[0]。
//   v1：magic 'L''R''S''1' | u32 version=1 | u32 doc_count
//       每条记录： u64 key | i32 page | u32 zoom | i32 columns | i32 rotation
//                  | u32 flags | i32 scheme | u32 bookmark_count | 书签…
//       解码时把 key 当作 path_key 填入（v1 的键就是路径键），page_count=0、locations 为空。
[[nodiscard]] std::vector<std::uint8_t> encode_state(const ReaderState& s);

// 解析。magic/版本不符、数据截断、超限一律返回 false 且**不改动 out**。
[[nodiscard]] bool decode_state(const std::uint8_t* data, std::size_t size,
                                ReaderState& out) noexcept;

// ---- 文件 I/O（原子替换） ----
// 读失败/文件不存在/内容损坏 → 返回空状态（不抛、不崩）。
[[nodiscard]] ReaderState load_state(const std::wstring& path) noexcept;
// 写临时文件 + MoveFileExW 替换。返回是否成功。
[[nodiscard]] bool save_state(const std::wstring& path, const ReaderState& s) noexcept;
// 与 save_state 同一套原子写入，但入参是**已编码**的字节：供异步持久化服务在
// 工作线程直接落盘，UI 线程只负责 encode_state 产快照（ADR-082）。
[[nodiscard]] bool write_state_bytes(const std::wstring& path,
                                     const std::vector<std::uint8_t>& bytes) noexcept;

// 解析上限（防御损坏/恶意文件把内存撑爆）
inline constexpr std::uint32_t kMaxDocs = 4096;
inline constexpr std::uint32_t kMaxBookmarksPerDoc = 1024;
inline constexpr std::uint32_t kMaxLabelBytes = 512;
inline constexpr std::uint32_t kMaxPathBytes = 1024;   // 单个位置的上限（超长路径会被截断）
inline constexpr std::uint32_t kStateVersion = 3;
inline constexpr std::uint32_t kStateVersionV2 = 2;
inline constexpr std::uint32_t kStateVersionV1 = 1;

}  // namespace lr
