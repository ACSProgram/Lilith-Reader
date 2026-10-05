// utils.ixx — Lilith Reader 通用工具模块（Phase 1）
// 路径/扩展名/编码转换/窗口状态 ini 持久化。
// 纪律：不抛异常跨边界，所有 Win32 调用失败均有合理回退。

module;
#include <windows.h>
#include <string>
#include <string_view>

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

inline int read_ini_int(const std::wstring& ini, const wchar_t* key, int fallback) {
    wchar_t buf[32]{};
    const DWORD n = GetPrivateProfileStringW(L"window", key, L"", buf, 32, ini.c_str());
    if (n == 0 || n >= 31) return fallback;
    wchar_t* end = nullptr;
    const long v = wcstol(buf, &end, 10);
    if (end == buf) return fallback;
    return static_cast<int>(v);
}

// 通用 ini 整数读取（指定节）。供 [cache] BudgetMB 等设置项使用（Phase 4）。
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
