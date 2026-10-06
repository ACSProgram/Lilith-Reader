// reader_state_test.cpp — 阅读状态序列化自动化测试（Phase 5）
//
// 做法：**直接链接项目自己的 lilithreader.reader_state 模块**，对纯序列化
// （encode_state/decode_state）与文档键（document_key）逐一断言。
// 状态文件是"锦上添花"——解析失败必须安全拒绝，绝不能因损坏而崩溃或影响打开文档，
// 因此这里把"坏输入一律拒绝"钉成规格。
//
// 退出码：0 = 全部通过，1 = 有 FAIL。
// 用法： reader_state_test.exe

#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

import lilithreader.utils;           // file_fingerprint（ADR-062）
import lilithreader.reader_state;

namespace {

int g_pass = 0, g_fail = 0;

void check(bool cond, const char* name) {
    if (cond) { ++g_pass; std::printf("  [PASS] %s\n", name); }
    else      { ++g_fail; std::printf("  [FAIL] %s\n", name); }
}

void check_eq_u64(std::uint64_t got, std::uint64_t want, const char* name) {
    if (got == want) { ++g_pass; std::printf("  [PASS] %-46s (%llu)\n", name,
                                              (unsigned long long)got); }
    else { ++g_fail; std::printf("  [FAIL] %-46s 期望 %llu，实际 %llu\n", name,
                                 (unsigned long long)want, (unsigned long long)got); }
}

void check_near(float got, float want, const char* name) {
    if (std::fabs(got - want) <= 1e-4f) { ++g_pass; std::printf("  [PASS] %s\n", name); }
    else { ++g_fail; std::printf("  [FAIL] %s（期望 %.4f，实际 %.4f）\n", name,
                                 (double)want, (double)got); }
}

// ---- 小端写入（用于手工构造损坏输入） ----
void put_u32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back((std::uint8_t)(v & 0xFF));
    b.push_back((std::uint8_t)((v >> 8) & 0xFF));
    b.push_back((std::uint8_t)((v >> 16) & 0xFF));
    b.push_back((std::uint8_t)((v >> 24) & 0xFF));
}
void put_u64(std::vector<std::uint8_t>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back((std::uint8_t)((v >> (8 * i)) & 0xFF));
}
std::vector<std::uint8_t> header(const char* magic, std::uint32_t version, std::uint32_t count) {
    std::vector<std::uint8_t> b;
    for (int i = 0; i < 4; ++i) b.push_back((std::uint8_t)magic[i]);
    put_u32(b, version);
    put_u32(b, count);
    return b;
}

// 写临时文件（供指纹测试构造"同一份内容 / 差一个字节"的样本）
bool write_file(const std::wstring& p, const std::string& data) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr);
    CloseHandle(h);
    return ok && w == data.size();
}

// ---- 1. 序列化往返 ----
lr::ReaderState make_state() {
    lr::ReaderState s;
    lr::DocRecord& a = s.upsert(0x1122334455667788ull);
    a.page = 42; a.zoom = 1.75f; a.columns = 2; a.rotation = 90;
    a.fit_width = false; a.spread = true; a.scheme = 2;
    a.bookmarks.push_back(lr::Bookmark{ 3, "start" });
    a.bookmarks.push_back(lr::Bookmark{ 100, "" });
    lr::DocRecord& b = s.upsert(0xdeadbeefcafebabeull);
    b.page = 0; b.zoom = 1.0f; b.columns = 4; b.rotation = 270;
    b.fit_width = true; b.spread = false; b.scheme = 1;
    return s;
}

