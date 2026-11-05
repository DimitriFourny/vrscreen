#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>

// A small append-only text log in the app's files dir that survives logcat rotation (read it with
// `adb shell run-as com.df0.vrscreen cat files/boundary.log`). Each line starts with the local
// wall-clock time. When the file passes maxBytes it is renamed to <name>.1 (replacing the previous
// one) and a new file is started, so at most ~2 x maxBytes are kept. Meant for rare events (a few
// lines a minute): every line opens, appends and closes the file. Platform-free (host-tested).
namespace qm {

class RotatingLog {
public:
    RotatingLog() = default;
    RotatingLog(std::string path, long maxBytes) : path_(std::move(path)), maxBytes_(maxBytes) {}

    void setPath(std::string path, long maxBytes) {
        path_ = std::move(path);
        maxBytes_ = maxBytes;
    }
    const std::string& path() const { return path_; }

    void write(const char* format, ...) __attribute__((format(printf, 2, 3))) {
        if (path_.empty()) return;
        char line[512];
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        const size_t stamp = std::strftime(line, sizeof(line), "%Y-%m-%d %H:%M:%S ", &local);
        va_list args;
        va_start(args, format);
        std::vsnprintf(line + stamp, sizeof(line) - stamp, format, args);
        va_end(args);
        FILE* f = std::fopen(path_.c_str(), "a");
        if (f == nullptr) return;
        std::fputs(line, f);
        std::fputc('\n', f);
        const long size = std::ftell(f);
        std::fclose(f);
        if (size > maxBytes_) std::rename(path_.c_str(), (path_ + ".1").c_str());
    }

private:
    std::string path_;
    long maxBytes_ = 64 * 1024;
};

}  // namespace qm
