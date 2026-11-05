#include "options.h"

#include <stdexcept>

#include "util.h"

namespace vr {

const char* const kUsage = R"(Usage: vrscreen-sender [options]

Streams this PC's screen and sound to the VRScreen headset app (Meta Quest).
Without options it runs as a tray icon; settings come from the tray menu.

Session overrides of saved settings:
  --fps N                  Target frame rate (default 60)
  --max-width N            Scale the display down to fit this width (default 3840)
  --codec hevc|h264        Video codec (default hevc)
  --bitrate MBPS           Average bitrate in Mbit/s (default 30)
  --no-hotkeys             No Ctrl+Alt+Win shortcuts
  --no-mouse               No Ctrl+Alt+Win + mouse/scroll gestures
  --mouse-sensitivity DEG  Orbit degrees per pixel (default 0.1)
  --scroll-sensitivity X   Scroll zoom multiplier (default 1)
  --no-launch              Do not start the headset app when it appears on USB

Developer options:
  --console                Show the log in a console window (or the calling terminal)
  --source screen|test     Desktop Duplication capture, or a synthetic test pattern with beeps
  --display N              Display index from --list-displays (this session)
  --list-displays          Print the displays and exit
  --port N                 TCP port (default 7777)
  --discovery-port N       UDP discovery port (default 7778)
  --no-discovery           Do not answer LAN discovery
  --keyframe-interval S    Maximum seconds between keyframes (default 5)
  --max-in-flight N        Max frames sent but not acknowledged (default 3)
  --width N --height N     Test pattern size (default 1920x1080)
  --no-menu                No tray icon (a port in use is then fatal)
  --no-audio               Never capture or send audio
  --audio-offset-ms MS     Add MS to audio timestamps (e.g. 150 for Bluetooth-like delay compensation)
  --encoder NAME           Prefer the hardware encoder whose name contains NAME (e.g. NVIDIA, AMD, Intel)
  --settings PATH          Settings file instead of %APPDATA%\VRScreen\settings.json
  --no-adb                 Never use adb (Wi-Fi only)
  --adb-serial S           The headset's adb serial when several devices are attached
  --debug-control-on-connect SPEC  e.g. "translate:1,0;orbit:50,0*50;curvature:500;wait:3000;dim:1000"
  --version                Print the version
  --help                   This text
)";

static long long parseInt(const std::string& flag, const std::string& value, long long lo, long long hi) {
    try {
        size_t used = 0;
        const long long v = std::stoll(value, &used);
        if (used != value.size() || v < lo || v > hi) throw std::out_of_range("");
        return v;
    } catch (const std::exception&) {
        throw std::runtime_error(formatString("%s expects an integer between %lld and %lld, got \"%s\"", flag.c_str(),
                                              lo, hi, value.c_str()));
    }
}

static double parseDouble(const std::string& flag, const std::string& value, double lo, double hi) {
    try {
        size_t used = 0;
        const double v = std::stod(value, &used);
        if (used != value.size() || !(v >= lo && v <= hi)) throw std::out_of_range("");
        return v;
    } catch (const std::exception&) {
        throw std::runtime_error(
            formatString("%s expects a number between %g and %g, got \"%s\"", flag.c_str(), lo, hi, value.c_str()));
    }
}

std::vector<DebugControlStep> parseControlSpec(const std::string& spec) {
    std::vector<DebugControlStep> steps;
    size_t start = 0;
    while (start <= spec.size()) {
        size_t end = spec.find(';', start);
        if (end == std::string::npos) end = spec.size();
        std::string item = trim(spec.substr(start, end - start));
        start = end + 1;
        if (item.empty()) continue;
        int repeat = 1;
        if (const size_t star = item.find('*'); star != std::string::npos) {
            repeat = int(parseInt("repeat", item.substr(star + 1), 1, 10000));
            item = item.substr(0, star);
        }
        std::string name = item, args;
        if (const size_t colon = item.find(':'); colon != std::string::npos) {
            name = item.substr(0, colon);
            args = item.substr(colon + 1);
        }
        std::vector<long long> nums;
        size_t a = 0;
        while (!args.empty() && a <= args.size()) {
            size_t comma = args.find(',', a);
            if (comma == std::string::npos) comma = args.size();
            nums.push_back(parseInt(name, trim(args.substr(a, comma - a)), -100000, 100000));
            a = comma + 1;
        }
        const auto need = [&](size_t n) {
            if (nums.size() != n) throw std::runtime_error("control step \"" + item + "\": expects " + std::to_string(n) + " argument(s)");
        };
        DebugControlStep step;
        if (name == "wait") {
            need(1);
            step.wait = true;
            step.waitMs = int(nums[0]);
        } else if (name == "recenter") { need(0); step.command = proto::ControlCommand::recenter(); }
        else if (name == "reset") { need(0); step.command = proto::ControlCommand::reset(); }
        else if (name == "distance") { need(1); step.command = proto::ControlCommand::distance(int8_t(nums[0])); }
        else if (name == "size") { need(1); step.command = proto::ControlCommand::size(int8_t(nums[0])); }
        else if (name == "translate") { need(2); step.command = proto::ControlCommand::translate(int8_t(nums[0]), int8_t(nums[1])); }
        else if (name == "orbit") { need(2); step.command = proto::ControlCommand::orbit(int16_t(nums[0]), int16_t(nums[1])); }
        else if (name == "distancefine") { need(1); step.command = proto::ControlCommand::distanceFine(int16_t(nums[0])); }
        else if (name == "sizefine") { need(1); step.command = proto::ControlCommand::sizeFine(int16_t(nums[0])); }
        else if (name == "curvature") { need(1); step.command = proto::ControlCommand::setCurvature(uint16_t(nums[0])); }
        else if (name == "dim") { need(1); step.command = proto::ControlCommand::setDim(uint16_t(nums[0])); }
        else if (name == "hands") { need(1); step.command = proto::ControlCommand::setHandGestures(nums[0] != 0); }
        else throw std::runtime_error("unknown control step \"" + name + "\"");
        for (int i = 0; i < repeat; ++i) steps.push_back(step);
    }
    return steps;
}