void test_round_trip() {
    std::printf("\n[1] 序列化往返\n");
    const lr::ReaderState in = make_state();
    const std::vector<std::uint8_t> bytes = lr::encode_state(in);
    check(!bytes.empty(), "编码非空");

    lr::ReaderState out;
    check(lr::decode_state(bytes.data(), bytes.size(), out), "解码成功");
    check(out.docs.size() == 2, "文档条数 = 2");

    const lr::DocRecord* a = out.find(0x1122334455667788ull);
    check(a != nullptr, "找到文档 A");
    if (a) {
        check(a->page == 42, "A.page");
        check_near(a->zoom, 1.75f, "A.zoom");
        check(a->columns == 2, "A.columns");
        check(a->rotation == 90, "A.rotation");
        check(a->fit_width == false, "A.fit_width");
        check(a->spread == true, "A.spread");
        check(a->scheme == 2, "A.scheme");
        check(a->bookmarks.size() == 2, "A 书签数 = 2");
        if (a->bookmarks.size() == 2) {
            check(a->bookmarks[0].page == 3 && a->bookmarks[0].label == "start", "书签 0 内容");
            check(a->bookmarks[1].page == 100 && a->bookmarks[1].label.empty(), "书签 1 内容");
        }
    }
    const lr::DocRecord* b = out.find(0xdeadbeefcafebabeull);
    check(b != nullptr && b->rotation == 270 && b->scheme == 1, "文档 B 字段");

    // 往返二次编码应逐字节一致（确定性）
    const std::vector<std::uint8_t> bytes2 = lr::encode_state(out);
    check(bytes == bytes2, "二次编码逐字节一致");
}

// ---- 2. 空状态与容器语义 ----
void test_container_semantics() {
    std::printf("\n[2] 容器语义\n");
    lr::ReaderState s;
    check(s.find(0) == nullptr, "key=0 查不到（无效键）");
    check(s.find(123) == nullptr, "不存在的 key 返回 nullptr");

    lr::DocRecord& r = s.upsert(123);
    r.page = 7;
    check(s.docs.size() == 1, "upsert 新建一条");
    lr::DocRecord& r2 = s.upsert(123);
    check(&r2 == &r && s.docs.size() == 1, "再次 upsert 命中同一条");
    check(s.erase(123), "erase 命中");
    check(!s.erase(123), "erase 二次返回 false");
    check(s.find(123) == nullptr, "erase 后查不到");

    lr::ReaderState empty;
    const std::vector<std::uint8_t> bytes = lr::encode_state(empty);
    lr::ReaderState out;
    check(lr::decode_state(bytes.data(), bytes.size(), out), "空状态可解码");
    check(out.docs.empty(), "空状态解码后无记录");
}

// ---- 3. 坏输入一律安全拒绝 ----
void test_bad_input() {
    std::printf("\n[3] 坏输入拒绝\n");
    lr::ReaderState out;
    out.upsert(1);  // 先塞一条，验证失败时 out 不被改动

    // 空/太短
    check(!lr::decode_state(nullptr, 0, out), "nullptr 拒绝");
    const std::uint8_t tiny[4] = { 'L', 'R', 'S', '1' };
    check(!lr::decode_state(tiny, sizeof tiny, out), "仅 magic 拒绝");
    check(out.docs.size() == 1, "失败不改动 out");

    // magic 错
    std::vector<std::uint8_t> bad_magic = header("XXX1", lr::kStateVersion, 0);
    check(!lr::decode_state(bad_magic.data(), bad_magic.size(), out), "magic 错拒绝");

    // 版本不符
    std::vector<std::uint8_t> bad_ver = header("LRS1", lr::kStateVersion + 1, 0);
    check(!lr::decode_state(bad_ver.data(), bad_ver.size(), out), "版本不符拒绝");

    // magic 与版本必须"配对"：LRS2 配 v1 也是损坏（版本与 magic 不符）
    std::vector<std::uint8_t> mixed = header("LRS2", lr::kStateVersionV1, 0);
    check(!lr::decode_state(mixed.data(), mixed.size(), out), "magic/版本不配对拒绝");

    // 条数超上限
    std::vector<std::uint8_t> too_many = header("LRS1", lr::kStateVersionV1, lr::kMaxDocs + 1);
    check(!lr::decode_state(too_many.data(), too_many.size(), out), "条数超上限拒绝");

    // 记录被截断（声明 1 条但没有记录体）
    std::vector<std::uint8_t> truncated = header("LRS1", lr::kStateVersionV1, 1);
    put_u64(truncated, 0xABCDEF);  // 只有 key，后面字段缺失
    check(!lr::decode_state(truncated.data(), truncated.size(), out), "记录截断拒绝");

    // v2 路径长度超上限
    std::vector<std::uint8_t> pl = header("LRS2", lr::kStateVersionV2, 1);
    put_u64(pl, 1);
    put_u64(pl, 0);
    put_u32(pl, 0);
    put_u32(pl, lr::kMaxPathBytes + 1);   // 声明的路径长度超限
    check(!lr::decode_state(pl.data(), pl.size(), out), "v2 路径长度超上限拒绝");

    // v3 位置条数超上限
    std::vector<std::uint8_t> loc_over = header("LRS3", lr::kStateVersion, 1);
    put_u64(loc_over, 1);
    put_u64(loc_over, 0);
    put_u32(loc_over, 0);
    put_u32(loc_over, static_cast<std::uint32_t>(lr::kMaxLocations) + 1);
    check(!lr::decode_state(loc_over.data(), loc_over.size(), out), "位置条数超上限拒绝");

    // v3 单条位置长度超上限
    std::vector<std::uint8_t> loc_len = header("LRS3", lr::kStateVersion, 1);
    put_u64(loc_len, 1);
    put_u64(loc_len, 0);
    put_u32(loc_len, 0);
    put_u32(loc_len, 1);
    put_u32(loc_len, lr::kMaxPathBytes + 1);
    check(!lr::decode_state(loc_len.data(), loc_len.size(), out), "位置长度超上限拒绝");

    // 书签数超上限
    std::vector<std::uint8_t> bm_over = header("LRS1", lr::kStateVersionV1, 1);
    put_u64(bm_over, 1);           // key
    put_u32(bm_over, 0);           // page
    put_u32(bm_over, 0x3F800000);  // zoom = 1.0f
    put_u32(bm_over, 1);           // columns
    put_u32(bm_over, 0);           // rotation
    put_u32(bm_over, 0);           // flags
    put_u32(bm_over, 0);           // scheme
    put_u32(bm_over, lr::kMaxBookmarksPerDoc + 1);  // 书签数超限
    check(!lr::decode_state(bm_over.data(), bm_over.size(), out), "书签数超上限拒绝");
}

