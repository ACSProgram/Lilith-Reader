// reader_state.cpp — lilithreader.reader_state 的实现单元（Phase 5）
//
// 两部分：
//   1. 纯序列化（encode_state / decode_state）——不碰磁盘，可单测；
//   2. 文件 I/O（load_state / save_state / document_key）——Win32，原子写入。
//
// 纪律：本文件所有对外函数都不抛异常；解析任何损坏输入都返回失败而不是崩溃。
//       阅读状态是"锦上添花"，绝不能因为它写坏/读坏而影响打开文档这条主路径。

module;

#define NOMINMAX
#include <windows.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

module lilithreader.reader_state;

namespace lr {
namespace {

// ---- FNV-1a 64 ----
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t fnv1a(std::uint64_t h, const void* data, std::size_t n) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= static_cast<std::uint64_t>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

// ---- 小端读写 ----
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}
void put_u64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

// 带边界检查的读取器：任何越界即置 bad，后续读取全部返回 0。
struct Reader {
    const std::uint8_t* p = nullptr;
    std::size_t         n = 0;
    std::size_t         off = 0;
    bool                bad = false;

    bool need(std::size_t k) {
        if (bad || off + k > n) { bad = true; return false; }
        return true;
    }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        const std::uint32_t v = static_cast<std::uint32_t>(p[off]) |
                                (static_cast<std::uint32_t>(p[off + 1]) << 8) |
                                (static_cast<std::uint32_t>(p[off + 2]) << 16) |
                                (static_cast<std::uint32_t>(p[off + 3]) << 24);
        off += 4;
        return v;
    }
    std::uint64_t u64() {
        if (!need(8)) return 0;
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[off + i]) << (8 * i);
        off += 8;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() { return std::bit_cast<float>(u32()); }
    // 拷贝 n 字节到 string（已按上限钳制）
    std::string str(std::uint32_t len) {
        if (!need(len)) return {};
        std::string s(reinterpret_cast<const char*>(p + off), len);
        off += len;
        return s;
    }
};

// 读取整个文件（上限 64MB 防御）。失败返回空 vector。
std::vector<std::uint8_t> read_file(const std::wstring& path) noexcept {
    std::vector<std::uint8_t> out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (64ll << 20)) {
        CloseHandle(h);
        return out;
    }
    out.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok || read != out.size()) { out.clear(); return out; }
    return out;
}

}  // namespace

// ---- ReaderState ----

const DocRecord* ReaderState::find(std::uint64_t key) const noexcept {
    if (key == 0) return nullptr;
    for (const auto& kv : docs)
        if (kv.first == key) return &kv.second;
    return nullptr;
}

DocRecord& ReaderState::upsert(std::uint64_t key) {
    for (auto& kv : docs)
        if (kv.first == key) return kv.second;
    docs.emplace_back(key, DocRecord{});
    return docs.back().second;
}

bool ReaderState::erase(std::uint64_t key) noexcept {
    for (auto it = docs.begin(); it != docs.end(); ++it) {
        if (it->first == key) { docs.erase(it); return true; }
    }
    return false;
}

// ---- 文档键 ----

