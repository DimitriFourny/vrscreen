// vrscreen-viewer.exe: a headless test client for the sender, speaking the same protocol as the
// Quest app (hello check, CONFIG/FRAME/CONTROL/AUDIO, PING/PONG clock offset, FRAME_ACK,
// HEADSET_STATUS). Prints stats once per second; optionally decodes (software), writes a PNG
// snapshot, a WAV of the audio, and measures A/V sync against the sender's test pattern.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mfapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "decoder.h"
#include "nal.h"
#include "protocol.h"
#include "util.h"

using namespace vr;

namespace {

const char kUsage[] = R"(Usage: vrscreen-viewer [options]
  --host HOST             Sender address (default 127.0.0.1)
  --port N                Sender port (default 7777)
  --discover              Find the sender by UDP broadcast
  --discovery-port N      Discovery port (default 7778)
  --duration S            Exit after S seconds
  --decode                Decode every frame (software decoder) and report decoded fps
  --snapshot PATH.png     Decode and write the first keyframe as PNG
  --exit-after-snapshot   Exit once the snapshot is written
  --audio-wav PATH.wav    Write the received audio (gaps filled with silence)
  --av-sync               With the sender's --source test: pair flashes and beeps, log audio - video
  --no-ack                Never send FRAME_ACK
  --fake-battery SPEC     Send HEADSET_STATUS like a headset: 72, 72,charging, 100,plugged, unknown
  --dump PATH             Write the received video access units (Annex-B elementary stream)
)";

struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 7777;
    bool discover = false;
    uint16_t discoveryPort = 7778;
    double duration = 0;
    bool decode = false;
    std::wstring snapshot;
    bool exitAfterSnapshot = false;
    std::wstring wav;
    bool avSync = false;
    bool ack = true;
    std::optional<proto::HeadsetStatus> battery;
    std::wstring dump;
};

uint64_t now() { return nowMicros(); }

bool discoverSender(const Options& o, std::string& host, uint16_t& port) {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL yes = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof yes);
    DWORD timeout = 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    const proto::Bytes discover = proto::discovery::discover();
    for (int attempt = 0; attempt < 30; ++attempt) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(o.discoveryPort);
        to.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(s, reinterpret_cast<const char*>(discover.data()), int(discover.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);  // a sender on this PC
        sendto(s, reinterpret_cast<const char*>(discover.data()), int(discover.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof to);
        uint8_t buffer[512];
        sockaddr_in from{};
        int fromLen = sizeof from;
        const int n = recvfrom(s, reinterpret_cast<char*>(buffer), sizeof buffer, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n > 0) {
            if (auto a = proto::discovery::parseAnnounce(buffer, size_t(n))) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                host = ip;
                port = a->tcpPort;
                std::printf("discovery: \"%s\" at %s:%u\n", a->name.c_str(), ip, port);
                closesocket(s);
                return true;
            }
        }
    }
    closesocket(s);
    return false;
}

