#include "app.h"

#include <shellapi.h>

#include <algorithm>

#include "log.h"
#include "util.h"

namespace vr {
namespace {
constexpr UINT kUiTaskMessage = WM_APP + 1;
constexpr UINT_PTR kSaveTimer = 1;
constexpr wchar_t kWindowClass[] = L"VRScreenSenderWindow";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"VRScreen";
constexpr const char* kAdbInstallCommand = "winget install Google.PlatformTools";
constexpr wchar_t kPlatformToolsUrl[] = L"https://developer.android.com/tools/releases/platform-tools";
constexpr wchar_t kQuestDriverUrl[] = L"https://developers.meta.com/horizon/downloads/package/oculus-adb-drivers/";
constexpr wchar_t kDeveloperModeUrl[] = L"https://developers.meta.com/horizon/documentation/native/android/mobile-device-setup/";

std::string percentLabel(int percent) { return percent == 0 ? "Off" : std::to_string(percent) + " %"; }
}  // namespace

App::App(Options options)
    : options_(std::move(options)),
      model_(options_.settingsPath ? *options_.settingsPath : roamingDataDirectory() + L"\\settings.json", options_.overrides),
      displayIndexOverride_(options_.displayIndex) {
    applied_ = model_.effective();
    const std::wstring apk = exeDirectory() + L"\\vrscreen-headset.apk";
    if (GetFileAttributesW(apk.c_str()) != INVALID_FILE_ATTRIBUTES) bundledApk_ = apk;
}

App::~App() = default;

CaptureSettings App::captureSettings(const Settings& s) const {
    CaptureSettings c;
    c.display = s.display;
    c.displayIndex = displayIndexOverride_;
    c.maxWidth = s.maxWidth;
    c.fps = s.fps;
    return c;
}

EncoderSettings App::encoderSettings(const Settings& s) const {
    EncoderSettings e;
    e.codec = s.codec;
    e.bitrateMbps = s.bitrateMbps;
    e.fps = s.fps;
    e.keyframeIntervalSeconds = options_.keyframeIntervalSeconds;
    e.preferredName = options_.encoderName;
    return e;
}

void App::ui(std::function<void()> task) {
    {
        std::lock_guard lock(uiMutex_);
        uiTasks_.push_back(std::move(task));
    }
    PostMessageW(hwnd_, kUiTaskMessage, 0, 0);
}

void App::requestQuit() { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }

// MARK: Startup

int App::run() {
    log("settings: " + applied_.summary() + " (" + narrow(model_.path()) + ")");

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, kWindowClass, L"VRScreen", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    server_ = std::make_unique<MirrorServer>(loop_, options_.port, uint32_t(options_.maxInFlight));
    server_->controlOnConnect = options_.controlOnConnect;
    server_->updateHeadsetSettings(applied_);
    pipeline_ = std::make_unique<SenderPipeline>(loop_, *server_, encoderSettings(applied_));
    audioSink_ = std::make_unique<AudioSink>(loop_, *server_);
    if (options_.discoveryPort) discovery_ = std::make_unique<DiscoveryResponder>(loop_, *options_.discoveryPort, options_.port);

    pipeline_->onEncoderError = [this](const std::string* error) {
        const std::string text = error ? *error : "";
        ui([this, text] {
            health_.encoderError = text;
            updateTray();
        });
    };
    server_->onActiveClientChanged = [this](const std::string* address) {
        lastHeadsetStatus_.reset();
        clientConnected_ = address != nullptr;
        const std::string text = address ? *address : "";
        ui([this, text] {
            health_.clientAddress = text;
            health_.battery.reset();
            applyAudio();
            updateTray();
        });
    };
    server_->onHeadsetStatus = [this](const proto::HeadsetStatus& status) {
        if (lastHeadsetStatus_ != status) log("headset battery " + status.describe());
        lastHeadsetStatus_ = status;
        ui([this, status] {
            health_.battery = status;
            updateTray();
        });
    };

    if (options_.menu) {
        icon_ = makeTrayIcon(false, false);
        tray_.add(hwnd_, icon_, "VRScreen");
    }
    if (applied_.hotkeys) {
        hotkeys_ = std::make_unique<HotkeyController>([this](const HotkeyBinding& b, bool repeat) {
            if (!repeat) log(std::string("hotkey ") + b.label + " -> " + b.command.describe());
            sendControl(b.command, repeat ? "" : b.label);
        });
        hotkeys_->registerAll(hwnd_);
    }
    if (applied_.mouseGestures) {
        mouse_ = std::make_unique<MouseControl>(applied_.mouseSensitivity, applied_.scrollSensitivity,
                                                [this](std::vector<proto::ControlCommand> commands) {
                                                    loop_.post([this, commands = std::move(commands)] {
                                                        for (const auto& c : commands) server_->sendControl(c);
                                                    });
                                                });
        mouse_->start();
    }
    if (options_.adb) {
        usb_ = std::make_unique<UsbHeadsetLink>(options_.port, options_.adbSerial, bundledApk_, applied_.launchHeadsetApp,
                                                [this](const UsbStatus& status) {
                                                    ui([this, status] {
                                                        health_.usb = status;
                                                        updateTray();
                                                    });
                                                });
    } else {
        health_.usb.link = UsbStatus::Link::Off;
    }

    loop_.start();
    loop_.post([this] { startNetwork(); });
    // Once-per-second stats (loop thread), rescheduling itself.
    struct StatsTicker {
        static void schedule(App* app) {
            app->loop_.postDelayed(std::chrono::seconds(1), [app] {
                std::string audio = app->audioSink_->enabled() ? "" : "audio off";
                if (app->audioSink_->enabled()) {
                    static long long lastQueued = 0, lastDropped = 0;
                    const long long q = app->server_->audioChunksQueued(), d = app->server_->audioChunksDropped();
                    audio = formatString("audio %lld chunks/s, %lld dropped", q - lastQueued, d - lastDropped);
                    lastQueued = q;
                    lastDropped = d;
                    const std::string delivery = app->audioSink_->takeStats();
                    if (!delivery.empty()) audio += ", " + delivery;
                }
                app->pipeline_->logStats(audio);
                schedule(app);
            });
        }
    };
    StatsTicker::schedule(this);
    updateTray();
    if (options_.menu && GetFileAttributesW(model_.path().c_str()) == INVALID_FILE_ATTRIBUTES) {
        tray_.balloon("VRScreen is running", "Start VRScreen in the headset. Settings are in this tray icon's menu.");
        model_.change([](Settings&) {});  // write the defaults once, so this tip shows only on the first run
        SetTimer(hwnd_, kSaveTimer, 500, nullptr);
    }

    watchSettingsFile();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    shutdown();
    return 0;
}

void App::startNetwork() {
    int error = 0;
    if (!server_->listening()) {
        if (!server_->start(error)) {
            if (error == WSAEADDRINUSE && options_.menu) {
                static bool reported = false;
                if (!reported) {
                    logf("server: TCP port %u is in use (another VRScreen running?); retrying every 3 s", options_.port);
                    reported = true;
                }
                ui([this] {
                    health_.portInUse = options_.port;
                    updateTray();
                });
                loop_.postDelayed(std::chrono::seconds(3), [this] { startNetwork(); });
                return;
            }
            logf("server: cannot listen on TCP port %u: %s", options_.port, win32ErrorText(DWORD(error)).c_str());
            ui([this] {
                if (!options_.menu) {
                    MessageBoxW(nullptr, L"VRScreen cannot listen on its TCP port (see the log).", L"VRScreen", MB_ICONERROR);
                }
                PostQuitMessage(1);
            });
            return;
        }
        ui([this] {
            if (health_.portInUse) {
                health_.portInUse.reset();
                updateTray();
            }
            if (usb_) usb_->start();
            checkFirewallAsync();
        });
        startSources();
    }
    if (discovery_ && !discovery_->running()) {
        if (!discovery_->start(error)) {
            logf("discovery: cannot use UDP port %u: %s; retrying every 3 s", *options_.discoveryPort,
                 win32ErrorText(DWORD(error)).c_str());
            loop_.postDelayed(std::chrono::seconds(3), [this] { startNetwork(); });
        }
    }
}

void App::startSources() {
    if (sourcesStarted_) return;
    sourcesStarted_ = true;
    const Settings s = applied_;  // read once at startup; later changes go through apply()
    if (options_.source == Options::Source::Test) {
        source_ = std::make_unique<TestPattern>(options_.testWidth, options_.testHeight, captureSettings(s));
        source_->onAudio = [this](const float* samples, int frames, uint64_t micros) {
            audioSink_->push(samples, frames, 48000, micros);
        };
    } else {
        source_ = std::make_unique<ScreenCapture>(captureSettings(s));
    }
    source_->onFrame = [this](VideoFrame frame) {
        loop_.post([this, frame = std::move(frame)]() mutable { pipeline_->handleCapture(std::move(frame)); });
    };
    source_->onStatus = [this](const CaptureStatus& status) {
        if (status.kind == CaptureStatus::Kind::Failed) log("capture: failed: " + status.message);
        ui([this, status] {
            health_.captureError = status.kind == CaptureStatus::Kind::Failed ? status.message : "";
            updateTray();
        });
    };
    source_->start();
    ui([this] { applyAudio(); });
}

// MARK: Settings

void App::changeSettings(const std::function<void(Settings&)>& change, std::initializer_list<SettingKey> keys) {
    model_.change(change, keys);
    SetTimer(hwnd_, kSaveTimer, 500, nullptr);
    apply(model_.effective());
}

void App::apply(const Settings& next) {
    const Settings old = applied_;
    applied_ = next;
    if (old.curvature != next.curvature || old.dim != next.dim || old.handGestures != next.handGestures) {
        loop_.post([this, next] { server_->updateHeadsetSettings(next); });
    }
    if (old.audio != next.audio || old.mutePC != next.mutePC) applyAudio();
    if (source_ && !(captureSettings(old) == captureSettings(next))) {
        const bool restart = old.display != next.display || old.maxWidth != next.maxWidth;
        source_->update(captureSettings(next));
        // A picture of the old display or size must not be re-encoded for a new client.
        if (restart) loop_.post([this] { pipeline_->discardLatestCapture(); });
    }
    if (!(encoderSettings(old) == encoderSettings(next))) {
        const EncoderSettings e = encoderSettings(next);
        log("video: " + next.videoSummary());
        loop_.post([this, e] { pipeline_->replaceEncoder(e); });
    }
    if (old.hotkeys != next.hotkeys) {
        if (next.hotkeys) {
            if (!hotkeys_)
                hotkeys_ = std::make_unique<HotkeyController>([this](const HotkeyBinding& b, bool repeat) {
                    if (!repeat) log(std::string("hotkey ") + b.label + " -> " + b.command.describe());
                    sendControl(b.command, repeat ? "" : b.label);
                });
            hotkeys_->registerAll(hwnd_);
        } else if (hotkeys_) {
            hotkeys_->unregisterAll();
            log("hotkeys: off");
        }
    }
    if (old.mouseGestures != next.mouseGestures) {
        if (next.mouseGestures) {
            mouse_ = std::make_unique<MouseControl>(next.mouseSensitivity, next.scrollSensitivity,
                                                    [this](std::vector<proto::ControlCommand> commands) {
                                                        loop_.post([this, commands = std::move(commands)] {
                                                            for (const auto& c : commands) server_->sendControl(c);
                                                        });
                                                    });
            mouse_->start();
        } else {
            mouse_.reset();
            log("mouse: off");
        }
    } else if (mouse_ && (old.mouseSensitivity != next.mouseSensitivity || old.scrollSensitivity != next.scrollSensitivity)) {
        mouse_->setSensitivity(next.mouseSensitivity, next.scrollSensitivity);
    }
    if (usb_ && old.launchHeadsetApp != next.launchHeadsetApp) usb_->setLaunchOnAttach(next.launchHeadsetApp);
    updateTray();
}

void App::applyAudio() {
    const bool on = options_.audioAllowed && applied_.audio;
    const bool wasOn = audioSink_->enabled();
    loop_.post([this, on] { audioSink_->setEnabled(on); });
    if (on != wasOn) log(on ? "audio: on (PC sound to the headset, 10 ms s16le chunks)" : "audio: off");
    if (options_.source == Options::Source::Screen && sourcesStarted_) {
        if (on && !loopback_) {
            loopback_ = std::make_unique<LoopbackCapture>(*audioSink_, options_.audioOffsetMs);
            loopback_->onStatus = [this](const std::string& error) {
                ui([this, error] {
                    health_.audioError = error;
                    updateTray();
                });
            };
            loopback_->start();
        } else if (!on && loopback_) {
            loopback_.reset();
            health_.audioError.clear();
        }
    }
    if (loopback_) loopback_->setMute(on && applied_.mutePC && !health_.clientAddress.empty());
}

void App::sendControl(const proto::ControlCommand& command, const std::string& origin) {
    loop_.post([this, command, origin] {
        const auto address = server_->sendControl(command);
        if (!origin.empty() && !address) log(origin + ": no headset connected; " + command.describe() + " dropped");
    });
}

// A hand edit of settings.json applies at once (the Mac sender's `kill -USR1`). Our own saves
// reload to the same values, which changes nothing.
void App::watchSettingsFile() {
    const std::wstring path = model_.path();
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    const std::wstring dir = path.substr(0, slash);
    createDirectories(dir);
    settingsWatcher_ = std::thread([this, dir, path] {
        HANDLE change = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
        if (change == INVALID_HANDLE_VALUE) return;
        FILETIME last{};
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) last = data.ftLastWriteTime;
        while (!stopWatching_) {
            if (WaitForSingleObject(change, 250) != WAIT_OBJECT_0) continue;
            FindNextChangeNotification(change);
            Sleep(100);  // let the writer finish
            if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
                CompareFileTime(&data.ftLastWriteTime, &last) == 0)
                continue;
            last = data.ftLastWriteTime;
            ui([this] {
                const Settings before = model_.effective();
                model_.reload();
                if (!(model_.effective() == before)) {
                    log("settings: reloaded " + narrow(model_.path()) + ": " + model_.effective().summary());
                    apply(model_.effective());
                }
            });
        }
        FindCloseChangeNotification(change);
    });
}

