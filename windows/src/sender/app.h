// The sender application: owns the settings, the streaming parts (event loop, server, pipeline,
// capture, audio, discovery, USB) and the UI (tray icon, menu, problems, hotkeys, mouse). The
// UI lives on the main thread; streaming state on the event loop thread.
#pragma once

#include <windows.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "audio_capture.h"
#include "capture.h"
#include "discovery.h"
#include "event_loop.h"
#include "firewall.h"
#include "hotkeys.h"
#include "mouse_control.h"
#include "options.h"
#include "pipeline.h"
#include "server.h"
#include "settings.h"
#include "tray.h"
#include "usb_headset.h"

namespace vr {

class App {
public:
    explicit App(Options options);
    ~App();
    int run();
    // Any thread: ask the app to quit (Ctrl-C, logoff).
    void requestQuit();

private:
    struct Problem {
        bool error = false;
        std::string title, detail;
        std::vector<std::pair<std::string, std::function<void()>>> actions;
    };
    struct Health {
        std::optional<uint16_t> portInUse;
        std::string captureError;
        std::string encoderError;
        std::string audioError;
        UsbStatus usb;
        std::optional<proto::HeadsetStatus> battery;
        std::string clientAddress;  // empty: no client
        FirewallState firewall;
    };

    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handleMessage(UINT, WPARAM, LPARAM);
    void ui(std::function<void()> task);  // run on the UI thread
    void startNetwork();                   // loop thread
    void startSources();                   // loop thread, once listening
    void apply(const Settings& next);      // UI thread
    void applyAudio();                     // UI thread
    void changeSettings(const std::function<void(Settings&)>& change, std::initializer_list<SettingKey> keys = {});
    void sendControl(const proto::ControlCommand& command, const std::string& origin);
    void showMenu();
    void updateTray();
    std::vector<Problem> problems();
    void checkFirewallAsync();
    void shutdown();
    CaptureSettings captureSettings(const Settings& s) const;
    EncoderSettings encoderSettings(const Settings& s) const;
    bool startWithWindows() const;
    void setStartWithWindows(bool on);
    void showAbout();
    void watchSettingsFile();
    void openUrl(const std::wstring& url);
    void copyToClipboard(const std::string& text);

    Options options_;
    SettingsModel model_;
    Settings applied_;
    std::optional<int> displayIndexOverride_;
    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    TrayIcon tray_;
    HICON icon_ = nullptr;
    Health health_;
    std::mutex uiMutex_;
    std::deque<std::function<void()>> uiTasks_;
    bool quitting_ = false;
    std::wstring bundledApk_;

    EventLoop loop_;
    std::unique_ptr<MirrorServer> server_;
    std::unique_ptr<SenderPipeline> pipeline_;
    std::unique_ptr<DiscoveryResponder> discovery_;
    std::unique_ptr<AudioSink> audioSink_;
    std::unique_ptr<LoopbackCapture> loopback_;
    std::unique_ptr<VideoSource> source_;
    std::unique_ptr<UsbHeadsetLink> usb_;
    std::unique_ptr<HotkeyController> hotkeys_;
    std::unique_ptr<MouseControl> mouse_;
    bool sourcesStarted_ = false;            // loop thread
    std::atomic<bool> clientConnected_{false};
    std::atomic<bool> firewallChecking_{false};
    std::atomic<bool> stopWatching_{false};
    std::thread settingsWatcher_;
    std::optional<proto::HeadsetStatus> lastHeadsetStatus_;  // loop thread
};

}  // namespace vr
