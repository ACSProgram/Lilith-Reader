// utils.ixx — Lilith Reader 通用工具模块（Phase 1）
// 路径/编码转换/扩展名闸门 + ini 读写（窗口状态 [window] 与用户偏好 [ui]/[cache]）。
// 纪律：不抛异常跨边界，所有 Win32 调用失败均有合理回退。

module;
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

export module lilithreader.utils;

export namespace lr {

// ---- 受支持的文档格式（渲染核心 Phase 2 经 MuPDF 接入） ----
//
// 这是一道**策略闸门**（ADR-013）：只有清单内的扩展名才会进入后台线程，
// 内容终究交给 MuPDF 按内容识别（识别结果与扩展名不符时如何处置见 ADR-016）。
// 单页图片（png/jpg/gif/bmp/tif）在 MuPDF 里本就是"1 页文档"，故正式纳入清单。
inline constexpr std::wstring_view kSupportedExtensions[] = {
    L".pdf", L".epub", L".mobi", L".fb2", L".cbz", L".xps",
    // 单页图片（ADR-017）
    L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".tif", L".tiff",
};

inline std::wstring to_lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// 小写扩展名（含点）；无扩展名返回空串。目录名中的点不算扩展名。
inline std::wstring extension_of(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    const size_t sep = path.find_last_of(L"\\/");
    if (sep != std::wstring::npos && dot < sep) return L"";
    return to_lower(path.substr(dot));
}

inline std::wstring file_name_of(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return sep == std::wstring::npos ? path : path.substr(sep + 1);
}

inline bool file_exists(const std::wstring& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool is_supported(const std::wstring& path) {
    const std::wstring ext = extension_of(path);
    for (const std::wstring_view e : kSupportedExtensions)
        if (ext == e) return true;
    return false;
}

// ---- 稀疏采样文件指纹（ADR-062）----
//
// 用途：判断"两个路径上的文件是不是同一份内容"，让阅读状态（读到哪/书签）在文件被
// 复制、移动、重命名、重新解压之后仍能沿用 —— 路径会变，内容不变。
//
// 为什么是"稀疏采样"而不是全文件哈希：全量读一遍对几百 MB 的 PDF 是与渲染抢 I/O 的重活，
// 而这里只需要"区分不同文档"这一档强度。取**头 128KB + 中间 64KB + 尾 128KB**（合计
// ≤ 320KB）再叠上文件大小：PDF 的头（版本/元数据）与尾（xref/对象表）恰好是最能区分
// 两份文档的区域，中段补一刀防止"头尾相同、正文不同"的构造性碰撞。
// 开销与文件大小**无关**，可在工作线程里廉价计算（UI 线程不读文件内容，ADR-009）。
//
// 返回 0 = 不可用（打不开/读不全/空文件），调用方退回路径键即可。
inline constexpr std::size_t kFingerprintHeadBytes = 128u * 1024u;
inline constexpr std::size_t kFingerprintMidBytes  = 64u  * 1024u;
inline constexpr std::size_t kFingerprintTailBytes = 128u * 1024u;

inline std::uint64_t file_fingerprint(const std::wstring& path) noexcept {
    if (path.empty()) return 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;

    LARGE_INTEGER fsz{};
    if (!GetFileSizeEx(h, &fsz) || fsz.QuadPart <= 0) { CloseHandle(h); return 0; }
    const std::uint64_t size = static_cast<std::uint64_t>(fsz.QuadPart);

    // 与 reader_state 的文档键同一套 FNV-1a 64：项目里只有一种哈希，不必引入依赖。
    constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t kFnvPrime = 1099511628211ull;
    std::uint64_t fp = kFnvOffset;
    const auto mix = [&fp](const unsigned char* p, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) { fp ^= static_cast<std::uint64_t>(p[i]); fp *= kFnvPrime; }
    };
    mix(reinterpret_cast<const unsigned char*>(&size), sizeof size);

    // 三段（小文件会互相重叠、等价于整篇参与，确定性不受影响）
    std::uint64_t offs[3] = { 0, size / 2, 0 };
    std::size_t   lens[3] = { 0, 0, 0 };
    const std::size_t cap = (std::size_t)(size < kFingerprintHeadBytes ? size : kFingerprintHeadBytes);
    lens[0] = cap;
    const std::size_t mid_cap = (std::size_t)(size - offs[1] < kFingerprintMidBytes
                                                  ? size - offs[1] : kFingerprintMidBytes);
    lens[1] = mid_cap;
    const std::size_t tail_cap = (std::size_t)(size < kFingerprintTailBytes ? size : kFingerprintTailBytes);
    lens[2] = tail_cap;
    offs[2] = size - tail_cap;

    std::vector<unsigned char> buf(kFingerprintHeadBytes);
    for (int i = 0; i < 3; ++i) {
        if (lens[i] == 0) continue;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(offs[i]);
        if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN)) { CloseHandle(h); return 0; }
        DWORD got = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(lens[i]), &got, nullptr) ||
            got != static_cast<DWORD>(lens[i])) { CloseHandle(h); return 0; }
        mix(buf.data(), lens[i]);
    }
    CloseHandle(h);
    return fp == 0 ? 1 : fp;   // 0 保留为"不可用"
}

