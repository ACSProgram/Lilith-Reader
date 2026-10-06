// reader_state.cpp — lilithreader.reader_state 的实现单元（Phase 5，ADR-062 增身份分层）
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
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
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
    // 拷贝 n 字节到 string；n 超过 max 视为损坏（返回空并置 bad）
    std::string str(std::uint32_t len, std::uint32_t max) {
        if (len > max || !need(len)) { bad = true; return {}; }
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

// 字段钳制：损坏/越界值不让它进入内存（rotation 归一到 0/90/180/270）
void clamp_record(DocRecord& r) noexcept {
    r.columns = r.columns < 1 ? 1 : (r.columns > 4 ? 4 : r.columns);
    r.rotation %= 360;
    if (r.rotation < 0) r.rotation += 360;
    r.rotation = (r.rotation / 90) * 90;
    r.color_mode = r.color_mode < 0 ? 0 : (r.color_mode > 2 ? 2 : r.color_mode);
    if (!(r.zoom > 0.0f) || r.zoom > 100.0f) r.zoom = 1.0f;
    if (r.page < 0) r.page = 0;
    if (r.page_count < 0) r.page_count = 0;
}

// 书签表（两版共用）
bool read_bookmarks(Reader& rd, DocRecord& r) noexcept {
    const std::uint32_t bm = rd.u32();
    if (rd.bad || bm > kMaxBookmarksPerDoc) return false;
    r.bookmarks.reserve(bm);
    for (std::uint32_t j = 0; j < bm; ++j) {
        Bookmark k;
        k.page = rd.i32();
        const std::uint32_t len = rd.u32();
        k.label = rd.str(len, kMaxLabelBytes);
        if (rd.bad) return false;
        if (k.page < 0) k.page = 0;
        r.bookmarks.push_back(std::move(k));
    }
    return !rd.bad;
}

// 记录体（v3：key 之后是身份 + 位置列表，再是阅读状态）
bool read_record_v3(Reader& rd, std::uint64_t key, ReaderState& tmp) noexcept {
    DocRecord r;
    r.path_key = rd.u64();
    r.page_count = rd.i32();
    const std::uint32_t locs = rd.u32();
    if (rd.bad || locs > kMaxLocations) return false;
    r.locations.reserve(locs);
    for (std::uint32_t j = 0; j < locs; ++j) {
        const std::uint32_t len = rd.u32();
        std::string p = rd.str(len, kMaxPathBytes);
        if (rd.bad) return false;
        if (!p.empty()) r.locations.push_back(std::move(p));
    }

    r.page = rd.i32();
    r.zoom = rd.f32();
    r.columns = rd.i32();
    r.rotation = rd.i32();
    const std::uint32_t flags = rd.u32();
    r.color_mode = rd.i32();
    r.fit_width = (flags & 1u) != 0;
    r.spread = (flags & 2u) != 0;
    if (rd.bad) return false;

    if (!read_bookmarks(rd, r)) return false;
    clamp_record(r);
    if (key != 0) tmp.docs.emplace_back(key, std::move(r));
    return true;
}

// 记录体（v2：只有一条路径 → 迁进 locations[0]）
bool read_record_v2(Reader& rd, std::uint64_t key, ReaderState& tmp) noexcept {
    DocRecord r;
    r.path_key = rd.u64();
    r.page_count = rd.i32();
    const std::uint32_t plen = rd.u32();
    const std::string path = rd.str(plen, kMaxPathBytes);
    if (rd.bad) return false;
    if (!path.empty()) r.locations.push_back(path);

    r.page = rd.i32();
    r.zoom = rd.f32();
    r.columns = rd.i32();
    r.rotation = rd.i32();
    const std::uint32_t flags = rd.u32();
    r.color_mode = rd.i32();
    r.fit_width = (flags & 1u) != 0;
    r.spread = (flags & 2u) != 0;
    if (rd.bad) return false;

    if (!read_bookmarks(rd, r)) return false;
    clamp_record(r);
    if (key != 0) tmp.docs.emplace_back(key, std::move(r));
    return true;
}

// 记录体（v1：只有阅读状态，key 本身即路径键 → 填进 path_key；位置无从还原，留空）
bool read_record_v1(Reader& rd, std::uint64_t key, ReaderState& tmp) noexcept {
    DocRecord r;
    r.page = rd.i32();
    r.zoom = rd.f32();
    r.columns = rd.i32();
    r.rotation = rd.i32();
    const std::uint32_t flags = rd.u32();
    r.color_mode = rd.i32();
    r.fit_width = (flags & 1u) != 0;
    r.spread = (flags & 2u) != 0;
    if (rd.bad) return false;

    if (!read_bookmarks(rd, r)) return false;
    clamp_record(r);
    r.path_key = key;   // v1 的键就是"路径+大小+修改时间"哈希，迁移成兜底键
    if (key != 0) tmp.docs.emplace_back(key, std::move(r));
    return true;
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

bool rekey(ReaderState& s, std::size_t idx, std::uint64_t new_key) noexcept {
    if (idx >= s.docs.size() || new_key == 0) return false;
    for (std::size_t i = 0; i < s.docs.size(); ++i) {
        if (i != idx && s.docs[i].first == new_key) return false;   // 已被占用
    }
    s.docs[idx].first = new_key;
    return true;
}

// ---- 已知位置（ADR-065）----
void remember_location(DocRecord& r, const std::string& path_u8) {
    if (path_u8.empty()) return;
    for (auto it = r.locations.begin(); it != r.locations.end(); ++it) {
        if (*it == path_u8) {
            if (it == r.locations.begin()) return;          // 已在最前，无事可做
            r.locations.erase(it);                          // 挪到最前：最近打开的优先
            break;
        }
    }
    r.locations.insert(r.locations.begin(), path_u8);
    if (r.locations.size() > kMaxLocations) r.locations.resize(kMaxLocations);
}

const std::string& last_location(const DocRecord& r) noexcept {
    static const std::string kEmpty;
    return r.locations.empty() ? kEmpty : r.locations.front();
}

bool has_location(const DocRecord& r, const std::string& path_u8) noexcept {
    if (path_u8.empty()) return false;
    for (const std::string& p : r.locations)
        if (p == path_u8) return true;
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

// ---- 身份分层（ADR-062）----

std::uint64_t primary_key(const DocIdentity& id) noexcept {
    if (id.content != 0) return id.content;
    return id.path;   // 都取不到时返回 0 = 不参与存取
}

DocMatch locate(const ReaderState& s, const DocIdentity& id, bool smart) noexcept {
    DocMatch m;
    const std::size_t n = s.docs.size();

    // 页数已知且与记录不符 → 不是同一版（书签会错位），不接受这条记录
    const auto usable = [&](const DocRecord& r) noexcept {
        return id.page_count <= 0 || r.page_count <= 0 || r.page_count == id.page_count;
    };

    // 1) 内容指纹：文件被移动 / 复制 / 重新解压后仍能命中
    if (smart && id.content != 0) {
        for (std::size_t i = 0; i < n; ++i) {
            if (s.docs[i].first != id.content) continue;
            const DocRecord& r = s.docs[i].second;
            if (!usable(r)) continue;
            m.index = static_cast<int>(i);
            m.relocated = !r.locations.empty() && !has_location(r, id.path_u8);
            return m;
        }
    }

    // 2) 路径键：同一位置直接沿用（v1 记录也经这一层迁移）
    if (id.path != 0) {
        for (std::size_t i = 0; i < n; ++i) {
            const DocRecord& r = s.docs[i].second;
            if (s.docs[i].first != id.path && r.path_key != id.path) continue;
            if (!usable(r)) continue;
            m.index = static_cast<int>(i);
            m.relocated = false;   // 路径键含路径，能命中就说明位置没变
            return m;
        }
    }
    return m;
}

std::uint64_t adopt(ReaderState& s, std::size_t idx, const DocIdentity& id) noexcept {
    if (idx >= s.docs.size()) return 0;
    DocRecord& r = s.docs[idx].second;
    r.path_key = id.path;
    r.page_count = id.page_count;
    remember_location(r, id.path_u8);
    const std::uint64_t key = primary_key(id);
    if (key != 0) s.docs[idx].first = key;   // 改挂到内容指纹下：以后换位置也能直接命中
    return s.docs[idx].first;
}

// ---- 序列化 ----

std::vector<std::uint8_t> encode_state(const ReaderState& s) {
    std::vector<std::uint8_t> b;
    b.reserve(64 + s.docs.size() * 64);
    b.push_back('L'); b.push_back('R'); b.push_back('S'); b.push_back('3');
    put_u32(b, kStateVersion);
    put_u32(b, static_cast<std::uint32_t>(s.docs.size()));

    for (const auto& kv : s.docs) {
        const DocRecord& r = kv.second;
        put_u64(b, kv.first);
        put_u64(b, r.path_key);
        put_u32(b, static_cast<std::uint32_t>(r.page_count));
        // 位置按上限截断（超长路径不进文件，宁可丢"上次在哪"也不让文件膨胀）
        const std::size_t ln = r.locations.size() > kMaxLocations ? kMaxLocations
                                                                  : r.locations.size();
        put_u32(b, static_cast<std::uint32_t>(ln));
        for (std::size_t i = 0; i < ln; ++i) {
            std::uint32_t plen = static_cast<std::uint32_t>(r.locations[i].size());
            if (plen > kMaxPathBytes) plen = kMaxPathBytes;
            put_u32(b, plen);
            b.insert(b.end(), r.locations[i].begin(), r.locations[i].begin() + plen);
        }

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

    // magic 决定版本：LRS1 → v1（旧格式，key 即路径键），LRS2 → v2（单路径），LRS3 → v3
    if (rd.p[0] != 'L' || rd.p[1] != 'R' || rd.p[2] != 'S') return false;
    int version = 0;
    if (rd.p[3] == '1')      version = 1;
    else if (rd.p[3] == '2') version = 2;
    else if (rd.p[3] == '3') version = 3;
    else return false;
    rd.off = 4;

    const std::uint32_t file_version = rd.u32();
    if (rd.bad || file_version != static_cast<std::uint32_t>(version)) return false;

    const std::uint32_t count = rd.u32();
    if (rd.bad || count > kMaxDocs) return false;

    ReaderState tmp;
    tmp.docs.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t key = rd.u64();
        bool ok = false;
        switch (version) {
            case 3:  ok = read_record_v3(rd, key, tmp); break;
            case 2:  ok = read_record_v2(rd, key, tmp); break;
            default: ok = read_record_v1(rd, key, tmp); break;
        }
        if (!ok) return false;
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