std::uint64_t document_key(const std::wstring& path) noexcept {
    if (path.empty()) return 0;

    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return 0;

    // 路径按 UTF-16 原始码元参与哈希：对同一路径稳定，且无需转 UTF-8。
    std::uint64_t h = kFnvOffset;
    h = fnv1a(h, path.data(), path.size() * sizeof(wchar_t));

    // 文件大小（高 32 位 + 低 32 位）
    const std::uint64_t size =
        (static_cast<std::uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    h = fnv1a(h, &size, sizeof size);

    // 修改时间（FILETIME 的 64 位）
    const std::uint64_t mtime =
        (static_cast<std::uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
        fad.ftLastWriteTime.dwLowDateTime;
    h = fnv1a(h, &mtime, sizeof mtime);

    // 0 保留为"无效键"
    return h == 0 ? 1 : h;
}

// ---- 序列化 ----

std::vector<std::uint8_t> encode_state(const ReaderState& s) {
    std::vector<std::uint8_t> b;
    b.reserve(64 + s.docs.size() * 48);
    b.push_back('L'); b.push_back('R'); b.push_back('S'); b.push_back('1');
    put_u32(b, kStateVersion);
    put_u32(b, static_cast<std::uint32_t>(s.docs.size()));

    for (const auto& kv : s.docs) {
        const DocRecord& r = kv.second;
        put_u64(b, kv.first);
        put_u32(b, static_cast<std::uint32_t>(r.page));
        put_u32(b, std::bit_cast<std::uint32_t>(r.zoom));
        put_u32(b, static_cast<std::uint32_t>(r.columns));
        put_u32(b, static_cast<std::uint32_t>(r.rotation));
        std::uint32_t flags = 0;
        if (r.fit_width) flags |= 1u;
        if (r.spread)    flags |= 2u;
        put_u32(b, flags);
        put_u32(b, static_cast<std::uint32_t>(r.color_mode));

        const std::uint32_t bm =
            static_cast<std::uint32_t>(r.bookmarks.size() > kMaxBookmarksPerDoc
                                           ? kMaxBookmarksPerDoc
                                           : r.bookmarks.size());
        put_u32(b, bm);
        for (std::uint32_t i = 0; i < bm; ++i) {
            const Bookmark& k = r.bookmarks[i];
            put_u32(b, static_cast<std::uint32_t>(k.page));
            std::uint32_t len = static_cast<std::uint32_t>(k.label.size());
            if (len > kMaxLabelBytes) len = kMaxLabelBytes;
            put_u32(b, len);
            b.insert(b.end(), k.label.begin(), k.label.begin() + len);
        }
    }
    return b;
}

bool decode_state(const std::uint8_t* data, std::size_t size, ReaderState& out) noexcept {
    if (data == nullptr || size < 12) return false;
    Reader rd{ data, size, 0, false };

    // magic
    if (rd.p[0] != 'L' || rd.p[1] != 'R' || rd.p[2] != 'S' || rd.p[3] != '1') return false;
    rd.off = 4;

    const std::uint32_t version = rd.u32();
    if (rd.bad || version != kStateVersion) return false;

    const std::uint32_t count = rd.u32();
    if (rd.bad || count > kMaxDocs) return false;

    ReaderState tmp;
    tmp.docs.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t key = rd.u64();
        DocRecord r;
        r.page = rd.i32();
        r.zoom = rd.f32();
        r.columns = rd.i32();
        r.rotation = rd.i32();
        const std::uint32_t flags = rd.u32();
        r.color_mode = rd.i32();
        r.fit_width = (flags & 1u) != 0;
        r.spread = (flags & 2u) != 0;

        const std::uint32_t bm = rd.u32();
        if (rd.bad || bm > kMaxBookmarksPerDoc) return false;
        r.bookmarks.reserve(bm);
        for (std::uint32_t j = 0; j < bm; ++j) {
            Bookmark k;
            k.page = rd.i32();
            const std::uint32_t len = rd.u32();
            if (rd.bad || len > kMaxLabelBytes) return false;
            k.label = rd.str(len);
            if (rd.bad) return false;
            r.bookmarks.push_back(std::move(k));
        }
        if (rd.bad) return false;

        // 字段钳制：损坏/越界值不让它进入内存（rotation 归一到 0/90/180/270）
        r.columns = r.columns < 1 ? 1 : (r.columns > 4 ? 4 : r.columns);
        r.rotation %= 360;
        if (r.rotation < 0) r.rotation += 360;
        r.rotation = (r.rotation / 90) * 90;
        r.color_mode = r.color_mode < 0 ? 0 : (r.color_mode > 2 ? 2 : r.color_mode);
        if (!(r.zoom > 0.0f) || r.zoom > 100.0f) r.zoom = 1.0f;

        if (key != 0) tmp.docs.emplace_back(key, std::move(r));
    }
    if (rd.bad) return false;

    out = std::move(tmp);
    return true;
}

// ---- 文件 I/O ----

ReaderState load_state(const std::wstring& path) noexcept {
    ReaderState st;
    const std::vector<std::uint8_t> bytes = read_file(path);
    if (bytes.empty()) return st;  // 不存在/读失败：空状态
    ReaderState parsed;
    if (!decode_state(bytes.data(), bytes.size(), parsed)) return st;  // 损坏：安全拒绝
    return parsed;
}

bool save_state(const std::wstring& path, const ReaderState& s) noexcept {
    const std::vector<std::uint8_t> bytes = encode_state(s);

    // 先写临时文件（同目录，保证 MoveFileEx 是同一卷上的原子重命名）
    const std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr);
    FlushFileBuffers(h);
    CloseHandle(h);
    if (!ok || written != bytes.size()) {
        DeleteFileW(tmp.c_str());
        return false;
    }

    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace lr
