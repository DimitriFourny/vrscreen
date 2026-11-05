#include "util.h"

#include <windows.h>
#include <shlobj.h>

#include <cctype>
#include <cstdarg>
#include <cstdio>

namespace vr {
namespace {
int64_t qpcFrequency() {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency;
}
}  // namespace

uint64_t qpcTicksToMicros(int64_t ticks) {
    const int64_t f = qpcFrequency();
    if (ticks < 0) return 0;
    return uint64_t(ticks / f) * 1'000'000 + uint64_t(ticks % f) * 1'000'000 / uint64_t(f);
}

uint64_t nowMicros() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return qpcTicksToMicros(now.QuadPart);
}

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string win32ErrorText(unsigned long code) {
    wchar_t* buffer = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text;
    if (n && buffer) text = trim(narrow(std::wstring_view(buffer, n)));
    if (buffer) LocalFree(buffer);
    if (text.empty()) text = formatString("error %lu", code);
    return text;
}

std::string hresultText(long hr) {
    std::string text = formatString("0x%08lX", static_cast<unsigned long>(hr));
    std::string message = win32ErrorText(static_cast<unsigned long>(hr));
    if (!startsWith(message, "error ")) text += " (" + message + ")";
    return text;
}

std::string formatString(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char stack[512];
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(stack, sizeof stack, fmt, copy);
    va_end(copy);
    std::string out;
    if (n < 0) {
        out = fmt;
    } else if (size_t(n) < sizeof stack) {
        out.assign(stack, size_t(n));
    } else {
        out.resize(size_t(n) + 1);
        vsnprintf(out.data(), out.size(), fmt, args);
        out.resize(size_t(n));
    }
    va_end(args);
    return out;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

std::vector<std::string> splitLines(std::string_view s) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find_first_of("\r\n", start);
        if (end == std::string_view::npos) end = s.size();
        if (end > start) lines.emplace_back(s.substr(start, end - start));
        start = end + 1;
    }
    return lines;
}

std::vector<std::string> splitWhitespace(std::string_view s) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        if (j > i) tokens.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return tokens;
}

bool startsWith(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }

bool containsIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() && std::tolower(static_cast<unsigned char>(haystack[i + k])) ==
                                        std::tolower(static_cast<unsigned char>(needle[k])))
            ++k;
        if (k == needle.size()) return true;
    }
    return false;
}

std::wstring exePath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (n < path.size()) {
            path.resize(n);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring exeDirectory() {
    std::wstring path = exePath();
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

static std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &raw))) out = raw;
    CoTaskMemFree(raw);
    return out;
}

std::wstring roamingDataDirectory() { return knownFolder(FOLDERID_RoamingAppData) + L"\\VRScreen"; }
std::wstring localDataDirectory() { return knownFolder(FOLDERID_LocalAppData) + L"\\VRScreen"; }

bool createDirectories(const std::wstring& path) {
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    return result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS || result == ERROR_FILE_EXISTS;
}

}  // namespace vr