// ---- 4. 字段钳制（损坏值不得进入内存） ----
void test_field_clamping() {
    std::printf("\n[4] 字段钳制\n");
    std::vector<std::uint8_t> b = header("LRS1", lr::kStateVersionV1, 1);
    put_u64(b, 0x55);
    put_u32(b, 5);              // page
    put_u32(b, 0);              // zoom bits = 0.0f（非法）
    put_u32(b, 99);             // columns 越界 → 钳到 4
    put_u32(b, 450);            // rotation 450 → 归一到 90
    put_u32(b, 0);              // flags
    put_u32(b, 77);             // scheme 越界 → 钳到 2
    put_u32(b, 0);              // 书签数 0

    lr::ReaderState out;
    check(lr::decode_state(b.data(), b.size(), out), "可解码");
    const lr::DocRecord* r = out.find(0x55);
    check(r != nullptr, "记录存在");
    if (r) {
        check(r->columns == 4, "columns 钳到 4");
        check(r->rotation == 90, "rotation 450 归一到 90");
        check(r->scheme == 2, "scheme 钳到 2");
        check_near(r->zoom, 1.0f, "非法 zoom 回落到 1.0");
    }
}

// ---- 5. 文档键 ----
void test_document_key() {
    std::printf("\n[5] 文档键\n");
    check_eq_u64(lr::document_key(L"Z:\\nonexistent\\no-such-file.pdf"), 0,
                 "不存在的路径 → 0");
    check_eq_u64(lr::document_key(L""), 0, "空路径 → 0");

    wchar_t exe[1024] = {};
    GetModuleFileNameW(nullptr, exe, 1024);
    const std::uint64_t k1 = lr::document_key(exe);
    const std::uint64_t k2 = lr::document_key(exe);
    check(k1 != 0, "exe 路径键非 0");
    check_eq_u64(k1, k2, "同一路径键稳定");
}