class WavWriter {
public:
    bool open(const std::wstring& path) {
        file_.open(path, std::ios::binary | std::ios::trunc);
        if (!file_) return false;
        file_.write(std::string(44, '\0').data(), 44);
        return true;
    }
    void write(const uint8_t* pcm, size_t bytes, uint32_t rate, uint8_t channels, uint64_t micros) {
        if (!file_) return;
        rate_ = rate;
        channels_ = channels;
        const size_t frameBytes = size_t(channels) * 2;
        if (start_ == 0) start_ = micros;
        // Fill gaps (up to 10 s) so a position in the file maps to capture time.
        const double expectedFrames = double(micros - start_) * rate / 1e6;
        const long long gap = (long long)std::llround(expectedFrames) - (long long)(dataBytes_ / frameBytes);
        if (gap > 0 && gap < rate * 10) {
            std::vector<char> zeros(size_t(gap) * frameBytes, 0);
            file_.write(zeros.data(), std::streamsize(zeros.size()));
            dataBytes_ += zeros.size();
        }
        file_.write(reinterpret_cast<const char*>(pcm), std::streamsize(bytes));
        dataBytes_ += bytes;
    }
    void close() {
        if (!file_) return;
        file_.seekp(0);
        auto u32 = [&](uint32_t v) { file_.write(reinterpret_cast<const char*>(&v), 4); };
        auto u16 = [&](uint16_t v) { file_.write(reinterpret_cast<const char*>(&v), 2); };
        file_.write("RIFF", 4);
        u32(uint32_t(36 + dataBytes_));
        file_.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(channels_);
        u32(rate_);
        u32(rate_ * channels_ * 2);
        u16(uint16_t(channels_ * 2));
        u16(16);
        file_.write("data", 4);
        u32(uint32_t(dataBytes_));
        file_.close();
    }
    uint64_t startMicros() const { return start_; }

private:
    std::ofstream file_;
    uint64_t start_ = 0;
    size_t dataBytes_ = 0;
    uint32_t rate_ = 48000;
    uint8_t channels_ = 2;
};

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--host") o.host = value();
        else if (a == "--port") o.port = uint16_t(std::stoi(value()));
        else if (a == "--discover") o.discover = true;
        else if (a == "--discovery-port") o.discoveryPort = uint16_t(std::stoi(value()));
        else if (a == "--duration") o.duration = std::stod(value());
        else if (a == "--decode") o.decode = true;
        else if (a == "--snapshot") o.snapshot = widen(value());
        else if (a == "--exit-after-snapshot") o.exitAfterSnapshot = true;
        else if (a == "--audio-wav") o.wav = widen(value());
        else if (a == "--av-sync") { o.avSync = true; o.decode = true; }
        else if (a == "--no-ack") o.ack = false;
        else if (a == "--dump") o.dump = widen(value());
        else if (a == "--fake-battery") {
            const std::string v = value();
            proto::HeadsetStatus s;
            if (v != "unknown") s.battery = uint8_t(std::stoi(v));
            if (v.find("charging") != std::string::npos) s.flags |= 1;
            if (v.find("plugged") != std::string::npos || v.find("charging") != std::string::npos) s.flags |= 2;
            o.battery = s;
        } else if (a == "--help" || a == "-h") {
            std::printf("%s", kUsage);
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n\n%s", a.c_str(), kUsage);
            return 2;
        }
    }
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    std::string host = o.host;
    uint16_t port = o.port;
    if (o.discover && !discoverSender(o, host, port)) {
        std::printf("discovery: no sender answered\n");
        return 1;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        std::printf("cannot connect to %s:%u: %s\n", host.c_str(), port, win32ErrorText(DWORD(WSAGetLastError())).c_str());
        return 1;
    }
    BOOL one = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    std::printf("connected to %s:%u\n", host.c_str(), port);
    const auto sendBytes = [&](const proto::Bytes& b) { send(s, reinterpret_cast<const char*>(b.data()), int(b.size()), 0); };
    if (o.battery) sendBytes(proto::headsetStatusMessage(o.battery->battery, o.battery->flags));

    proto::MessageReader reader(true);
    Decoder decoder;
    bool decoderReady = false;
    WavWriter wav;
    if (!o.wav.empty() && !wav.open(o.wav)) std::printf("cannot write %s\n", narrow(o.wav).c_str());
    std::ofstream dump;
    if (!o.dump.empty()) dump.open(o.dump, std::ios::binary | std::ios::trunc);

    std::optional<proto::ConfigMessage> config;
    std::optional<proto::AudioConfigMessage> audioConfig;
    uint32_t framesReceived = 0;
    bool waitingKeyframe = true;
    bool snapshotDone = o.snapshot.empty();
    // Clock offset (sender - client) from the PING with the smallest RTT among the last 16.
    std::deque<std::pair<int64_t, int64_t>> samples;  // rtt, offset
    std::optional<int64_t> offset;
    int64_t lastRtt = 0;
    int pingsSent = 0;
    uint64_t nextPing = now();
    const uint64_t start = now();
    uint64_t nextStats = start + 1'000'000;
    uint64_t nextBattery = start + 60'000'000;
    struct {
        int frames = 0, keyframes = 0, decoded = 0, audioChunks = 0, gaps = 0;
        long long bytes = 0, audioBytes = 0;
        std::vector<double> latencies, decodedLatencies, audioAges;
    } st;
    uint64_t lastAudioEnd = 0;
    // A/V sync: flash (video) and beep (audio) times.
    std::vector<uint64_t> flashes, beeps;
    bool lastBright = false;
    uint64_t quietSince = 0;
    bool inBeep = false;
    std::vector<double> syncDiffs;

    std::vector<uint8_t> buffer(256 * 1024);
    for (;;) {
        const uint64_t t = now();
        if (o.duration > 0 && t - start > uint64_t(o.duration * 1e6)) break;
        if (t >= nextPing) {
            sendBytes(proto::pingMessage(t));
            ++pingsSent;
            nextPing = t + (pingsSent < 5 ? 100'000 : 1'000'000);
        }
        if (o.battery && t >= nextBattery) {
            sendBytes(proto::headsetStatusMessage(o.battery->battery, o.battery->flags));
            nextBattery = t + 60'000'000;
        }
        if (t >= nextStats) {
            const auto pct = [](std::vector<double>& v, double q) {
                if (v.empty()) return 0.0;
                std::sort(v.begin(), v.end());
                return v[std::min(v.size() - 1, size_t(q * v.size()))];
            };
            const auto avg = [](const std::vector<double>& v) {
                double sum = 0;
                for (double x : v) sum += x;
                return v.empty() ? 0.0 : sum / v.size();
            };
            std::string line = formatString("recv %d fps (%d key)", st.frames, st.keyframes);
            if (o.decode) line += formatString(" | decoded %d fps", st.decoded);
            line += formatString(" | %.2f Mbit/s", st.bytes * 8 / 1e6);
            if (offset) {
                line += formatString(" | capture->recv avg %.1f max %.1f ms", avg(st.latencies),
                                     st.latencies.empty() ? 0.0 : *std::max_element(st.latencies.begin(), st.latencies.end()));
                if (o.decode && !st.decodedLatencies.empty()) line += formatString(" | capture->decoded avg %.1f ms", avg(st.decodedLatencies));
                line += formatString(" | rtt %.2f ms", lastRtt / 1000.0);
            }
            if (config) line += formatString(" | %s %ux%u", proto::codecName(config->codec), config->width, config->height);
            if (audioConfig && st.audioChunks)
                line += formatString(" | audio %d chunks/s %.0f kbit/s, %d gaps, arrival-capture p50 %.1f p95 %.1f ms", st.audioChunks,
                                     st.audioBytes * 8 / 1000.0, st.gaps, pct(st.audioAges, 0.5), pct(st.audioAges, 0.95));
            std::printf("%s\n", line.c_str());
            st = {};
            nextStats += 1'000'000;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        timeval tv{0, 10'000};
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
        const int n = recv(s, reinterpret_cast<char*>(buffer.data()), int(buffer.size()), 0);
        if (n <= 0) {
            std::printf("connection closed (%s)\n", n == 0 ? "by the sender" : win32ErrorText(DWORD(WSAGetLastError())).c_str());
            break;
        }
        reader.append(buffer.data(), size_t(n));
        uint8_t type;
        proto::Bytes p;
        while (reader.next(type, p)) {
            const uint64_t arrival = now();
            switch (type) {
                case proto::kConfig: {
                    if (p.size() < 9) { std::printf("malformed CONFIG\n"); return 1; }
                    proto::ConfigMessage c;
                    c.codec = proto::Codec(p[0]);
                    c.width = getU32(&p[1]);
                    c.height = getU32(&p[5]);
                    c.parameterSets.assign(p.begin() + 9, p.end());
                    if (config && *config == c) break;
                    std::string types;
                    for (const auto& u : nal::split(c.parameterSets.data(), c.parameterSets.size()))
                        types += std::to_string(nal::type(c.codec, u)) + " ";
                    std::printf("CONFIG %s %ux%u, parameter sets %zu bytes (NAL types %s)\n", proto::codecName(c.codec), c.width,
                                c.height, c.parameterSets.size(), types.c_str());
                    config = c;
                    waitingKeyframe = true;
                    if (o.decode || !o.snapshot.empty()) {
                        std::string error;
                        decoderReady = decoder.configure(c, error);
                        if (!decoderReady) std::printf("decoder: %s\n", error.c_str());
                        else std::printf("decoder: %s\n", decoder.name().c_str());
                    }
                    break;
                }
                case proto::kFrame: {
                    if (p.size() < 9) { std::printf("malformed FRAME\n"); return 1; }
                    ++framesReceived;
                    const bool key = p[0] & 1;
                    const uint64_t capture = getU64(&p[1]);
                    proto::Bytes au(p.begin() + 9, p.end());
                    ++st.frames;
                    st.keyframes += key;
                    st.bytes += long long(p.size()) + 5;
                    if (offset) st.latencies.push_back((double(int64_t(arrival) + *offset) - double(capture)) / 1000.0);
                    if (dump) dump.write(reinterpret_cast<const char*>(au.data()), std::streamsize(au.size()));
                    if (key && config) {
                        // Protocol check: a keyframe starts with the CONFIG's parameter sets.
                        if (au.size() < config->parameterSets.size() ||
                            !std::equal(config->parameterSets.begin(), config->parameterSets.end(), au.begin()))
                            std::printf("WARNING: keyframe does not start with the CONFIG parameter sets\n");
                    }
                    if (waitingKeyframe && !key) std::printf("WARNING: first FRAME after CONFIG is not a keyframe\n");
                    if (key) waitingKeyframe = false;
                    if (decoderReady && (o.decode || !snapshotDone)) {
                        std::vector<DecodedPicture> pics;
                        std::string error;
                        if (!decoder.decode(au, capture, pics, error)) std::printf("decoder: %s\n", error.c_str());
                        for (auto& pic : pics) {
                            ++st.decoded;
                            if (offset) st.decodedLatencies.push_back((double(int64_t(now()) + *offset) - double(pic.micros)) / 1000.0);
                            if (!snapshotDone) {
                                std::string err;
                                if (writePng(pic, o.snapshot, err)) std::printf("snapshot: wrote %s (%dx%d, mean luma %.0f)\n", narrow(o.snapshot).c_str(), pic.width, pic.height, pic.meanLuma);
                                else std::printf("snapshot: %s\n", err.c_str());
                                snapshotDone = true;
                            }
                            const bool bright = pic.meanLuma > 200;
                            if (o.avSync && bright && !lastBright) flashes.push_back(pic.micros);
                            lastBright = bright;
                        }
                    }
                    if (o.ack) sendBytes(proto::frameAckMessage(framesReceived));
                    if (snapshotDone && o.exitAfterSnapshot && !o.snapshot.empty()) goto done;
                    break;
                }
                case proto::kControl: {
                    if (auto c = proto::ControlCommand::decode(p.data(), p.size())) std::printf("control: %s\n", c->describe().c_str());
                    break;
                }
                case proto::kAudioConfig: {
                    if (p.size() < 6) { std::printf("malformed AUDIO_CONFIG\n"); return 1; }
                    audioConfig = proto::AudioConfigMessage{p[0], getU32(&p[1]), p[5]};
                    std::printf("AUDIO_CONFIG format %u, %u Hz, %u ch\n", p[0], audioConfig->sampleRate, audioConfig->channels);
                    break;
                }
                case proto::kAudio: {
                    if (p.size() < 8 || !audioConfig) break;
                    const uint64_t capture = getU64(&p[0]);
                    const size_t bytes = p.size() - 8;
                    const size_t frames = bytes / (size_t(audioConfig->channels) * 2);
                    ++st.audioChunks;
                    st.audioBytes += long long(bytes);
                    if (lastAudioEnd && capture > lastAudioEnd + 1000) ++st.gaps;
                    lastAudioEnd = capture + uint64_t(frames * 1'000'000 / audioConfig->sampleRate);
                    if (offset) st.audioAges.push_back((double(int64_t(arrival) + *offset) - double(capture)) / 1000.0);
                    wav.write(&p[8], bytes, audioConfig->sampleRate, audioConfig->channels, capture);
                    if (o.avSync) {
                        const int16_t* samplesPtr = reinterpret_cast<const int16_t*>(&p[8]);
                        for (size_t f = 0; f < frames; ++f) {
                            const int v = std::abs(int(samplesPtr[f * audioConfig->channels]));
                            const uint64_t ts = capture + uint64_t(f * 1'000'000 / audioConfig->sampleRate);
                            if (v > 1000) {
                                if (!inBeep && ts - quietSince >= 100'000) beeps.push_back(ts);
                                inBeep = true;
                                quietSince = ts;
                            } else if (ts - quietSince > 5'000) {
                                inBeep = false;
                            }
                        }
                    }
                    break;
                }
                case proto::kPong: {
                    if (p.size() < 16) break;
                    const int64_t t0 = int64_t(getU64(&p[0])), ts = int64_t(getU64(&p[8])), t1 = int64_t(now());
                    const int64_t rtt = t1 - t0;
                    lastRtt = rtt;
                    samples.emplace_back(rtt, ts - (t0 + rtt / 2));
                    if (samples.size() > 16) samples.pop_front();
                    offset = std::min_element(samples.begin(), samples.end())->second;
                    break;
                }
                default: break;
            }
        }
        if (!reader.error().empty()) {
            std::printf("protocol error: %s\n", reader.error().c_str());
            break;
        }
        // Pair flashes with beeps (nearest within 500 ms).
        while (o.avSync && !flashes.empty() && !beeps.empty()) {
            const int64_t d = int64_t(beeps.front()) - int64_t(flashes.front());
            if (d < -500'000) { beeps.erase(beeps.begin()); continue; }
            if (d > 500'000) { flashes.erase(flashes.begin()); continue; }
            syncDiffs.push_back(d / 1000.0);
            std::printf("av-sync: flash at %llu, beep at %llu: audio - video = %+.3f ms\n", (unsigned long long)flashes.front(),
                        (unsigned long long)beeps.front(), d / 1000.0);
            flashes.erase(flashes.begin());
            beeps.erase(beeps.begin());
        }
    }
done:
    if (!syncDiffs.empty()) {
        double sum = 0;
        for (double d : syncDiffs) sum += d;
        std::printf("av-sync summary: %zu pairs, mean audio - video %+.3f ms\n", syncDiffs.size(), sum / syncDiffs.size());
    }
    wav.close();
    closesocket(s);
    MFShutdown();
    return 0;
}
