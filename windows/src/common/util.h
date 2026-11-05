// Small helpers shared by the sender, the viewer and the tests.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vr {

// The sender clock (docs/protocol.md, "Clocks"): QueryPerformanceCounter in microseconds.
// Monotonic, unrelated to wall time. Desktop Duplication's LastPresentTime and WASAPI's
// QPC positions are on the same counter, so FRAME and AUDIO times are comparable.
uint64_t nowMicros();
// A raw QueryPerformanceCounter value (e.g. DXGI_OUTDUPL_FRAME_INFO::LastPresentTime) in µs.
uint64_t qpcTicksToMicros(int64_t ticks);
// A WASAPI QPC position (100 ns units) in µs.
inline uint64_t hundredNsToMicros(uint64_t v) { return v / 10; }

std::wstring widen(std::string_view utf8);
std::string narrow(std::wstring_view wide);

// "0x80070005 (Access is denied.)"
std::string hresultText(long hr);
// Text of GetLastError()/WSAGetLastError() codes.
std::string win32ErrorText(unsigned long code);

std::string formatString(const char* fmt, ...);

std::string trim(std::string_view s);
std::vector<std::string> splitLines(std::string_view s);
std::vector<std::string> splitWhitespace(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
bool containsIgnoreCase(std::string_view haystack, std::string_view needle);

// Big-endian appenders and readers for the wire format.
inline void putU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }
inline void putU16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}
inline void putU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
inline void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int s = 56; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
inline uint16_t getU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
inline uint32_t getU32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline uint64_t getU64(const uint8_t* p) { return uint64_t(getU32(p)) << 32 | getU32(p + 4); }

// Directory of the running executable (no trailing separator).
std::wstring exeDirectory();
std::wstring exePath();
// %APPDATA%\VRScreen and %LOCALAPPDATA%\VRScreen (created on demand by the callers).
std::wstring roamingDataDirectory();
std::wstring localDataDirectory();
bool createDirectories(const std::wstring& path);

}  // namespace vr
