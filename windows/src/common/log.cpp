#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "util.h"

namespace vr {
namespace {
constexpr long long kRotateBytes = 5LL * 1024 * 1024;

std::mutex g_mutex;
FILE* g_file = nullptr;
std::wstring g_path;
long long g_size = 0;

std::wstring rotatedPath(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    return dot == std::wstring::npos ? path + L".1" : path.substr(0, dot) + L".1" + path.substr(dot);
}

void rotateIfNeeded() {
    if (!g_file || g_size < kRotateBytes) return;
    fclose(g_file);
    const std::wstring old = rotatedPath(g_path);
    DeleteFileW(old.c_str());
    MoveFileW(g_path.c_str(), old.c_str());
    g_file = _wfsopen(g_path.c_str(), L"ab", _SH_DENYNO);
    if (g_file) fwrite("\xEF\xBB\xBF", 1, 3, g_file);
    g_size = 3;
}
}  // namespace

bool openLogFile(const std::wstring& path) {
    std::lock_guard lock(g_mutex);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) createDirectories(path.substr(0, slash));
    FILE* f = _wfsopen(path.c_str(), L"ab", _SH_DENYNO);
    if (!f) return false;
    if (g_file) fclose(g_file);
    g_file = f;
    g_path = path;
    _fseeki64(g_file, 0, SEEK_END);
    g_size = _ftelli64(g_file);
    if (g_size == 0) {
        fwrite("\xEF\xBB\xBF", 1, 3, g_file);  // UTF-8 BOM, so Notepad and PowerShell read it as UTF-8
        g_size = 3;
    }
    return true;
}

std::wstring logFilePath() {
    std::lock_guard lock(g_mutex);
    return g_path;
}

void log(const std::string& line) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string stamped = formatString("%04d-%02d-%02d %02d:%02d:%02d.%03d [%5lu] ", t.wYear, t.wMonth, t.wDay,
                                             t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
                                             GetCurrentProcessId()) + line + "\n";
    std::lock_guard lock(g_mutex);
    fwrite(stamped.data(), 1, stamped.size(), stdout);
    fflush(stdout);
    if (g_file) {
        fwrite(stamped.data(), 1, stamped.size(), g_file);
        fflush(g_file);
        g_size += static_cast<long long>(stamped.size());
        rotateIfNeeded();
    }
}

void logf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buffer[2048];
    vsnprintf(buffer, sizeof buffer, fmt, args);
    va_end(args);
    log(buffer);
}

}  // namespace vr
