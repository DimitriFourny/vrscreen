#include "usb_headset.h"

#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include <ws2tcpip.h>

#include <algorithm>

#include "log.h"
#include "util.h"

namespace vr {

// MARK: Status texts

std::string UsbStatus::linkText() const {
    switch (link) {
        case Link::Off: return "USB off (--no-adb)";
        case Link::AdbMissing: return "USB unavailable: adb not found";
        case Link::AdbBroken: return "USB unavailable: adb isn't working";
        case Link::NoHeadset:
            return metaUsbDevice.empty() ? "No headset on USB" : metaUsbDevice + " on USB, not visible to adb";
        case Link::Unauthorized: return "Headset on USB: waiting for \"Allow USB debugging\"";
        case Link::Authorizing: return "Headset on USB: authorizing...";
        case Link::Offline: return "Headset on USB: offline (replug the cable)";
        case Link::Ambiguous: return std::to_string(ambiguousCount) + " adb devices, none is a Quest (use --adb-serial)";
        case Link::Connected: {
            std::string s = model + " on USB (" + serial + ")";
            if (otherDevices > 0) s += formatString(", %d other adb device%s", otherDevices, otherDevices == 1 ? "" : "s");
            return s;
        }
    }
    return {};
}

std::string UsbStatus::appText() const {
    if (!connected()) return {};
    std::string installed = app == App::Unknown ? "checking..." : app == App::NotInstalled ? "not installed" : appVersion + " installed";
    std::string s = "Headset app: " + installed;
    if (!bundledVersion.empty()) s += " · " + bundledVersion + " next to VRScreen";
    return s;
}

std::string UsbStatus::travelText() const {
    switch (link) {
        case Link::Off: return "Travel mode: unknown (USB off)";
        case Link::AdbMissing: return "Travel mode: unknown (adb not found)";
        case Link::Connected: break;
        default: return "Travel mode: unknown (headset not on USB)";
    }
    switch (travel) {
        case Travel::On: return "Travel mode: On";
        case Travel::Off: return "Travel mode: Off";
        case Travel::FormatChanged: return "Travel mode: unavailable (dumpsys format changed)";
        default: return "Travel mode: checking...";
    }
}

std::string AdbResult::describe() const {
    if (!launched) return "cannot run adb: " + output;
    if (timedOut) return "timed out";
    return formatString("exit %d: %s", exitCode, trim(output).c_str());
}

// MARK: Adb

std::optional<Adb> Adb::locate() {
    std::vector<std::wstring> candidates;
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) candidates.emplace_back(found);
    const auto env = [](const wchar_t* name) {
        wchar_t buffer[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(name, buffer, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::wstring(buffer, n) : std::wstring();
    };
    for (const wchar_t* key : {L"ANDROID_HOME", L"ANDROID_SDK_ROOT"}) {
        const std::wstring sdk = env(key);
        if (!sdk.empty()) candidates.push_back(sdk + L"\\platform-tools\\adb.exe");
    }
    const std::wstring local = env(L"LOCALAPPDATA"), profile = env(L"USERPROFILE");
    candidates.push_back(exeDirectory() + L"\\platform-tools\\adb.exe");
    candidates.push_back(local + L"\\Android\\Sdk\\platform-tools\\adb.exe");
    candidates.push_back(local + L"\\Microsoft\\WinGet\\Links\\adb.exe");
    candidates.push_back(L"C:\\platform-tools\\adb.exe");
    candidates.push_back(L"C:\\ProgramData\\chocolatey\\bin\\adb.exe");
    candidates.push_back(profile + L"\\scoop\\shims\\adb.exe");
    // winget's package folder (Google.PlatformTools_...).
    WIN32_FIND_DATAW fd;
    const std::wstring pattern = local + L"\\Microsoft\\WinGet\\Packages\\Google.PlatformTools*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            candidates.push_back(local + L"\\Microsoft\\WinGet\\Packages\\" + fd.cFileName + L"\\platform-tools\\adb.exe");
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (const auto& c : candidates) {
        const DWORD attr = GetFileAttributesW(c.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) return Adb(c);
    }
    return std::nullopt;
}

static std::wstring quoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) return arg;
    // CommandLineToArgvW rules: backslashes are literal unless they precede a quote.
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
        } else {
            out.append(backslashes, L'\\');
        }
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, L'\\');
    return out + L"\"";
}