// ---- 6. v3 身份与位置往返（ADR-062 / ADR-065）----
void test_identity_round_trip() {
    std::printf("\n[6] 身份与位置往返（v3）\n");
    lr::ReaderState s;
    lr::DocRecord& a = s.upsert(0xAAA1ull);
    a.page = 7; a.page_count = 300; a.path_key = 0xBBB1ull;
    a.locations.push_back("D:/\xE4\xB9\xA6/b.pdf");   // "D:/书/b.pdf"（UTF-8）
    a.locations.push_back("\xD0\xA1\xE8\xAF\xB4/a.pdf");   // "小读/a.pdf"
    a.bookmarks.push_back(lr::Bookmark{ 2, "x" });

    const std::vector<std::uint8_t> bytes = lr::encode_state(s);
    check(bytes.size() > 4 && bytes[0] == 'L' && bytes[1] == 'R' &&
          bytes[2] == 'S' && bytes[3] == '3', "magic = LRS3");

    lr::ReaderState out;
    check(lr::decode_state(bytes.data(), bytes.size(), out), "v3 解码成功");
    const lr::DocRecord* r = out.find(0xAAA1ull);
    check(r != nullptr, "记录存在");
    if (r) {
        check(r->page == 7, "阅读位置往返");
        check(r->path_key == 0xBBB1ull, "path_key 往返");
        check(r->page_count == 300, "page_count 往返");
        check(r->locations.size() == 2, "位置条数往返");
        check(r->locations.size() == 2 && r->locations[0] == a.locations[0] &&
              r->locations[1] == a.locations[1], "位置顺序往返（UTF-8 原样）");
        check(lr::last_location(*r) == a.locations[0], "last_location = 最前那条");
    }
}

// ---- 7. 旧格式迁移：v1（无路径）/ v2（单路径）----
void test_v1_migration() {
    std::printf("\n[7] v1 记录迁移\n");
    std::vector<std::uint8_t> b = header("LRS1", lr::kStateVersionV1, 1);
    put_u64(b, 0x1234);           // key（v1 的键就是"路径+大小+修改时间"哈希）
    put_u32(b, 11);               // page
    put_u32(b, 0x3F800000);       // zoom = 1.0f
    put_u32(b, 1);                // columns
    put_u32(b, 0);                // rotation
    put_u32(b, 0);                // flags
    put_u32(b, 0);                // scheme
    put_u32(b, 0);                // 书签数

    lr::ReaderState out;
    check(lr::decode_state(b.data(), b.size(), out), "v1 可解码");
    const lr::DocRecord* r = out.find(0x1234);
    check(r != nullptr, "v1 记录存在");
    if (r) {
        check(r->page == 11, "阅读位置保留");
        check(r->path_key == 0x1234, "旧 key → path_key（迁移）");
        check(r->page_count == 0, "page_count = 0（未知）");
        check(r->locations.empty(), "位置列表为空（v1 没存过路径）");
    }
}

void test_v2_migration() {
    std::printf("\n[7b] v2 记录迁移（单路径 → 位置列表）\n");
    const char* p = "D:/books/a.pdf";
    std::vector<std::uint8_t> b = header("LRS2", lr::kStateVersionV2, 1);
    put_u64(b, 0x2222);           // key
    put_u64(b, 0x9999);           // path_key
    put_u32(b, 120);              // page_count
    put_u32(b, static_cast<std::uint32_t>(std::strlen(p)));
    for (const char* q = p; *q != '\0'; ++q) b.push_back(static_cast<std::uint8_t>(*q));
    put_u32(b, 9);                // page
    put_u32(b, 0x3F800000);       // zoom = 1.0f
    put_u32(b, 1);                // columns
    put_u32(b, 0);                // rotation
    put_u32(b, 1);                // flags: fit_width
    put_u32(b, 0);                // scheme
    put_u32(b, 0);                // 书签数

    lr::ReaderState out;
    check(lr::decode_state(b.data(), b.size(), out), "v2 可解码");
    const lr::DocRecord* r = out.find(0x2222);
    check(r != nullptr, "v2 记录存在");
    if (r) {
        check(r->page == 9 && r->page_count == 120, "阅读位置保留");
        check(r->path_key == 0x9999, "path_key 保留");
        check(r->locations.size() == 1, "单路径迁进位置列表");
        check(!r->locations.empty() && r->locations[0] == p, "位置内容 = v2 的路径");
    }

    // v2 里路径为空（v1 迁上来后又没打开过）→ 位置列表保持空
    std::vector<std::uint8_t> e = header("LRS2", lr::kStateVersionV2, 1);
    put_u64(e, 0x3333);
    put_u64(e, 0x4444);
    put_u32(e, 0);
    put_u32(e, 0);                // 空路径
    put_u32(e, 0); put_u32(e, 0x3F800000); put_u32(e, 1); put_u32(e, 0);
    put_u32(e, 0); put_u32(e, 0); put_u32(e, 0);
    lr::ReaderState out2;
    check(lr::decode_state(e.data(), e.size(), out2), "v2 空路径可解码");
    const lr::DocRecord* re = out2.find(0x3333);
    check(re != nullptr && re->locations.empty(), "空路径不进位置列表");
}

