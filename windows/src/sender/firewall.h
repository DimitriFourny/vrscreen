// Windows Firewall: whether inbound connections to this executable are allowed on the current
// network profile (Wi-Fi needs them; USB through adb does not), and the elevated fix.
#pragma once

#include <string>

namespace vr {

struct FirewallState {
    bool checked = false;
    bool blocked = false;     // inbound connections from the LAN would be dropped
    bool publicNetwork = false;
    std::string detail;       // why, for the problem list
};

FirewallState checkFirewall(const std::wstring& exePath);
// Runs netsh elevated (UAC prompt): removes this program's rules (including the block rules
// Windows creates when its prompt is dismissed) and allows inbound connections from the local
// subnet on the current profile(s) and private networks. Blocking; false if cancelled/failed.
bool allowThroughFirewall(const std::wstring& exePath, bool includePublic);

}  // namespace vr
