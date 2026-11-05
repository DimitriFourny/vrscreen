// Logging: every line goes to stdout (when there is a console) and, once `openLogFile` was
// called, to a file that is rotated at 5 MB (`vrscreen.log` → `vrscreen.1.log`).
#pragma once

#include <string>

namespace vr {

void log(const std::string& line);
// printf-style.
void logf(const char* fmt, ...);
// Opens (appending) the log file; returns false if it cannot be written.
bool openLogFile(const std::wstring& path);
std::wstring logFilePath();

}  // namespace vr