// ---- 8. 身份分层定位（locate / adopt / primary_key）----
void test_locate_adopt() {
    std::printf("\n[8] 身份分层定位\n");
    lr::ReaderState s;
    // 一条"已迁移"的记录：主键 = 内容指纹，记着一个旧位置
    lr::DocRecord& a = s.upsert(0xC0FFEEull);
    a.page = 55; a.page_count = 200; a.path_key = 0x999;
    a.locations.push_back("D:/old/book.pdf");

    lr::DocIdentity id;   // 同一份内容，换到新路径打开
    id.content = 0xC0FFEEull; id.path = 0x777; id.page_count = 200;
    id.path_u8 = "E:/new/book.pdf";

    const lr::DocMatch hit = lr::locate(s, id, true);
    check(hit.index == 0, "内容指纹命中");
    check(hit.relocated, "没见过的位置 → relocated");

    const std::uint64_t key = lr::adopt(s, 0, id);
    check(key == 0xC0FFEEull, "adopt 后主键 = 内容指纹");
    check(s.docs[0].second.locations.front() == "E:/new/book.pdf", "新位置进列表最前");
    check(s.docs[0].second.locations.size() == 2, "旧位置仍在列表里（一份数据、两处路径）");
    check(s.docs[0].second.path_key == 0x777, "path_key 刷新");
    check(s.docs[0].second.page_count == 200, "page_count 刷新");

    const lr::DocMatch again = lr::locate(s, id, true);
    check(again.index == 0 && !again.relocated, "同路径再打开不算 relocated");

    lr::DocIdentity old_id = id;   // 回到**已记过的**那个位置：也不该再问
    old_id.path = 0x999; old_id.path_u8 = "D:/old/book.pdf";
    const lr::DocMatch back = lr::locate(s, old_id, true);
    check(back.index == 0 && !back.relocated, "回到记过的位置不算 relocated");

    lr::DocIdentity moved = id;   // 再换一个位置：靠指纹仍能命中
    moved.path = 0x1234; moved.path_u8 = "F:/x/y.pdf";
    check(lr::locate(s, moved, false).index == -1, "关掉智能匹配 → 新位置不命中");
    check(lr::locate(s, moved, true).index == 0, "开启智能匹配 → 新位置命中");

    lr::DocIdentity bad = id;     // 页数对不上：不是同一版，不继承（书签会错位）
    bad.page_count = 999;
    check(lr::locate(s, bad, true).index == -1, "页数不符不继承");

    lr::DocIdentity only_path;    // 主键：指纹优先，取不到退回路径键
    only_path.path = 0xABC;
    check_eq_u64(lr::primary_key(only_path), 0xABC, "无指纹时主键 = 路径键");
    check_eq_u64(lr::primary_key(lr::DocIdentity{}), 0, "两者都无 → 0");

    // rekey：把一份共享的阅读数据拆成两条独立记录时，给留下的那条换主键
    lr::ReaderState t;
    t.upsert(0xA1ull).locations.push_back("a");
    t.upsert(0xB2ull).locations.push_back("b");
    check(!lr::rekey(t, 0, 0xB2ull), "新键被别的记录占用 → 拒绝");
    check(!lr::rekey(t, 5, 0xC3ull), "下标越界 → 拒绝");
    check(!lr::rekey(t, 0, 0), "新键为 0 → 拒绝");
    check(lr::rekey(t, 0, 0xC3ull), "改挂成功");
    check(t.find(0xC3ull) != nullptr && t.find(0xA1ull) == nullptr, "记录已改挂");
    check(t.find(0xC3ull) != nullptr && t.find(0xC3ull)->locations.front() == "a",
          "改挂不动记录内容");
}