// MARK: Window

LRESULT CALLBACK App::windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app && app->hwnd_ == hwnd) return app->handleMessage(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT App::handleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kUiTaskMessage) {
        std::deque<std::function<void()>> tasks;
        {
            std::lock_guard lock(uiMutex_);
            tasks.swap(uiTasks_);
        }
        for (auto& t : tasks) t();
        return 0;
    }
    if (message == taskbarCreated_ && taskbarCreated_) {
        tray_.readd();
        return 0;
    }
    switch (message) {
        case TrayIcon::kCallbackMessage:
            switch (LOWORD(lparam)) {
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                case NIN_SELECT:
                case NIN_KEYSELECT:
                case WM_LBUTTONUP:
                    showMenu();
                    break;
                default: break;
            }
            return 0;
        case WM_HOTKEY:
            if (hotkeys_ && hotkeys_->onHotkey(wparam)) return 0;
            break;
        case WM_TIMER:
            if (wparam == kSaveTimer) {
                KillTimer(hwnd_, kSaveTimer);
                model_.saveIfDirty();
                return 0;
            }
            if (hotkeys_ && hotkeys_->onTimer(wparam)) return 0;
            break;
        case WM_DISPLAYCHANGE:
            updateTray();
            break;
        case WM_SETTINGCHANGE:
            // Light/dark taskbar.
            updateTray();
            break;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wparam) {
                log("windows is ending the session; quitting");
                shutdown();
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

// MARK: Problems and tray

std::vector<App::Problem> App::problems() {
    std::vector<Problem> list;
    const auto add = [&](bool error, std::string title, std::string detail,
                         std::vector<std::pair<std::string, std::function<void()>>> actions = {}) {
        list.push_back({error, std::move(title), std::move(detail), std::move(actions)});
    };
    const auto showLogs = std::make_pair(std::string("Show Logs..."), std::function<void()>([this] {
        ShellExecuteW(nullptr, L"open", logFilePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }));
    if (health_.portInUse)
        add(true, formatString("Port %u in use (another VRScreen running?)", *health_.portInUse),
            "VRScreen starts as soon as the port is free.", {showLogs});
    if (!health_.captureError.empty()) add(true, "Screen capture failed", health_.captureError, {showLogs});
    if (!health_.encoderError.empty()) add(true, "Video encoder error", health_.encoderError, {showLogs});
    if (health_.firewall.blocked) {
        const bool pub = health_.firewall.publicNetwork;
        std::vector<std::pair<std::string, std::function<void()>>> actions;
        actions.emplace_back(pub ? "Allow on this network (administrator)..." : "Allow through the firewall (administrator)...",
                             [this, pub] {
                                 const std::wstring exe = exePath();
                                 std::thread([this, exe, pub] {
                                     allowThroughFirewall(exe, pub);
                                     ui([this] { checkFirewallAsync(); });
                                 }).detach();
                             });
        if (pub)
            actions.emplace_back("Open network settings (make this network Private)...",
                                 [this] { openUrl(L"ms-settings:network-status"); });
        add(false, "Wi-Fi blocked by Windows Firewall",
            health_.firewall.detail + " USB works; for Wi-Fi allow VRScreen (local network only).", actions);
    }
    using L = UsbStatus::Link;
    const UsbStatus& usb = health_.usb;
    if (usb.link == L::AdbMissing && !usb.metaUsbDevice.empty())
        add(false, "USB needs Android platform-tools", usb.metaUsbDevice + " is plugged in, but adb is not installed.",
            {{"Copy install command (" + std::string(kAdbInstallCommand) + ")", [this] { copyToClipboard(kAdbInstallCommand); }},
             {"Download platform-tools...", [this] { openUrl(kPlatformToolsUrl); }}});
    if (usb.link == L::AdbBroken)
        add(true, "adb isn't working", usb.adbError, {{"Restart adb", [this] { if (usb_) usb_->restartAdb(); }}, showLogs});
    else if (usb.serverUnstable)
        add(false, "adb keeps stopping", "The adb server stopped 3 times in 2 minutes.",
            {{"Restart adb", [this] { if (usb_) usb_->restartAdb(); }}, showLogs});
    if (usb.link == L::NoHeadset && !usb.metaUsbDevice.empty())
        add(false, usb.metaUsbDevice + " on USB, but adb can't see it",
            "Turn on Developer Mode (Meta Horizon phone app) and USB debugging, or install the Meta Quest ADB driver.",
            {{"Download the Meta Quest ADB driver...", [this] { openUrl(kQuestDriverUrl); }},
             {"How to turn on Developer Mode...", [this] { openUrl(kDeveloperModeUrl); }}});
    if (usb.link == L::Unauthorized) add(false, "Put on the headset and accept \"Allow USB debugging\"", "Tick \"Always allow from this computer\".");
    if (usb.link == L::Offline) add(false, "Headset offline on USB", "Unplug and replug the cable.");
    if (usb.link == L::Ambiguous) add(false, usb.linkText(), "Start VRScreen with --adb-serial SERIAL.");
    if (usb.connected() && !usb.bundledVersion.empty() && !usb.installing) {
        if (usb.app == UsbStatus::App::NotInstalled)
            add(false, "Headset app not installed", "Install it over USB.", {{"Install headset app (" + usb.bundledVersion + ")", [this] { usb_->installApp(); }}});
        else if (usb.app == UsbStatus::App::Installed && adb::compareVersions(usb.appVersion, usb.bundledVersion).value_or(0) < 0)
            add(false, "Headset app out of date", usb.appVersion + " installed, " + usb.bundledVersion + " available.",
                {{"Update headset app", [this] { usb_->installApp(); }}});
    }
    if (!health_.audioError.empty() && applied_.audio) add(false, "No sound for the headset", health_.audioError);
    if (health_.battery && health_.battery->known() && health_.battery->battery <= 15 && !health_.battery->charging() &&
        !health_.battery->plugged())
        add(false, formatString("Headset battery low: %u %%", health_.battery->battery), "Plug in the headset.");
    std::stable_sort(list.begin(), list.end(), [](const Problem& a, const Problem& b) { return a.error > b.error; });
    return list;
}

void App::updateTray() {
    if (!options_.menu || quitting_) return;
    const auto list = problems();
    static std::string lastLogged = "-";
    std::string summary;
    for (const auto& p : list) summary += (summary.empty() ? "" : "; ") + p.title;
    if (summary != lastLogged) {
        log("problems: " + (summary.empty() ? std::string("none") : summary));
        lastLogged = summary;
    }
    std::string tip = "VRScreen - ";
    if (health_.clientAddress.empty()) tip += "no headset connected";
    else tip += startsWith(health_.clientAddress, "127.0.0.1") ? "headset on USB" : "headset on Wi-Fi";
    if (!list.empty()) tip += " (" + list.front().title + ")";
    HICON next = makeTrayIcon(!list.empty(), !health_.clientAddress.empty());
    tray_.update(next, tip);
    if (icon_) DestroyIcon(icon_);
    icon_ = next;
}

void App::checkFirewallAsync() {
    if (firewallChecking_.exchange(true)) return;
    const std::wstring exe = exePath();
    std::thread([this, exe] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const FirewallState state = checkFirewall(exe);
        CoUninitialize();
        ui([this, state] {
            firewallChecking_ = false;
            const bool changed = state.blocked != health_.firewall.blocked;
            health_.firewall = state;
            if (changed) log(state.blocked ? "firewall: " + state.detail : "firewall: VRScreen is allowed on this network");
            updateTray();
        });
    }).detach();
}

void App::showMenu() {
    checkFirewallAsync();  // refreshed for next time
    if (usb_) usb_->refreshTravelMode();
    MenuBuilder m;
    const auto list = problems();
    for (const auto& p : list) {
        m.label((p.error ? "\xE2\x9B\x94 " : "\xE2\x9A\xA0 ") + p.title);  // ⛔ / ⚠
        // Details wrapped at ~64 characters so the menu stays narrow.
        std::string line;
        for (const auto& word : splitWhitespace(p.detail)) {
            if (!line.empty() && line.size() + 1 + word.size() > 64) {
                m.label("      " + line);
                line.clear();
            }
            line += (line.empty() ? "" : " ") + word;
        }
        if (!line.empty()) m.label("      " + line);
        for (const auto& [text, action] : p.actions) m.item("      " + text, action);
    }
    if (!list.empty()) m.separator();

    if (health_.clientAddress.empty()) m.label("No headset connected");
    else if (startsWith(health_.clientAddress, "127.0.0.1")) m.label("Headset: USB (" + health_.clientAddress + ")");
    else m.label("Headset: Wi-Fi " + health_.clientAddress.substr(0, health_.clientAddress.rfind(':')));
    if (!health_.clientAddress.empty()) {
        if (!health_.battery || !health_.battery->known()) m.label("Battery: -");
        else {
            std::string b = formatString("Battery: %u %%", health_.battery->battery);
            if (health_.battery->charging()) b += " - charging";
            else if (health_.battery->plugged()) b += " - plugged in";
            m.label(b);
        }
    }
    m.label(health_.usb.travelText());
    m.separator();

    const auto percentMenu = [&](const std::string& title, uint16_t current, std::function<void(uint16_t)> set) {
        m.submenu(title + ": " + percentLabel(current / 10));
        for (int p : Settings::kPercentPresets) m.radio(percentLabel(p), current == p * 10, [set, p] { set(uint16_t(p * 10)); });
        if (std::find(Settings::kPercentPresets.begin(), Settings::kPercentPresets.end(), current / 10) == Settings::kPercentPresets.end())
            m.radio(percentLabel(current / 10), true, [] {});
        m.endSubmenu();
    };
    percentMenu("Curved screen", applied_.curvature,
                [this](uint16_t v) { changeSettings([v](Settings& s) { s.curvature = v; }); });
    percentMenu("Dim surroundings", applied_.dim, [this](uint16_t v) { changeSettings([v](Settings& s) { s.dim = v; }); });
    m.item("Hand gestures (pinch & hold to move, two hands to resize)",
           [this] { changeSettings([](Settings& s) { s.handGestures = !s.handGestures; }); }, applied_.handGestures);
    m.item("Audio in headset", [this] { changeSettings([](Settings& s) { s.audio = !s.audio; }, {SettingKey::Audio}); },
           applied_.audio && options_.audioAllowed, options_.audioAllowed);
    m.item("Mute PC while mirroring", [this] { changeSettings([](Settings& s) { s.mutePC = !s.mutePC; }); },
           applied_.mutePC, options_.audioAllowed && applied_.audio && options_.source == Options::Source::Screen);

    m.submenu("Video");
    {
        m.label("Resolution");
        bool matched = false;
        for (int w : Settings::kMaxWidthPresets) {
            const std::string label = w == 3840 ? "Up to 3840 px wide (4K)" : w == 1920 ? "Up to 1920 px wide (1080p)" : "Up to " + std::to_string(w) + " px wide";
            matched |= applied_.maxWidth == w;
            m.radio("   " + label, applied_.maxWidth == w,
                    [this, w] { changeSettings([w](Settings& s) { s.maxWidth = w; }, {SettingKey::MaxWidth}); });
        }
        if (!matched) m.radio("   Up to " + std::to_string(applied_.maxWidth) + " px wide (command line)", true, [] {});
        m.separator();
        m.label("Frame rate");
        matched = false;
        for (int f : Settings::kFpsPresets) {
            matched |= applied_.fps == f;
            m.radio("   " + std::to_string(f) + " fps", applied_.fps == f,
                    [this, f] { changeSettings([f](Settings& s) { s.fps = f; }, {SettingKey::Fps}); });
        }
        if (!matched) m.radio("   " + std::to_string(applied_.fps) + " fps (command line)", true, [] {});
        m.separator();
        m.label("Codec");
        m.radio("   HEVC (H.265)", applied_.codec == proto::Codec::HEVC,
                [this] { changeSettings([](Settings& s) { s.codec = proto::Codec::HEVC; }, {SettingKey::Codec}); });
        m.radio("   H.264", applied_.codec == proto::Codec::H264,
                [this] { changeSettings([](Settings& s) { s.codec = proto::Codec::H264; }, {SettingKey::Codec}); });
        m.separator();
        m.label("Bitrate");
        matched = false;
        for (double b : Settings::kBitratePresets) {
            matched |= applied_.bitrateMbps == b;
            m.radio("   " + formatMbps(b) + " Mbit/s", applied_.bitrateMbps == b,
                    [this, b] { changeSettings([b](Settings& s) { s.bitrateMbps = b; }, {SettingKey::Bitrate}); });
        }
        if (!matched) m.radio("   " + formatMbps(applied_.bitrateMbps) + " Mbit/s (command line)", true, [] {});
        const auto displays = listDisplays();
        if (displays.size() > 1 && options_.source == Options::Source::Screen) {
            m.separator();
            m.label("Display");
            for (size_t i = 0; i < displays.size(); ++i) {
                const auto& d = displays[i];
                const bool checked = displayIndexOverride_ ? *displayIndexOverride_ == int(i)
                                     : applied_.display.empty() ? d.primary : applied_.display == d.deviceName;
                const std::string name = d.deviceName;
                const bool primary = d.primary;
                m.radio("   " + d.label(), checked, [this, name, primary] {
                    displayIndexOverride_.reset();
                    changeSettings([name, primary](Settings& s) { s.display = primary ? "" : name; });
                    if (source_) source_->update(captureSettings(applied_));
                });
            }
        }
    }
    m.endSubmenu();

    m.submenu("Controls");
    m.item("Keyboard shortcuts (Ctrl+Alt+Win + arrows, R, 0)",
           [this] { changeSettings([](Settings& s) { s.hotkeys = !s.hotkeys; }, {SettingKey::Hotkeys}); }, applied_.hotkeys);
    m.item("Mouse orbit & wheel zoom (Ctrl+Alt+Win)",
           [this] { changeSettings([](Settings& s) { s.mouseGestures = !s.mouseGestures; }, {SettingKey::MouseGestures}); },
           applied_.mouseGestures);
    m.separator();
    m.label("Mouse sensitivity");
    for (auto [label, value] : {std::pair{"Low", 0.05}, {"Normal", 0.1}, {"High", 0.2}}) {
        m.radio(std::string("   ") + label, applied_.mouseSensitivity == value,
                [this, value] { changeSettings([value](Settings& s) { s.mouseSensitivity = value; }, {SettingKey::MouseSensitivity}); },
                applied_.mouseGestures);
    }
    m.label("Wheel sensitivity");
    for (auto [label, value] : {std::pair{"Low", 0.5}, {"Normal", 1.0}, {"High", 2.0}}) {
        m.radio(std::string("   ") + label, applied_.scrollSensitivity == value,
                [this, value] { changeSettings([value](Settings& s) { s.scrollSensitivity = value; }, {SettingKey::ScrollSensitivity}); },
                applied_.mouseGestures);
    }
    m.endSubmenu();

    m.submenu("Headset");
    {
        const UsbStatus& usb = health_.usb;
        m.label(usb.linkText());
        if (usb.link == UsbStatus::Link::AdbMissing) {
            m.item("Copy install command (" + std::string(kAdbInstallCommand) + ")", [this] { copyToClipboard(kAdbInstallCommand); });
            m.item("Download platform-tools...", [this] { openUrl(kPlatformToolsUrl); });
        }
        if (usb.connected()) {
            m.label(usb.appText());
            if (usb.installing) m.label("Installing the headset app...");
            else if (!usb.installResult.empty()) m.label(usb.installResult);
            if (!usb.bundledVersion.empty() && !usb.installing) {
                std::string text = "Install headset app (" + usb.bundledVersion + ")";
                if (usb.app == UsbStatus::App::Installed) {
                    const int cmp = adb::compareVersions(usb.appVersion, usb.bundledVersion).value_or(0);
                    text = cmp < 0 ? "Update headset app (" + usb.appVersion + " -> " + usb.bundledVersion + ")"
                                   : "Reinstall headset app (" + usb.bundledVersion + ")";
                }
                m.item(text, [this] { usb_->installApp(); }, false, usb.app != UsbStatus::App::Unknown);
            }
            m.item("Launch headset app", [this] { usb_->launchApp(); }, false, usb.app != UsbStatus::App::NotInstalled);
            m.item("Open Quick Settings (Travel mode)...", [this] { usb_->openQuickSettings(); });
        }
        if (usb.link == UsbStatus::Link::AdbBroken || usb.serverUnstable) m.item("Restart adb", [this] { usb_->restartAdb(); });
        m.separator();
        m.item("Open headset app when connected",
               [this] { changeSettings([](Settings& s) { s.launchHeadsetApp = !s.launchHeadsetApp; }, {SettingKey::LaunchHeadsetApp}); },
               applied_.launchHeadsetApp, usb_ != nullptr);
    }
    m.endSubmenu();
    m.separator();
    m.item("Re-center (Ctrl+Alt+Win+R)", [this] { sendControl(proto::ControlCommand::recenter(), "menu Re-center"); });
    m.item("Reset placement (Ctrl+Alt+Win+0)", [this] { sendControl(proto::ControlCommand::reset(), "menu Reset placement"); });
    m.separator();
    m.item("Start with Windows", [this] { setStartWithWindows(!startWithWindows()); }, startWithWindows());
    m.item("Allow Wi-Fi through Windows Firewall...", [this] {
        const std::wstring exe = exePath();
        const bool pub = health_.firewall.publicNetwork;
        std::thread([this, exe, pub] {
            allowThroughFirewall(exe, pub);
            ui([this] { checkFirewallAsync(); });
        }).detach();
    });
    m.item("Show Logs...", [this] { ShellExecuteW(nullptr, L"open", logFilePath().c_str(), nullptr, nullptr, SW_SHOWNORMAL); });
    m.item("About VRScreen", [this] { showAbout(); });
    m.item("Quit VRScreen", [this] { requestQuit(); });
    m.show(hwnd_);
}

// MARK: Misc

bool App::startWithWindows() const {
    wchar_t value[1024];
    DWORD size = sizeof value;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS) return false;
    return std::wstring(value).find(exePath()) != std::wstring::npos;
}