AdbResult Adb::run(const std::vector<std::string>& args, int timeoutMs) const {
    AdbResult result;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 64 * 1024)) {
        result.launched = false;
        result.output = win32ErrorText(GetLastError());
        return result;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    std::wstring cmd = quoteArg(path_);
    for (const auto& a : args) cmd += L" " + quoteArg(widen(a));
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = writePipe;
    si.StartupInfo.hStdError = writePipe;
    // Inherit only these handles.
    HANDLE inherit[2] = {writePipe, nul};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuffer(attrSize);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);
    PROCESS_INFORMATION pi{};
    const BOOL created = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                        &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(writePipe);
    CloseHandle(nul);
    if (!created) {
        CloseHandle(readPipe);
        result.launched = false;
        result.output = win32ErrorText(GetLastError());
        return result;
    }
    // Read without blocking: `start-server` leaves a daemon that may keep the pipe open, so
    // completion is the client's exit, not end of file.
    const auto drain = [&] {
        DWORD available = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            char buffer[4096];
            DWORD n = 0;
            if (!ReadFile(readPipe, buffer, std::min<DWORD>(available, sizeof buffer), &n, nullptr) || n == 0) break;
            result.output.append(buffer, n);
        }
    };
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(timeoutMs);
    for (;;) {
        drain();
        const DWORD r = WaitForSingleObject(pi.hProcess, 10);
        if (r == WAIT_OBJECT_0) break;
        if (GetTickCount64() >= deadline) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            result.timedOut = true;
            break;
        }
    }
    drain();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = int(code);
    result.ok = !result.timedOut && code == 0;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readPipe);
    return result;
}

bool Adb::serverRunning() {
    wchar_t buffer[16];
    int port = 5037;
    const DWORD n = GetEnvironmentVariableW(L"ANDROID_ADB_SERVER_PORT", buffer, 16);
    if (n > 0 && n < 16) port = _wtoi(buffer);
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    const bool ok = connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    closesocket(s);
    return ok;
}

std::string metaUsbDeviceName() {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::string name;
    SP_DEVINFO_DATA info{sizeof info};
    for (DWORD i = 0; name.empty() && SetupDiEnumDeviceInfo(set, i, &info); ++i) {
        wchar_t id[512];
        if (!SetupDiGetDeviceInstanceIdW(set, &info, id, 512, nullptr)) continue;
        std::wstring upper = id;
        for (auto& c : upper) c = wchar_t(towupper(c));
        // The composite parent only (not each interface "&MI_xx").
        if (upper.find(L"VID_2833") == std::wstring::npos || upper.find(L"&MI_") != std::wstring::npos) continue;
        wchar_t desc[256] = {};
        if (SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(desc), sizeof desc, nullptr) ||
            SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<BYTE*>(desc), sizeof desc, nullptr)) {
            name = narrow(desc);
        }
        if (name.empty() || containsIgnoreCase(name, "composite")) name = "Meta Quest";
    }
    SetupDiDestroyDeviceInfoList(set);
    return name;
}

// MARK: UsbHeadsetLink

UsbHeadsetLink::UsbHeadsetLink(uint16_t port, std::optional<std::string> preferredSerial, std::wstring bundledApk,
                               bool launchOnAttach, std::function<void(const UsbStatus&)> onStatus)
    : port_(port), preferredSerial_(std::move(preferredSerial)), bundledApk_(std::move(bundledApk)),
      launchOnAttach_(launchOnAttach), onStatus_(std::move(onStatus)) {
    if (!bundledApk_.empty()) status_.bundledVersion = VRSCREEN_VERSION;
}

UsbHeadsetLink::~UsbHeadsetLink() { stop(); }

void UsbHeadsetLink::start() {
    thread_ = std::thread([this] { run(); });
}

