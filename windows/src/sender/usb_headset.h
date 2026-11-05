// The headset over USB, driven by adb (port of the Mac UsbHeadsetLink): finds the authorized
// headset (polling every 3 s), starts the adb server when needed (ADB_MDNS=0), sets up
// `adb reverse tcp:P tcp:P` (re-checked on every poll), launches the headset app once per attach,
// installs the APK next to the executable on request, reads travel mode, and removes on exit only
// the reverse it created. All adb calls run on its own thread.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "adb_parse.h"

namespace vr {

struct UsbStatus {
    enum class Link { Off, AdbMissing, AdbBroken, NoHeadset, Unauthorized, Authorizing, Offline, Ambiguous, Connected };
    Link link = Link::NoHeadset;
    std::string serial, model, adbError;
    int otherDevices = 0;
    int ambiguousCount = 0;
    // A Meta USB device (VID 2833) that adb does not list: no USB debugging, or no ADB driver.
    std::string metaUsbDevice;
    bool serverUnstable = false;
    enum class App { Unknown, NotInstalled, Installed } app = App::Unknown;
    std::string appVersion;
    std::string bundledVersion;  // empty without an APK next to the exe
    bool installing = false;
    std::string installResult;
    enum class Travel { Unknown, On, Off, FormatChanged } travel = Travel::Unknown;

    bool connected() const { return link == Link::Connected; }
    bool operator==(const UsbStatus&) const = default;
    std::string linkText() const;
    std::string appText() const;
    std::string travelText() const;
};

struct AdbResult {
    bool ok = false;
    bool timedOut = false;
    bool launched = true;
    int exitCode = -1;
    std::string output;
    std::string describe() const;
};

class Adb {
public:
    explicit Adb(std::wstring path) : path_(std::move(path)) {}
    static std::optional<Adb> locate();
    // Runs adb with `args` (UTF-8), waits at most `timeoutMs`; kills only this client on timeout.
    AdbResult run(const std::vector<std::string>& args, int timeoutMs = 3000) const;
    static bool serverRunning();
    const std::wstring& path() const { return path_; }

private:
    std::wstring path_;
};

// Product name of an attached Meta USB device (vendor 0x2833), or empty.
std::string metaUsbDeviceName();

class UsbHeadsetLink {
public:
    static constexpr const char* kPackage = "com.df0.vrscreen";

    UsbHeadsetLink(uint16_t port, std::optional<std::string> preferredSerial, std::wstring bundledApk, bool launchOnAttach,
                   std::function<void(const UsbStatus&)> onStatus);
    ~UsbHeadsetLink();
    void start();
    void stop();  // removes the reverse this process created
    void setLaunchOnAttach(bool on);
    void installApp();
    void launchApp();
    void restartAdb();
    void openQuickSettings();
    void refreshTravelMode();

private:
    void run();
    void post(std::function<void()> task);
    void poll();
    void publish();
    void attach(const adb::Device& device);
    void detach();
    void ensureReverse();
    void launch(const std::string& reason);
    void readApp();
    void readTravel();
    std::optional<std::string> startServer();
    AdbResult headset(const std::vector<std::string>& args, int timeoutMs = 3000);
    void removeCreatedReverse();

    uint16_t port_;
    std::optional<std::string> preferredSerial_;
    std::wstring bundledApk_;
    std::atomic<bool> launchOnAttach_;
    std::function<void(const UsbStatus&)> onStatus_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    bool stopping_ = false;

    // Thread state.
    std::optional<Adb> adb_;
    UsbStatus status_;
    std::optional<UsbStatus> published_;
    std::string attachedSerial_;
    std::string createdReverseSerial_;
    bool serverSeenRunning_ = false;
    std::deque<unsigned long long> serverRestarts_;
    std::string serverError_;
    unsigned long long serverRetryAt_ = 0;
    bool loggedExistingReverse_ = false;
    std::string lastLoggedSelection_;
    unsigned long long nextTravelRead_ = 0;
    unsigned long long travelFastUntil_ = 0;
};

}  // namespace vr
