#include "firewall.h"

#include <windows.h>
#include <netfw.h>
#include <shellapi.h>
#include <wrl/client.h>

#include "log.h"
#include "util.h"

namespace vr {

using Microsoft::WRL::ComPtr;

static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}

FirewallState checkFirewall(const std::wstring& exePath) {
    FirewallState state;
    ComPtr<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) return state;
    long profiles = 0;
    if (FAILED(policy->get_CurrentProfileTypes(&profiles))) return state;
    state.checked = true;
    state.publicNetwork = (profiles & NET_FW_PROFILE2_PUBLIC) != 0;
    bool enabled = false, blockAll = false;
    for (long p : {NET_FW_PROFILE2_DOMAIN, NET_FW_PROFILE2_PRIVATE, NET_FW_PROFILE2_PUBLIC}) {
        if (!(profiles & p)) continue;
        VARIANT_BOOL on = VARIANT_FALSE, block = VARIANT_FALSE;
        policy->get_FirewallEnabled(NET_FW_PROFILE_TYPE2(p), &on);
        policy->get_BlockAllInboundTraffic(NET_FW_PROFILE_TYPE2(p), &block);
        enabled |= on != VARIANT_FALSE;
        blockAll |= on != VARIANT_FALSE && block != VARIANT_FALSE;
    }
    if (!enabled) return state;
    if (blockAll) {
        state.blocked = true;
        state.detail = "Windows Firewall blocks all incoming connections on this network.";
        return state;
    }
    ComPtr<INetFwRules> rules;
    ComPtr<IUnknown> unknown;
    ComPtr<IEnumVARIANT> it;
    if (FAILED(policy->get_Rules(&rules)) || FAILED(rules->get__NewEnum(&unknown)) || FAILED(unknown.As(&it))) {
        state.checked = false;
        return state;
    }
    const std::wstring me = lower(exePath);
    bool allowed = false, blockedByRule = false;
    VARIANT v;
    VariantInit(&v);
    ULONG fetched = 0;
    while (it->Next(1, &v, &fetched) == S_OK && fetched == 1) {
        ComPtr<INetFwRule> rule;
        if (v.vt == VT_DISPATCH && v.pdispVal && SUCCEEDED(v.pdispVal->QueryInterface(IID_PPV_ARGS(&rule)))) {
            BSTR app = nullptr;
            if (SUCCEEDED(rule->get_ApplicationName(&app)) && app && lower(app) == me) {
                VARIANT_BOOL ruleEnabled = VARIANT_FALSE;
                NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
                NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
                long ruleProfiles = 0;
                rule->get_Enabled(&ruleEnabled);
                rule->get_Direction(&direction);
                rule->get_Action(&action);
                rule->get_Profiles(&ruleProfiles);
                if (ruleEnabled != VARIANT_FALSE && direction == NET_FW_RULE_DIR_IN && (ruleProfiles & profiles)) {
                    if (action == NET_FW_ACTION_BLOCK) blockedByRule = true;
                    else allowed = true;
                }
            }
            SysFreeString(app);
        }
        VariantClear(&v);
    }
    if (blockedByRule || !allowed) {
        state.blocked = true;
        state.detail = blockedByRule ? "A Windows Firewall rule blocks VRScreen on this network"
                                     : "Windows Firewall has no rule allowing VRScreen on this network";
        state.detail += state.publicNetwork ? " (a Public network)." : ".";
    }
    return state;
}

bool allowThroughFirewall(const std::wstring& exePath, bool includePublic) {
    const std::wstring profiles = includePublic ? L"private,domain,public" : L"private,domain";
    const std::wstring args = L"/c netsh advfirewall firewall delete rule name=all program=\"" + exePath +
                              L"\" & netsh advfirewall firewall add rule name=\"VRScreen\" dir=in action=allow program=\"" +
                              exePath + L"\" enable=yes profile=" + profiles + L" remoteip=localsubnet";
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        log("firewall: elevation cancelled or failed: " + win32ErrorText(GetLastError()));
        return false;
    }
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        log(formatString("firewall: netsh finished with exit code %lu", code));
        return code == 0;
    }
    return true;
}

}  // namespace vr