void UsbHeadsetLink::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void UsbHeadsetLink::post(std::function<void()> task) {
    {
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    cv_.notify_all();
}

void UsbHeadsetLink::setLaunchOnAttach(bool on) { launchOnAttach_ = on; }

void UsbHeadsetLink::run() {
    adb_ = Adb::locate();
    log(adb_ ? "usb: using " + narrow(adb_->path()) + " (ADB_MDNS=0)"
             : "usb: adb not found (winget install Google.PlatformTools); USB disabled, Wi-Fi works");
    unsigned long long nextPoll = 0;
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            const auto now = GetTickCount64();
            cv_.wait_for(lock, std::chrono::milliseconds(nextPoll > now ? nextPoll - now : 0),
                         [&] { return stopping_ || !tasks_.empty(); });
            if (stopping_) break;
            if (!tasks_.empty()) {
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
        }
        if (task) {
            task();
            continue;
        }
        if (GetTickCount64() >= nextPoll) {
            poll();
            nextPoll = GetTickCount64() + 3000;
        }
    }
    removeCreatedReverse();
}

void UsbHeadsetLink::publish() {
    if (published_ && *published_ == status_) return;
    published_ = status_;
    if (onStatus_) onStatus_(status_);
}

AdbResult UsbHeadsetLink::headset(const std::vector<std::string>& args, int timeoutMs) {
    std::vector<std::string> full{"-s", attachedSerial_};
    full.insert(full.end(), args.begin(), args.end());
    return adb_->run(full, timeoutMs);
}

std::optional<std::string> UsbHeadsetLink::startServer() {
    if (adb_->run({"start-server"}, 10000).ok && Adb::serverRunning()) return std::nullopt;
    adb_->run({"kill-server"}, 5000);
    const AdbResult r = adb_->run({"start-server"}, 10000);
    if (r.ok) return Adb::serverRunning() ? std::nullopt : std::optional<std::string>("the server exits right after starting");
    return r.describe();
}

void UsbHeadsetLink::poll() {
    if (!adb_) {
        adb_ = Adb::locate();
        if (adb_) log("usb: found " + narrow(adb_->path()));
    }
    if (!adb_) {
        status_.link = UsbStatus::Link::AdbMissing;
        status_.metaUsbDevice = metaUsbDeviceName();
        return publish();
    }
    const auto now = GetTickCount64();
    if (!Adb::serverRunning()) {
        if (serverRetryAt_ && now < serverRetryAt_) return;
        if (serverError_.empty()) {
            if (serverSeenRunning_) {
                serverRestarts_.push_back(now);
                log("usb: the adb server stopped; starting it again (ADB_MDNS=0)");
            } else {
                log("usb: starting the adb server (ADB_MDNS=0)");
            }
        }
        if (auto error = startServer()) {
            if (*error != serverError_) log("usb: adb server does not start: " + *error + "; retrying every 30 s");
            serverError_ = *error;
            serverRetryAt_ = GetTickCount64() + 30000;
            detach();
            status_.link = UsbStatus::Link::AdbBroken;
            status_.adbError = *error;
            status_.metaUsbDevice = metaUsbDeviceName();
            return publish();
        }
        if (!serverError_.empty()) log("usb: adb server running again");
    }
    serverError_.clear();
    serverRetryAt_ = 0;
    serverSeenRunning_ = true;
    while (!serverRestarts_.empty() && now - serverRestarts_.front() > 120000) serverRestarts_.pop_front();
    status_.serverUnstable = serverRestarts_.size() >= 3;

    const AdbResult devicesResult = adb_->run({"devices", "-l"});
    if (!devicesResult.ok) {
        detach();
        status_.link = UsbStatus::Link::AdbBroken;
        status_.adbError = "adb devices: " + devicesResult.describe();
        return publish();
    }
    const auto selection = adb::select(adb::parseDevices(devicesResult.output), preferredSerial_);
    std::string text;
    using K = adb::Selection::Kind;
    switch (selection.kind) {
        case K::None: text = "no headset"; break;
        case K::Ready:
            text = selection.device.displayName() + " " + selection.device.serial + " ready" +
                   (selection.others > 0 ? formatString(" (%d other device(s) ignored)", selection.others) : "");
            break;
        case K::Unauthorized: text = selection.device.serial + " unauthorized: accept \"Allow USB debugging\" in the headset"; break;
        case K::Authorizing: text = selection.device.serial + " authorizing"; break;
        case K::Offline: text = selection.device.serial + " offline / no permissions: replug the cable"; break;
        case K::Ambiguous: text = formatString("%d devices and none is a Quest; pick one with --adb-serial", selection.count); break;
    }
    if (text != lastLoggedSelection_) {
        lastLoggedSelection_ = text;
        log("usb: " + text);
    }
    status_.metaUsbDevice.clear();
    switch (selection.kind) {
        case K::Ready:
            if (attachedSerial_ != selection.device.serial) {
                detach();
                attach(selection.device);
            } else {
                ensureReverse();
                if (GetTickCount64() >= nextTravelRead_ || GetTickCount64() < travelFastUntil_) readTravel();
            }
            status_.link = UsbStatus::Link::Connected;
            status_.serial = selection.device.serial;
            status_.model = selection.device.displayName();
            status_.otherDevices = selection.others;
            break;
        case K::Unauthorized:
            detach();
            status_.link = UsbStatus::Link::Unauthorized;
            break;
        case K::Authorizing:
            detach();
            status_.link = UsbStatus::Link::Authorizing;
            break;
        case K::Offline:
            detach();
            status_.link = UsbStatus::Link::Offline;
            break;
        case K::Ambiguous:
            detach();
            status_.link = UsbStatus::Link::Ambiguous;
            status_.ambiguousCount = selection.count;
            break;
        case K::None:
            detach();
            status_.link = UsbStatus::Link::NoHeadset;
            status_.metaUsbDevice = metaUsbDeviceName();
            break;
    }
    publish();
}