// ---- 10. 位置记忆（remember_location：去重 / 最近优先 / 封顶）----
void test_remember_location() {
    std::printf("\n[10] 位置记忆\n");
    lr::DocRecord r;
    check(lr::last_location(r).empty(), "空记录 → 没有最近位置");
    lr::remember_location(r, "");
    check(r.locations.empty(), "空路径不记");
    check(!lr::has_location(r, ""), "空路径不算已记过");

    lr::remember_location(r, "A");
    check(r.locations.size() == 1 && r.locations.front() == "A", "记下第一条");
    lr::remember_location(r, "B");
    check(r.locations.size() == 2 && r.locations.front() == "B", "新位置进最前");
    check(lr::last_location(r) == "B", "last_location 跟着走");
    lr::remember_location(r, "A");
    check(r.locations.size() == 2 && r.locations.front() == "A", "已记过的挪到最前、不重复");
    check(lr::has_location(r, "A") && lr::has_location(r, "B") && !lr::has_location(r, "C"),
          "has_location");

    for (int i = 0; i < 20; ++i) lr::remember_location(r, "p" + std::to_string(i));
    check(r.locations.size() == lr::kMaxLocations, "位置数封顶");
    check(r.locations.front() == "p19", "封顶时保留最新的、丢最旧的");
}

// ---- 9. 内容指纹（utils::file_fingerprint）----
void test_fingerprint() {
    std::printf("\n[9] 内容指纹\n");
    check_eq_u64(lr::file_fingerprint(L""), 0, "空路径 → 0");
    check_eq_u64(lr::file_fingerprint(L"Z:\\no\\such\\file.pdf"), 0, "不存在 → 0");

    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) == 0) { std::printf("  [SKIP] 取不到临时目录\n"); return; }
    const std::wstring dir = std::wstring(tmp) + L"lilith_fp_test";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring a = dir + L"\\a.bin";
    const std::wstring b = dir + L"\\b.bin";
    const std::wstring copy = dir + L"\\copy_of_a.bin";

    // 400KB > 采样窗口（320KB）：保证"只改中段/尾段一个字节"也会被采到
    const std::size_t n = 400u * 1024u;
    std::string data(n, '\0');
    for (std::size_t i = 0; i < n; ++i) data[i] = static_cast<char>('A' + static_cast<int>(i % 7));
    std::string mid = data;   mid[n / 2] = 'Z';
    std::string tail = data;  tail[n - 1] = 'Z';

    check(write_file(a, data) && write_file(b, mid), "样本写入成功");
    const std::uint64_t fa = lr::file_fingerprint(a);
    check(fa != 0, "指纹非 0");
    check_eq_u64(fa, lr::file_fingerprint(a), "同一文件稳定");
    check(fa != lr::file_fingerprint(b), "中段改一字节 → 指纹不同");

    check(write_file(a, tail), "尾段样本写入成功");
    check(lr::file_fingerprint(a) != fa, "尾段改一字节 → 指纹不同");
    check(write_file(a, data), "还原样本");

    if (CopyFileW(a.c_str(), copy.c_str(), FALSE)) {
        check_eq_u64(lr::file_fingerprint(copy), lr::file_fingerprint(a),
                     "复制到新路径 → 指纹相同（跨路径识别的根基）");
        DeleteFileW(copy.c_str());
    } else {
        check(false, "复制到新路径 → 指纹相同（复制失败）");
    }
    DeleteFileW(a.c_str());
    DeleteFileW(b.c_str());
    RemoveDirectoryW(dir.c_str());
}

}  // namespace

int main() {
    std::printf("Lilith Reader 阅读状态测试（Phase 5 + ADR-062/065 身份分层与位置列表）\n");
    test_round_trip();
    test_container_semantics();
    test_bad_input();
    test_field_clamping();
    test_document_key();
    test_identity_round_trip();
    test_v1_migration();
    test_v2_migration();
    test_locate_adopt();
    test_fingerprint();
    test_remember_location();

    std::printf("\n合计：通过 %d，失败 %d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
