#include "video_decoder.h"

#include "log.h"

#include <media/NdkMediaFormat.h>
#include <sys/system_properties.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>

namespace qm {
namespace {

constexpr const char* kVendorLowLatency = "vendor.qti-ext-dec-low-latency.enable";

// Low-latency decoding is on unless debug.vrscreen.lowlatency is "0" (for A/B measurements).
int32_t lowLatencySetting() {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get("debug.vrscreen.lowlatency", value);
    return std::strcmp(value, "0") == 0 ? 0 : 1;
}

// Offset of the second Annex-B start code (00 00 00 01) in an SPS+PPS blob, or 0 if none.
size_t secondStartCode(const std::vector<uint8_t>& b) {
    for (size_t i = 4; i + 4 <= b.size(); ++i) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 && b[i + 3] == 1) return i;
    }
    return 0;
}

}  // namespace

bool VideoDecoder::start(const StreamConfig& config, ANativeWindow* window, std::string* error) {
    stop();
    const bool hevc = config.codec == Codec::HEVC;
    const char* mime = hevc ? "video/hevc" : "video/avc";
    const std::vector<uint8_t>& ps = config.parameterSets;
    const size_t split = hevc ? 0 : secondStartCode(ps);
    if (!hevc && split == 0) {
        *error = "H.264 CONFIG without separate SPS and PPS";
        return false;
    }

    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, config.width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, config.height);
    // Keyframes of a 4K desktop can reach several hundred KB; leave ample room.
    const int64_t inputSize = std::max<int64_t>(2 << 20, int64_t{config.width} * config.height / 2);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE, static_cast<int32_t>(inputSize));
    if (hevc) {
        AMediaFormat_setBuffer(format, "csd-0", ps.data(), ps.size());
    } else {
        AMediaFormat_setBuffer(format, "csd-0", ps.data(), split);
        AMediaFormat_setBuffer(format, "csd-1", ps.data() + split, ps.size() - split);
    }
    const int32_t lowLatencyRequested = lowLatencySetting();
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_LOW_LATENCY, lowLatencyRequested);
    AMediaFormat_setInt32(format, kVendorLowLatency, lowLatencyRequested);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_PRIORITY, 0);  // realtime

    // Qualcomm lists a dedicated low-latency AVC decoder (the HEVC one has no such variant); fall
    // back to the default decoder for the type if it is missing or rejects the format.
    media_status_t status = AMEDIA_ERROR_UNKNOWN;
    for (int attempt = hevc ? 1 : 0; attempt < 2 && codec_ == nullptr; ++attempt) {
        codec_ = attempt == 0 ? AMediaCodec_createCodecByName("c2.qti.avc.decoder.low_latency")
                              : AMediaCodec_createDecoderByType(mime);
        if (codec_ == nullptr) continue;
        char* name = nullptr;
        if (AMediaCodec_getName(codec_, &name) == AMEDIA_OK && name != nullptr) {
            name_ = name;
            AMediaCodec_releaseName(codec_, name);
        } else {
            name_ = mime;
        }
        status = AMediaCodec_configure(codec_, format, window, nullptr, 0);
        if (status != AMEDIA_OK) {
            LOGW("Decoder %s rejected %s %dx%d (%d)", name_.c_str(), mime, config.width, config.height, status);
            AMediaCodec_delete(codec_);
            codec_ = nullptr;
        }
    }
    AMediaFormat_delete(format);
    if (codec_ == nullptr) {
        *error = std::string("no decoder accepts ") + mime + " " + std::to_string(config.width) + "x" +
                 std::to_string(config.height) + " (" + std::to_string(status) + ")";
        return false;
    }

    // The input format echoes the keys the codec accepted.
    AMediaFormat* input = AMediaCodec_getInputFormat(codec_);
    int32_t lowLatency = -1, vendorLowLatency = -1, priority = -1;
    if (input != nullptr) {
        AMediaFormat_getInt32(input, AMEDIAFORMAT_KEY_LOW_LATENCY, &lowLatency);
        AMediaFormat_getInt32(input, kVendorLowLatency, &vendorLowLatency);
        AMediaFormat_getInt32(input, AMEDIAFORMAT_KEY_PRIORITY, &priority);
        AMediaFormat_delete(input);
    }
    LOGI("Decoder %s configured for %s %dx%d: requested low-latency=%d and %s=%d; codec reports low-latency=%d, "
         "%s=%d, priority=%d (-1 = not reported back)",
         name_.c_str(), mime, config.width, config.height, lowLatencyRequested, kVendorLowLatency, lowLatencyRequested,
         lowLatency, kVendorLowLatency, vendorLowLatency, priority);

    status = AMediaCodec_start(codec_);
    if (status != AMEDIA_OK) {
        *error = name_ + " start failed (" + std::to_string(status) + ")";
        stop();
        return false;
    }
    renderedTotal_ = 0;
    queuedTotal_ = 0;
    failed_ = false;
    takeStats();
    running_ = true;
    outputThread_ = std::thread(&VideoDecoder::outputLoop, this);
    return true;
}