void UsbHeadsetLink::attach(const adb::Device& device) {
    attachedSerial_ = device.serial;
    loggedExistingReverse_ = false;
    status_.installResult.clear();
    if (!createdReverseSerial_.empty() && createdReverseSerial_ != device.serial) createdReverseSerial_.clear();
    ensureReverse();
    readApp();
    if (status_.app == UsbStatus::App::Installed) log("usb: headset app " + status_.appVersion + " installed");
    else if (status_.app == UsbStatus::App::NotInstalled) log("usb: headset app not installed");
    readTravel();
    if (launchOnAttach_) launch("headset connected");
}

void UsbHeadsetLink::detach() {
    if (attachedSerial_.empty()) return;
    log("usb: headset " + attachedSerial_ + " left");
    attachedSerial_.clear();
    status_.app = UsbStatus::App::Unknown;
    status_.appVersion.clear();
    status_.installResult.clear();
    status_.travel = UsbStatus::Travel::Unknown;
}

void UsbHeadsetLink::ensureReverse() {
    const AdbResult list = headset({"reverse", "--list"});
    if (!list.ok) {
        log("usb: adb reverse --list failed: " + list.describe());
        return;
    }
    const std::string spec = "tcp:" + std::to_string(port_);
    std::string existing;
    switch (adb::decideReverse(adb::parseReverseList(list.output), port_, &existing)) {
        case adb::ReverseDecision::Present:
            if (createdReverseSerial_ != attachedSerial_ && !loggedExistingReverse_) {
                loggedExistingReverse_ = true;
                log("usb: adb reverse " + spec + " " + spec + " already set up (left in place on exit)");
            }
            return;
        case adb::ReverseDecision::Replace: log("usb: replacing adb reverse " + spec + " -> " + existing); break;
        case adb::ReverseDecision::Create: break;
    }
    const AdbResult r = headset({"reverse", spec, spec});
    if (r.ok) {
        createdReverseSerial_ = attachedSerial_;
        log("usb: adb reverse " + spec + " " + spec + ": the headset reaches this PC at 127.0.0.1:" + std::to_string(port_));
    } else {
        log("usb: adb reverse " + spec + " failed: " + r.describe());
    }
}

void UsbHeadsetLink::readApp() {
    const AdbResult r = headset({"shell", std::string("dumpsys package ") + kPackage + " | grep versionName"});
    if (r.ok) {
        const std::string v = adb::parseVersionName(r.output);
        status_.app = v.empty() ? UsbStatus::App::NotInstalled : UsbStatus::App::Installed;
        status_.appVersion = v;
    } else if (!r.timedOut && r.exitCode == 1 && trim(r.output).empty()) {
        status_.app = UsbStatus::App::NotInstalled;
    } else {
        status_.app = UsbStatus::App::Unknown;
    }
}

