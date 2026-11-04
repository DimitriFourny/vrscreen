#include "mediacodec_source.h"

#include "log.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>

namespace qm {
namespace {

constexpr int64_t kQueueStallMicros = 1'000'000;
constexpr auto kSurfaceResizeTimeout = std::chrono::seconds(3);

std::string clip(const std::string& s, size_t max) { return s.size() <= max ? s : s.substr(0, max - 3) + "..."; }

std::string portOf(const std::string& endpoint) {
    const size_t colon = endpoint.rfind(':');
    return colon == std::string::npos ? endpoint : endpoint.substr(colon + 1);
}

}  // namespace

MediaCodecSource::MediaCodecSource() : client_(*this) { setStatus("Quest Mirror\nStarting..."); }

MediaCodecSource::~MediaCodecSource() {
    stopping_ = true;
    surfaceCv_.notify_all();
    client_.stop();
    stopDecoder();
}

void MediaCodecSource::onSurfaceReady(ANativeWindow* window, jobject /*surface*/, int32_t width, int32_t height) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        window_ = window;
        surfaceWidth_ = width;
        surfaceHeight_ = height;
    }
    surfaceCv_.notify_all();
}

void MediaCodecSource::onVisible(bool visible) {
    if (visible) {
        stopping_ = false;
        client_.start();
        LOGI("Stream: started (visible)");
        return;
    }
    // Synchronous: once this returns, the codec is released and nothing writes to the Surface.
    stopping_ = true;
    surfaceCv_.notify_all();
    client_.stop();
    stopDecoder();
    connected_ = false;
    LOGI("Stream: stopped (not visible)");
}

void MediaCodecSource::onSurfaceDestroyed() {
    std::lock_guard<std::mutex> decoderLock(decoderMutex_);
    decoder_.stop();
    std::lock_guard<std::mutex> lock(mutex_);
    window_ = nullptr;
}

bool MediaCodecSource::pictureReady() const {
    return connected_.load() && decoder_.running() && decoder_.renderedTotal() > 0;
}

std::string MediaCodecSource::statusText() const {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void MediaCodecSource::setStatus(const std::string& text) {
    {
        std::lock_guard<std::mutex> lock(statusMutex_);
        if (text == status_) return;
        status_ = text;
        ++statusGeneration_;
    }
    std::string oneLine = text;
    std::replace(oneLine.begin(), oneLine.end(), '\n', '|');
    LOGI("Status: %s", oneLine.c_str());
}

void MediaCodecSource::onConnecting(const std::string& endpoint, const std::string& lastError) {
    connected_ = false;
    endpoint_ = endpoint;
    std::string text = "Quest Mirror\nWaiting for the Mac on " + endpoint + " ...\nStart quest-mirror-sender on the Mac.\n" +
                       "USB: adb reverse tcp:" + portOf(endpoint) + " tcp:" + portOf(endpoint);
    if (!lastError.empty()) text += "\nLast try: " + clip(lastError, 38);
    setStatus(text);
}

void MediaCodecSource::onConnected(const std::string& endpoint) {
    connected_ = true;
    setStatus("Quest Mirror\nConnected to " + endpoint + "\nWaiting for video from the Mac...");
}

bool MediaCodecSource::onConfig(const StreamConfig& config) {
    if (config.payload == config_.payload && decoder_.running()) return true;  // identical CONFIG
    config_ = config;
    stopDecoder();
    std::string error;
    if (!startDecoder(&error)) {
        LOGE("Decoder setup failed: %s", error.c_str());
        return false;
    }
    char line[96];
    std::snprintf(line, sizeof(line), "%s %dx%d", config.codec == Codec::HEVC ? "HEVC" : "H.264", config.width,
                  config.height);
    setStatus("Quest Mirror\nConnected to " + endpoint_ + "\n" + line + " - waiting for a picture...");
    return true;
}

bool MediaCodecSource::startDecoder(std::string* error) {
    waitingForKeyframe_ = true;
    if (host_ == nullptr) {
        *error = "no host";
        return false;
    }
    const int32_t width = config_.width;
    const int32_t height = config_.height;
    host_->requestSurfaceSize(width, height);
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool ready = surfaceCv_.wait_for(lock, kSurfaceResizeTimeout, [&] {
            return stopping_.load() || (window_ != nullptr && surfaceWidth_ == width && surfaceHeight_ == height);
        });
        if (stopping_) {
            *error = "stopped";
            return false;
        }
        if (!ready) {
            *error = "timed out waiting for a " + std::to_string(width) + "x" + std::to_string(height) + " Surface";
            return false;
        }
    }
    // Holding decoderMutex_ keeps the window alive: onSurfaceDestroyed takes it first.
    std::lock_guard<std::mutex> decoderLock(decoderMutex_);
    ANativeWindow* window = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (window_ != nullptr && surfaceWidth_ == width && surfaceHeight_ == height) window = window_;
    }
    if (window == nullptr) {
        *error = "Surface went away";
        return false;
    }
    return decoder_.start(config_, window, error);
}