void VideoDecoder::stop() {
    running_ = false;
    if (outputThread_.joinable()) outputThread_.join();
    if (codec_ != nullptr) {
        AMediaCodec_stop(codec_);
        AMediaCodec_delete(codec_);
        codec_ = nullptr;
        LOGI("Decoder %s released after %llu pictures", name_.c_str(),
             static_cast<unsigned long long>(renderedTotal_.load()));
    }
    renderedTotal_ = 0;
}

VideoDecoder::QueueResult VideoDecoder::queue(const EncodedFrame& frame, int64_t timeoutMicros) {
    if (!running_ || failed_) return QueueResult::Error;
    const ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, timeoutMicros);
    if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) return QueueResult::NoInputBuffer;
    if (index < 0) {
        LOGE("dequeueInputBuffer failed: %zd", index);
        return QueueResult::Error;
    }
    size_t capacity = 0;
    uint8_t* buffer = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(index), &capacity);
    if (buffer == nullptr || frame.size > capacity) {
        LOGE("Frame of %zu bytes does not fit the %zu-byte input buffer", frame.size, capacity);
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, 0, 0, 0);
        return QueueResult::Error;
    }
    std::memcpy(buffer, frame.data, frame.size);
    {
        std::lock_guard<std::mutex> lock(queuedMutex_);
        queued_[queuedNext_++ % queued_.size()] = {static_cast<int64_t>(frame.captureTimeMicros),
                                                   StreamClient::nowMicros(), frame.freshCapture};
    }
    const media_status_t status = AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(index), 0, frame.size,
                                                               frame.captureTimeMicros, 0);
    if (status != AMEDIA_OK) {
        LOGE("queueInputBuffer failed: %d", status);
        return QueueResult::Error;
    }
    ++queuedTotal_;
    return QueueResult::Queued;
}

VideoDecoder::Stats VideoDecoder::takeStats() {
    Stats s;
    s.rendered = statRendered_.exchange(0);
    s.latencySamples = statLatencySamples_.exchange(0);
    s.latencySumMicros = statLatencySum_.exchange(0);
    s.latencyMaxMicros = statLatencyMax_.exchange(INT64_MIN);
    std::lock_guard<std::mutex> lock(queuedMutex_);
    s.decodeSamples = statDecodeSamples_;
    s.decodeSumMicros = statDecodeSum_;
    s.decodeMaxMicros = statDecodeMax_;
    statDecodeSamples_ = 0;
    statDecodeSum_ = statDecodeMax_ = 0;
    return s;
}

void VideoDecoder::outputLoop() {
    pthread_setname_np(pthread_self(), "qm-decoder-out");
    setpriority(PRIO_PROCESS, gettid(), -10);
    while (running_) {
        AMediaCodecBufferInfo info{};
        const ssize_t index = AMediaCodec_dequeueOutputBuffer(codec_, &info, 10'000);
        if (index >= 0) {
            const int64_t now = StreamClient::nowMicros();
            // Present now, on the local clock. Plain releaseOutputBuffer(render=true) would stamp the
            // buffer with its PTS, i.e. the Mac's capture clock, as the desired present time; a
            // consumer that honours it could hold buffers that look up to a second in the future.
            AMediaCodec_releaseOutputBufferAtTime(codec_, static_cast<size_t>(index), now * 1000);
            if (renderedTotal_++ == 0) LOGI("Decoder %s: first picture rendered", name_.c_str());
            ++statRendered_;
            std::lock_guard<std::mutex> lock(queuedMutex_);
            for (QueuedInput& q : queued_) {
                if (q.pts != info.presentationTimeUs) continue;
                const int64_t decode = now - q.queuedAt;
                ++statDecodeSamples_;
                statDecodeSum_ += decode;
                statDecodeMax_ = std::max(statDecodeMax_, decode);
                if (q.freshCapture) {
                    // Re-encodes of an old capture would report the pause as latency: skip them.
                    const int64_t delta = now - info.presentationTimeUs;
                    ++statLatencySamples_;
                    statLatencySum_ += delta;
                    int64_t max = statLatencyMax_.load();
                    while (delta > max && !statLatencyMax_.compare_exchange_weak(max, delta)) {
                    }
                }
                q.pts = -1;
                break;
            }
        } else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
            LOGI("Decoder output format: %s", AMediaFormat_toString(format));
            AMediaFormat_delete(format);
        } else if (index != AMEDIACODEC_INFO_TRY_AGAIN_LATER && index != AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            // Not one of the informational codes: the codec is in an error state (e.g. reclaimed).
            // queue() now fails and the stats tick reconnects, which brings a fresh keyframe.
            LOGE("Decoder %s failed: dequeueOutputBuffer returned %zd", name_.c_str(), index);
            failed_ = true;
            return;
        }
    }
}

}  // namespace qm