Options parseOptions(const std::vector<std::string>& args) {
    Options o;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::runtime_error(a + " needs a value");
            return args[++i];
        };
        auto& ov = o.overrides;
        if (a == "--fps") { ov.values.fps = int(parseInt(a, value(), 1, 240)); ov.keys.insert(SettingKey::Fps); }
        else if (a == "--max-width") { ov.values.maxWidth = int(parseInt(a, value(), 16, 16384)); ov.keys.insert(SettingKey::MaxWidth); }
        else if (a == "--codec") {
            const std::string v = value();
            if (v == "hevc" || v == "h265") ov.values.codec = proto::Codec::HEVC;
            else if (v == "h264" || v == "avc") ov.values.codec = proto::Codec::H264;
            else throw std::runtime_error("--codec expects hevc or h264");
            ov.keys.insert(SettingKey::Codec);
        }
        else if (a == "--bitrate") { ov.values.bitrateMbps = parseDouble(a, value(), 0.5, 500); ov.keys.insert(SettingKey::Bitrate); }
        else if (a == "--no-hotkeys") { ov.values.hotkeys = false; ov.keys.insert(SettingKey::Hotkeys); }
        else if (a == "--no-mouse") { ov.values.mouseGestures = false; ov.keys.insert(SettingKey::MouseGestures); }
        else if (a == "--mouse-sensitivity") { ov.values.mouseSensitivity = parseDouble(a, value(), 0.001, 10); ov.keys.insert(SettingKey::MouseSensitivity); }
        else if (a == "--scroll-sensitivity") { ov.values.scrollSensitivity = parseDouble(a, value(), 0.01, 100); ov.keys.insert(SettingKey::ScrollSensitivity); }
        else if (a == "--no-launch") { ov.values.launchHeadsetApp = false; ov.keys.insert(SettingKey::LaunchHeadsetApp); }
        else if (a == "--source") {
            const std::string v = value();
            if (v == "screen") o.source = Options::Source::Screen;
            else if (v == "test") o.source = Options::Source::Test;
            else throw std::runtime_error("--source expects screen or test");
        }
        else if (a == "--display") o.displayIndex = int(parseInt(a, value(), 0, 64));
        else if (a == "--list-displays") o.listDisplays = true;
        else if (a == "--port") o.port = uint16_t(parseInt(a, value(), 1, 65535));
        else if (a == "--discovery-port") o.discoveryPort = uint16_t(parseInt(a, value(), 1, 65535));
        else if (a == "--no-discovery") o.discoveryPort.reset();
        else if (a == "--keyframe-interval") o.keyframeIntervalSeconds = parseDouble(a, value(), 0.1, 3600);
        else if (a == "--max-in-flight") o.maxInFlight = int(parseInt(a, value(), 1, 1000));
        else if (a == "--width") o.testWidth = int(parseInt(a, value(), 16, 16384)) & ~1;
        else if (a == "--height") o.testHeight = int(parseInt(a, value(), 16, 16384)) & ~1;
        else if (a == "--no-menu") o.menu = false;
        else if (a == "--no-audio") o.audioAllowed = false;
        else if (a == "--audio-offset-ms") o.audioOffsetMs = int(parseInt(a, value(), 0, 1000));
        else if (a == "--encoder") o.encoderName = value();
        else if (a == "--settings") o.settingsPath = widen(value());
        else if (a == "--no-adb") o.adb = false;
        else if (a == "--adb-serial") o.adbSerial = value();
        else if (a == "--debug-control-on-connect") o.controlOnConnect = parseControlSpec(value());
        else if (a == "--console") o.console = true;
        else if (a == "--version") o.version = true;
        else if (a == "--help" || a == "-h" || a == "/?") o.help = true;
        else throw std::runtime_error("unknown option " + a);
    }
    return o;
}

}  // namespace vr