inline std::wstring to_absolute(const std::wstring& path) {
    wchar_t buf[32768]{};
    if (GetFullPathNameW(path.c_str(), 32768, buf, nullptr) && buf[0])
        return buf;
    return path;
}

inline std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(),
                                      static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// exe 所在目录（末尾含反斜杠）。失败时返回 L".\\"（工作目录兜底）。
inline std::wstring exe_dir() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(),
                                       static_cast<DWORD>(path.size()));
    path.resize(n);
    const size_t sep = path.find_last_of(L"\\/");
    if (sep == std::wstring::npos) return L".\\";
    path.resize(sep + 1);
    return path;
}

// ---- 窗口状态持久化（exe 同目录 ini，节 [window]） ----
// 说明：GetPrivateProfileIntW 不支持负数（多显示器有负坐标），
//       故经 GetPrivateProfileStringW + wcstol 读取。
struct WindowState {
    int x = 0, y = 0, w = 0, h = 0;
    bool maximized = false;
    bool valid = false;  // ini 中存在完整记录
};

// ---- 通用 ini 读写（exe 同目录；节/键由调用方给出） ----

inline int read_ini_int(const std::wstring& ini, const wchar_t* key, int fallback);

// 通用 ini 整数读取（指定节）。供 [cache] BudgetMB 与 [window] 等设置项使用。
// 缺键/非法值返回 fallback；负数与超范围由调用方钳制。
inline int read_ini_int_ex(const std::wstring& ini, const wchar_t* section,
                           const wchar_t* key, int fallback) {
    wchar_t buf[32]{};
    const DWORD n = GetPrivateProfileStringW(section, key, L"", buf, 32, ini.c_str());
    if (n == 0 || n >= 31) return fallback;
    wchar_t* end = nullptr;
    const long v = wcstol(buf, &end, 10);
    if (end == buf) return fallback;
    return static_cast<int>(v);
}

// [window] 节的整数读取（窗口状态专用，见 load_window_state）。
inline int read_ini_int(const std::wstring& ini, const wchar_t* key, int fallback) {
    return read_ini_int_ex(ini, L"window", key, fallback);
}

// 通用 ini 浮点读取（指定节）。缺键/非法值返回 fallback；范围钳制由调用方负责。
inline float read_ini_float_ex(const std::wstring& ini, const wchar_t* section,
                               const wchar_t* key, float fallback) {
    wchar_t buf[64]{};
    const DWORD n = GetPrivateProfileStringW(section, key, L"", buf, 64, ini.c_str());
    if (n == 0 || n >= 63) return fallback;
    wchar_t* end = nullptr;
    const double v = wcstod(buf, &end);
    if (end == buf) return fallback;
    return static_cast<float>(v);
}

// 通用 ini 写入（指定节；Phase 6 设置界面即时落盘用）。
inline void write_ini_int(const std::wstring& ini, const wchar_t* section,
                          const wchar_t* key, int value) {
    wchar_t buf[32]{};
    _itow_s(value, buf, 10);
    WritePrivateProfileStringW(section, key, buf, ini.c_str());
}

inline void write_ini_float(const std::wstring& ini, const wchar_t* section,
                            const wchar_t* key, float value) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"%.4f", static_cast<double>(value));
    WritePrivateProfileStringW(section, key, buf, ini.c_str());
}

// 通用 ini 字符串读取（指定节）。缺键/被截断返回 fallback。按键绑定 [keys] 用。
inline std::wstring read_ini_string_ex(const std::wstring& ini, const wchar_t* section,
                                       const wchar_t* key, const wchar_t* fallback) {
    wchar_t buf[512]{};
    const DWORD n = GetPrivateProfileStringW(section, key, fallback, buf, 512, ini.c_str());
    if (n == 0 || n >= 511) return std::wstring(fallback != nullptr ? fallback : L"");
    return std::wstring(buf, n);
}

inline void write_ini_string(const std::wstring& ini, const wchar_t* section,
                             const wchar_t* key, const wchar_t* value) {
    WritePrivateProfileStringW(section, key, value, ini.c_str());
}

// UTF-8 → UTF-16（按键绑定值在 ini 里是 UTF-8 文本，读取时需转回宽字符）。
inline std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline WindowState load_window_state(const std::wstring& ini_path) {
    constexpr int kAbsent = INT_MAX;
    WindowState s;
    s.x = read_ini_int(ini_path, L"X", kAbsent);
    s.y = read_ini_int(ini_path, L"Y", kAbsent);
    s.w = read_ini_int(ini_path, L"Width", kAbsent);
    s.h = read_ini_int(ini_path, L"Height", kAbsent);
    s.maximized = read_ini_int(ini_path, L"Maximized", 0) != 0;
    s.valid = (s.x != kAbsent && s.y != kAbsent &&
               s.w != kAbsent && s.h != kAbsent);
    return s;
}

inline void save_window_state(const std::wstring& ini_path, const WindowState& s) {
    if (!s.valid) return;
    auto write = [&](const wchar_t* key, int v) {
        wchar_t buf[32]{};
        _itow_s(v, buf, 10);
        WritePrivateProfileStringW(L"window", key, buf, ini_path.c_str());
    };
    write(L"X", s.x);
    write(L"Y", s.y);
    write(L"Width", s.w);
    write(L"Height", s.h);
    write(L"Maximized", s.maximized ? 1 : 0);
}

} // namespace lr