void UsbHeadsetLink::readTravel() {
    nextTravelRead_ = GetTickCount64() + 15000;
    const AdbResult r = headset({"shell", "dumpsys tracking | grep -F 'Travel Mode:'"});
    if (r.ok) {
        switch (adb::parseTravelMode(r.output)) {
            case adb::TravelMode::On: status_.travel = UsbStatus::Travel::On; break;
            case adb::TravelMode::Off: status_.travel = UsbStatus::Travel::Off; break;
            default: status_.travel = UsbStatus::Travel::FormatChanged; break;
        }
    } else if (!r.timedOut && r.exitCode == 1 && trim(r.output).empty()) {
        status_.travel = UsbStatus::Travel::FormatChanged;
    }
}

void UsbHeadsetLink::launch(const std::string& reason) {
    if (status_.app == UsbStatus::App::NotInstalled) {
        log("usb: not launching the headset app (" + reason + "): not installed");
        return;
    }
    // The app reads a non-default port from this property (empty = 7777).
    if (port_ != 7777) {
        headset({"shell", "setprop", "debug.vrscreen.port", std::to_string(port_)});
    } else {
        const AdbResult value = headset({"shell", "getprop", "debug.vrscreen.port"});
        if (value.ok && !trim(value.output).empty()) headset({"shell", "setprop", "debug.vrscreen.port", "''"});
    }
    const AdbResult r = headset({"shell", "am", "start", "-n", std::string(kPackage) + "/android.app.NativeActivity"});
    if (r.ok) log("usb: launched the headset app (" + reason + ")");
    else log("usb: launching the headset app failed: " + r.describe());
}

void UsbHeadsetLink::installApp() {
    post([this] {
        if (attachedSerial_.empty() || bundledApk_.empty() || status_.installing) return;
        status_.installing = true;
        status_.installResult.clear();
        publish();
        log("usb: installing " + narrow(bundledApk_) + " on the headset");
        const AdbResult r = headset({"install", "-r", narrow(bundledApk_)}, 300000);
        std::optional<std::string> failure = r.timedOut || !r.launched ? std::optional(r.describe()) : adb::installFailure(r.output);
        status_.installing = false;
        readApp();
        if (failure) {
            status_.installResult = "Install failed: " + *failure;
            if (failure->find("UPDATE_INCOMPATIBLE") != std::string::npos)
                status_.installResult += " (signed by another build: uninstall VRScreen in the headset first)";
            log("usb: install failed: " + *failure);
        } else {
            status_.installResult = "Installed " + status_.appVersion;
            log("usb: headset app installed (" + status_.appVersion + ")");
            if (launchOnAttach_) launch("installed");
        }
        publish();
    });
}

void UsbHeadsetLink::launchApp() {
    post([this] {
        if (!attachedSerial_.empty()) launch("menu");
    });
}

void UsbHeadsetLink::restartAdb() {
    post([this] {
        if (!adb_) return;
        log("usb: restarting the adb server (menu)");
        adb_->run({"kill-server"}, 5000);
        serverSeenRunning_ = false;
        serverRestarts_.clear();
        serverError_.clear();
        serverRetryAt_ = 0;
        poll();
    });
}

void UsbHeadsetLink::openQuickSettings() {
    post([this] {
        if (attachedSerial_.empty()) return;
        const AdbResult r = headset({"shell", "am", "start", "-a", "com.oculus.action.QUICK_SETTINGS"});
        log(r.ok ? "usb: opened Quick Settings in the headset (tap Travel mode there)"
                 : "usb: opening Quick Settings failed: " + r.describe());
        // Travel mode is toggled in the headset: read it on every poll for a while.
        travelFastUntil_ = GetTickCount64() + 12000;
    });
}

void UsbHeadsetLink::refreshTravelMode() {
    post([this] {
        if (attachedSerial_.empty()) return;
        readTravel();
        publish();
    });
}

void UsbHeadsetLink::removeCreatedReverse() {
    if (createdReverseSerial_.empty() || !adb_) return;
    const std::string spec = "tcp:" + std::to_string(port_);
    const AdbResult r = adb_->run({"-s", createdReverseSerial_, "reverse", "--remove", spec});
    log(r.ok ? "usb: removed adb reverse " + spec : "usb: could not remove adb reverse " + spec + ": " + r.describe());
    createdReverseSerial_.clear();
}

}  // namespace vr