void App::setStartWithWindows(bool on) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (on) {
        const std::wstring command = L"\"" + exePath() + L"\"";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                       DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, kRunValue);
    }
    RegCloseKey(key);
    log(on ? "start with Windows: on" : "start with Windows: off");
}

void App::showAbout() {
    const std::wstring text = widen(std::string("VRScreen ") + VRSCREEN_VERSION +
                                    " for Windows\n\nYour PC's screen and sound in a Meta Quest, over USB or Wi-Fi.\n\n"
                                    "This PC: " + DiscoveryResponder::computerName() +
                                    formatString(", TCP port %u", options_.port) +
                                    "\nLog: " + narrow(logFilePath()) + "\n\nMIT License. Meta and Meta Quest are "
                                    "trademarks of Meta Platforms, Inc.; this project is not affiliated with Meta.");
    MessageBoxW(nullptr, text.c_str(), L"About VRScreen", MB_OK | MB_ICONINFORMATION);
}

void App::openUrl(const std::wstring& url) { ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

void App::copyToClipboard(const std::string& text) {
    if (!OpenClipboard(hwnd_)) return;
    EmptyClipboard();
    const std::wstring w = widen(text);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

void App::shutdown() {
    if (quitting_) return;
    quitting_ = true;
    log("quitting");
    KillTimer(hwnd_, kSaveTimer);
    model_.saveIfDirty();
    stopWatching_ = true;
    if (settingsWatcher_.joinable()) settingsWatcher_.join();
    tray_.remove();
    if (hotkeys_) hotkeys_->unregisterAll();
    mouse_.reset();
    if (usb_) usb_->stop();  // removes the adb reverse it created
    if (source_) source_->stop();
    loopback_.reset();  // unmutes the PC if it muted it
    loop_.runSync([this] {
        if (discovery_) discovery_->stop();
        server_->stop();
    });
    pipeline_->stop();
    loop_.stop();
}

}  // namespace vr