void MediaCodecSource::stopDecoder() {
    std::lock_guard<std::mutex> lock(decoderMutex_);
    decoder_.stop();
}

bool MediaCodecSource::onFrame(const EncodedFrame& frame) {
    if (!config_.valid()) return true;  // no CONFIG yet: discard
    if (!decoder_.running()) {
        // The Surface was recreated under us; restart on the new one from the next keyframe.
        std::string error;
        if (!startDecoder(&error)) {
            LOGE("Decoder restart failed: %s", error.c_str());
            return false;
        }
    }
    if (waitingForKeyframe_) {
        if (!frame.keyframe) return true;  // discard until a decoder entry point
        waitingForKeyframe_ = false;
    }
    const int64_t deadline = StreamClient::nowMicros() + kQueueStallMicros;
    while (!stopping_) {
        VideoDecoder::QueueResult result;
        {
            std::lock_guard<std::mutex> lock(decoderMutex_);
            result = decoder_.running() ? decoder_.queue(frame, 10'000) : VideoDecoder::QueueResult::Error;
        }
        if (result == VideoDecoder::QueueResult::Queued) return true;
        if (result == VideoDecoder::QueueResult::Error) return false;
        if (StreamClient::nowMicros() > deadline) {
            LOGE("Decoder accepted no input for 1 s");
            return false;
        }
    }
    return true;
}

void MediaCodecSource::onDisconnected(const std::string& reason) {
    (void)reason;
    connected_ = false;
    stopDecoder();
    config_ = StreamConfig{};
}

void MediaCodecSource::onStatsTick(const LinkStats& stats) {
    const VideoDecoder::Stats d = decoder_.takeStats();
    int64_t offset = 0;
    const bool hasOffset = client_.clockOffset(&offset);
    char latency[96] = "n/a";
    if (d.latencySamples > 0 && hasOffset) {
        const double avg = (static_cast<double>(d.latencySumMicros) / static_cast<double>(d.latencySamples) +
                            static_cast<double>(offset)) / 1000.0;
        const double max = static_cast<double>(d.latencyMaxMicros + offset) / 1000.0;
        std::snprintf(latency, sizeof(latency), "avg %.1f ms max %.1f ms", avg, max);
    }
    char decode[64] = "n/a";
    if (d.decodeSamples > 0) {
        std::snprintf(decode, sizeof(decode), "avg %.1f ms max %.1f ms",
                      static_cast<double>(d.decodeSumMicros) / static_cast<double>(d.decodeSamples) / 1000.0,
                      static_cast<double>(d.decodeMaxMicros) / 1000.0);
    }
    const uint64_t queued = decoder_.queuedTotal();
    const uint64_t rendered = decoder_.renderedTotal();
    LOGI("Stats: recv %.1f fps, decoded %.1f fps, %.2f Mbit/s, capture->decoded %s, decode %s, rtt %.2f ms, "
         "in decoder %" PRIu64 ", %s %dx%d",
         static_cast<double>(stats.frames) / stats.seconds, static_cast<double>(d.rendered) / stats.seconds,
         static_cast<double>(stats.bytes) * 8.0 / stats.seconds / 1e6, latency, decode,
         stats.rttMicros >= 0 ? static_cast<double>(stats.rttMicros) / 1000.0 : -1.0,
         queued > rendered ? queued - rendered : 0, decoder_.running() ? decoder_.name().c_str() : "no decoder",
         config_.width, config_.height);
}

}  // namespace qm
