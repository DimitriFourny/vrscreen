// Unit tests for the Windows sender's device-independent logic. Byte sequences come from
// docs/protocol.md ("Example byte sequences"), so the wire format matches the Mac sender.
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "adb_parse.h"
#include "audio_chunker.h"
#include "json.h"
#include "mouse_gestures.h"
#include "nal.h"
#include "options.h"
#include "protocol.h"
#include "settings.h"
#include "util.h"

using namespace vr;

namespace {
int g_failures = 0, g_checks = 0;
std::string g_test;

void check(bool ok, const char* expr, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s (line %d): %s\n", g_test.c_str(), line, expr);
    }
}
#define CHECK(x) check((x), #x, __LINE__)

proto::Bytes hex(const std::string& s) {
    proto::Bytes out;
    for (const auto& t : splitWhitespace(s)) out.push_back(uint8_t(std::stoul(t, nullptr, 16)));
    return out;
}

void test(const char* name, const std::function<void()>& body) {
    g_test = name;
    body();
}
}  // namespace

int main() {
    test("hello and framing", [] {
        CHECK(proto::hello() == hex("56 52 53 43 00 01"));
        CHECK(proto::pingMessage(0x0A0B0C0D0E0F1011ULL) == hex("10 00 00 00 08 0A 0B 0C 0D 0E 0F 10 11"));
        CHECK(proto::frameAckMessage(42) == hex("12 00 00 00 04 00 00 00 2A"));
        CHECK(proto::headsetStatusMessage(72, 1) == hex("13 00 00 00 02 48 01"));
    });

    test("control commands", [] {
        CHECK(proto::ControlCommand::translate(-1, 1).encoded() == hex("03 00 00 00 03 06 FF 01"));
        CHECK(proto::ControlCommand::orbit(500, -250).encoded() == hex("03 00 00 00 05 07 01 F4 FF 06"));
        CHECK(proto::ControlCommand::recenter().encoded() == hex("03 00 00 00 01 01"));
        CHECK(proto::ControlCommand::setCurvature(350).encoded() == hex("03 00 00 00 03 0A 01 5E"));
        CHECK(proto::ControlCommand::setDim(1000).encoded() == hex("03 00 00 00 03 0B 03 E8"));
        CHECK(proto::ControlCommand::setDim(4000).encoded() == hex("03 00 00 00 03 0B 03 E8"));  // clamped
        CHECK(proto::ControlCommand::setHandGestures(true).encoded() == hex("03 00 00 00 02 0C 01"));
        CHECK(proto::ControlCommand::translate(1, 0).describe() == "TRANSLATE dx=+1 dy=0");
    });

    test("audio messages", [] {
        CHECK((proto::AudioConfigMessage{1, 48000, 2}.encoded() == hex("04 00 00 00 06 01 00 00 BB 80 02")));
        proto::AudioMessage a;
        a.captureMicros = 0x0102030405060708ULL;
        a.pcm = {0x01, 0x00, 0xFE, 0xFF};
        CHECK(a.encoded() == hex("05 00 00 00 0C 01 02 03 04 05 06 07 08 01 00 FE FF"));
    });

    test("frame and config", [] {
        proto::FrameMessage f;
        f.keyframe = true;
        f.captureMicros = 2;
        f.accessUnit = hex("00 00 00 01 26 01");
        CHECK(f.encoded() == hex("02 00 00 00 0F 01 00 00 00 00 00 00 00 02 00 00 00 01 26 01"));
        proto::ConfigMessage c;
        c.codec = proto::Codec::HEVC;
        c.width = 3840;
        c.height = 2160;
        c.parameterSets = hex("00 00 00 01 40 01");
        CHECK(c.encoded() == hex("01 00 00 00 0F 02 00 00 0F 00 00 00 08 70 00 00 00 01 40 01"));
    });

    test("reader", [] {
        proto::MessageReader r(true);
        proto::Bytes stream = proto::hello();
        auto ping = proto::pingMessage(7);
        stream.insert(stream.end(), ping.begin(), ping.end());
        auto unknown = proto::frame(0x7F, hex("AA BB"));
        stream.insert(stream.end(), unknown.begin(), unknown.end());
        uint8_t type;
        proto::Bytes payload;
        // Byte by byte: messages split across reads.
        std::vector<uint8_t> types;
        for (uint8_t b : stream) {
            r.append(&b, 1);
            while (r.next(type, payload)) types.push_back(type);
        }
        CHECK(types.size() == 2 && types[0] == proto::kPing && types[1] == 0x7F);
        CHECK(r.error().empty());
        proto::MessageReader bad(true);
        auto wrong = hex("56 52 53 43 00 02");
        bad.append(wrong.data(), wrong.size());
        CHECK(!bad.next(type, payload) && !bad.error().empty());
        proto::MessageReader big(false);
        auto huge = hex("02 05 00 00 00");
        big.append(huge.data(), huge.size());
        CHECK(!big.next(type, payload) && !big.error().empty());
    });

    test("frame ack window", [] {
        proto::FrameAckWindow w(3);
        CHECK(w.open() && !w.active());
        for (int i = 0; i < 5; ++i) w.recordSent();
        CHECK(w.open());  // inactive until the first ack
        CHECK(w.recordAck(1) && w.active() && w.inFlight() == 4 && !w.open());
        CHECK(!w.recordAck(0));  // backwards
        CHECK(!w.recordAck(6));  // more than sent
        CHECK(w.recordAck(3) && w.open());
        proto::FrameAckWindow wrap(3);
        for (int i = 0; i < 2; ++i) wrap.recordSent();
        CHECK(wrap.recordAck(2));
    });

    test("discovery", [] {
        CHECK(proto::discovery::discover() == hex("56 52 53 43 01 00 01"));
        CHECK(proto::discovery::announce(7777, "Mac") == hex("56 52 53 43 02 00 01 1E 61 03 4D 61 63"));
        auto d = proto::discovery::discover();
        CHECK(proto::discovery::isDiscover(d.data(), d.size()));
        auto a = proto::discovery::announce(7777, std::string(70, 'x'));
        CHECK(a.size() == 10 + 63);
        // "é" is 2 bytes: 62 x then é must not be cut in the middle.
        auto cut = proto::discovery::announce(1, std::string(62, 'x') + "\xC3\xA9");
        CHECK(cut[9] == 62);
        auto parsed = proto::discovery::parseAnnounce(a.data(), a.size());
        CHECK(parsed && parsed->tcpPort == 7777 && parsed->name.size() == 63);
    });

    test("nal rebuild", [] {
        using proto::Codec;
        // HEVC: AUD(35), VPS, SPS, PPS, IDR(19) with 3-byte start codes.
        auto in = hex("00 00 01 46 01 50  00 00 01 40 01 0C  00 00 00 01 42 01 01  00 00 01 44 01 C1  00 00 01 26 01 AF 80");
        nal::AccessUnit au;
        std::string error;
        CHECK(nal::rebuild(Codec::HEVC, in.data(), in.size(), {}, au, error));
        CHECK(au.keyframe);
        CHECK(au.parameterSets == hex("00 00 00 01 40 01 0C 00 00 00 01 42 01 01 00 00 00 01 44 01 C1"));
        CHECK(au.data == hex("00 00 00 01 40 01 0C 00 00 00 01 42 01 01 00 00 00 01 44 01 C1 00 00 00 01 26 01 AF 80"));
        // A P frame (TRAIL_R = 1).
        auto p = hex("00 00 00 01 02 01 D0 10");
        CHECK(nal::rebuild(Codec::HEVC, p.data(), p.size(), au.parameterSets, au, error));
        CHECK(!au.keyframe && au.parameterSets.empty() && au.data == p);
        // H.264 IDR without SPS/PPS: the cached ones are prepended.
        auto idr = hex("00 00 00 01 09 F0 00 00 00 01 65 88 84");
        auto cached = hex("00 00 00 01 67 64 00 28 00 00 00 01 68 EE");
        CHECK(nal::rebuild(Codec::H264, idr.data(), idr.size(), cached, au, error));
        CHECK(au.keyframe && au.data == hex("00 00 00 01 67 64 00 28 00 00 00 01 68 EE 00 00 00 01 65 88 84"));
        CHECK(!nal::rebuild(Codec::H264, idr.data(), idr.size(), {}, au, error));
    });

    test("settings json", [] {
        Settings s;
        s.curvature = 350;
        s.codec = proto::Codec::H264;
        s.bitrateMbps = 12.5;
        s.display = "\\\\.\\DISPLAY2";
        s.hotkeys = false;
        const Settings back = settingsFromJson(settingsToJson(s));
        CHECK(back == s);
        // Clamped, wrong types and unknown keys ignored, garbage → defaults.
        const Settings h = settingsFromJson(R"({"curvature": 5000, "fps": "fast", "audio": false, "extra": [1, {"a": 2}], "maxWidth": 1})");
        CHECK(h.curvature == 1000 && h.fps == 60 && !h.audio && h.maxWidth == 16);
        CHECK(settingsFromJson("not json") == Settings{});
        auto o = json::parseObject(R"({"s": "a\"bé"})");
        CHECK(o && std::get<std::string>(o->at("s")) == "a\"b\xC3\xA9");
    });

    test("options", [] {
        auto o = parseOptions({"--fps", "90", "--codec", "h264", "--no-adb", "--port", "7790", "--source", "test"});
        CHECK(o.overrides.keys.count(SettingKey::Fps) && o.overrides.values.fps == 90);
        CHECK(o.overrides.values.codec == proto::Codec::H264 && !o.adb && o.port == 7790);
        CHECK(o.source == Options::Source::Test);
        bool threw = false;
        try {
            parseOptions({"--fps", "0"});
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);
        auto steps = parseControlSpec("translate:1,0;orbit:50,0*3;wait:100;dim:1000");
        CHECK(steps.size() == 6 && steps[1].command == proto::ControlCommand::orbit(50, 0) && steps[4].wait);
        Settings base;
        base.fps = 30;
        CHECK(o.overrides.applied(base).fps == 90);
    });

    test("audio chunker", [] {
        AudioChunker c(48000, 2, 480);
        // 1000 frames from t = 1 s: two chunks, 40 frames pending.
        auto chunks = c.append(1000, 1'000'000, [](int, int ch) { return ch == 0 ? 0.5f : -1.0f; });
        CHECK(chunks.size() == 2 && chunks[0].captureMicros == 1'000'000 && chunks[1].captureMicros == 1'010'000);
        CHECK(chunks[0].pcm.size() == 1920);
        CHECK(chunks[0].pcm[0] == 0x00 && chunks[0].pcm[1] == 0x40);  // 16384 little-endian (0.5 * 32767 rounded)
        CHECK(chunks[0].pcm[2] == 0x01 && chunks[0].pcm[3] == 0x80);  // -32767
        CHECK(c.pendingFrames() == 40);
        // Continuation: 440 more frames at the expected time complete the third chunk.
        auto more = c.append(440, 1'000'000 + 1000 * 1'000'000 / 48000, [](int, int) { return 0.0f; });
        CHECK(more.size() == 1 && more[0].captureMicros == 1'020'000);
        // A gap drops the partial chunk.
        c.append(100, 2'000'000, [](int, int) { return 0.0f; });
        auto after = c.append(480, 3'000'000, [](int, int) { return 0.0f; });
        CHECK(c.discardedFrames() == 100 && after.size() == 1 && after[0].captureMicros == 3'000'000);
        CHECK(pcm16(2.0f) == 32767 && pcm16(-2.0f) == -32768 && pcm16(0.0f) == 0);
    });

    test("downmix", [] {
        auto stereo = makeDownmix(2, 0x3);
        CHECK(stereo.left[0] == 1 && stereo.right[1] == 1 && stereo.left[1] == 0);
        auto mono = makeDownmix(1, 0);
        CHECK(mono.left[0] == 1 && mono.right[0] == 1);
        auto surround = makeDownmix(6, 0x3F);  // FL FR FC LFE BL BR
        CHECK(surround.left[3] == 0 && surround.right[3] == 0);  // LFE dropped
        CHECK(surround.left[2] > 0 && surround.left[2] == surround.right[2]);
        CHECK(surround.left[1] == 0 && surround.right[0] == 0);
    });

    test("adb parsing", [] {
        const auto devices = adb::parseDevices(
            "* daemon started successfully\nList of devices attached\n"
            "2G0YC5ZG8L0123 device usb:1-1 product:eureka model:Quest_3 device:eureka transport_id:3\n"
            "emulator-5554 device product:sdk model:sdk_phone device:x\n"
            "XYZ unauthorized usb:1-2 transport_id:4\n");
        CHECK(devices.size() == 3 && devices[0].model == "Quest 3" && devices[0].isQuest());
        auto sel = adb::select(devices, std::nullopt);
        CHECK(sel.kind == adb::Selection::Kind::Ready && sel.device.serial == "2G0YC5ZG8L0123" && sel.others == 2);
        auto onlyUnauth = adb::select({devices[2]}, std::nullopt);
        CHECK(onlyUnauth.kind == adb::Selection::Kind::Unauthorized);
        auto twoPhones = adb::select({devices[1], devices[1]}, std::nullopt);
        CHECK(twoPhones.kind == adb::Selection::Kind::Ambiguous && twoPhones.count == 2);
        auto preferred = adb::select(devices, std::string("emulator-5554"));
        CHECK(preferred.kind == adb::Selection::Kind::Ready && preferred.device.serial == "emulator-5554");

        auto entries = adb::parseReverseList("UsbFfs tcp:7777 tcp:7777\nhost-19 tcp:8000 tcp:9000\n");
        CHECK(adb::decideReverse(entries, 7777) == adb::ReverseDecision::Present);
        CHECK(adb::decideReverse(entries, 7790) == adb::ReverseDecision::Create);
        std::string local;
        CHECK(adb::decideReverse(entries, 8000, &local) == adb::ReverseDecision::Replace && local == "tcp:9000");

        CHECK(adb::parseVersionName("    versionName=0.3.3\n") == "0.3.3");
        CHECK(adb::compareVersions("0.10.0", "0.9.1") == 1 && adb::compareVersions("0.3", "0.3.0") == 0);
        CHECK(!adb::compareVersions("dev", "0.3.0"));
        CHECK(!adb::installFailure("Performing Streamed Install\nSuccess\n"));
        CHECK(adb::installFailure("adb: failed to install x.apk: Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: Package ...]") ==
              std::optional<std::string>("INSTALL_FAILED_UPDATE_INCOMPATIBLE"));
        CHECK(adb::parseTravelMode("  TravelMode: off\n * Travel Mode: true\n") == adb::TravelMode::On);
        CHECK(adb::parseTravelMode("nothing") == adb::TravelMode::FormatChanged);
    });

    test("mouse gestures", [] {
        CHECK(gestureModeFrom(true, true, true, false) == GestureMode::OrbitAndDistance);
        CHECK(gestureModeFrom(true, true, true, true) == GestureMode::Size);
        CHECK(gestureModeFrom(true, true, false, false) == GestureMode::Inactive);
        MouseGestureAccumulator acc(0.1, 1.0);
        acc.addMouseDelta(3, -2, GestureMode::OrbitAndDistance);  // 0.3° right, 0.2° up
        acc.addWheel(120, GestureMode::OrbitAndDistance);         // one notch away: closer
        auto cmds = acc.flush();
        CHECK(cmds.size() == 2 && cmds[0] == proto::ControlCommand::orbit(30, 20) &&
              cmds[1] == proto::ControlCommand::distanceFine(-25));
        acc.addMouseDelta(0.04, 0, GestureMode::OrbitAndDistance);  // 0.4 units: carried over
        CHECK(acc.flush().empty());
        acc.addMouseDelta(0.07, 0, GestureMode::OrbitAndDistance);
        auto carried = acc.flush();
        CHECK(carried.size() == 1 && carried[0] == proto::ControlCommand::orbit(1, 0));
        acc.addWheel(-60, GestureMode::Size);
        auto size = acc.flush();
        CHECK(size.size() == 1 && size[0] == proto::ControlCommand::sizeFine(-12));
    });

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
