#include "pipeline.h"

#include <avrt.h>
#include <objbase.h>

#include "log.h"
#include "util.h"

namespace vr {

SenderPipeline::SenderPipeline(EventLoop& loop, MirrorServer& server, EncoderSettings settings)
    : loop_(loop), server_(server), encoder_(std::make_unique<VideoEncoder>(std::move(settings))) {
    server_.onClientReady = [this](const std::shared_ptr<ClientSession>& client) {
        client->needsKeyframe = true;
        pump();
    };
    server_.onFrameAck = [this](const std::shared_ptr<ClientSession>&) { pump(); };
    lastStatsMicros_ = nowMicros();
    thread_ = std::thread([this] { runEncoderThread(); });
}

SenderPipeline::~SenderPipeline() { stop(); }

void SenderPipeline::stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void SenderPipeline::postJob(std::function<void(VideoEncoder&)> job) {
    {
        std::lock_guard lock(mutex_);
        jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
}

void SenderPipeline::runEncoderThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    for (;;) {
        std::function<void(VideoEncoder&)> job;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !jobs_.empty(); });
            if (stopping_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        job(*encoder_);
    }
    encoder_->shutdown();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}

void SenderPipeline::handleCapture(VideoFrame frame) {
    if (server_.activeClient() && latest_ && latest_->sequence > lastEncodedSequence_) ++stats_.skipped;
    ++captureSequence_;
    // Session setup takes 50-300 ms: do it before a client needs it.
    if (frame.gpu.get() != preparedGpu_ || frame.width != preparedW_ || frame.height != preparedH_) {
        preparedGpu_ = frame.gpu.get();
        preparedW_ = frame.width;
        preparedH_ = frame.height;
        VideoFrame copy = frame;
        postJob([copy](VideoEncoder& encoder) {
            std::string error;
            encoder.prepare(copy, error);  // a failure resurfaces (and is reported) on the next encode
        });
    }
    latest_ = Capture{std::move(frame), captureSequence_};
    ++stats_.captured;
    pump();
}

void SenderPipeline::pump() {
    const auto client = server_.activeClient();
    if (!client || !latest_) return;
    if (!(latest_->sequence > lastEncodedSequence_ || client->needsKeyframe)) return;
    if (client->framesUnsent >= kMaxUnsentFrames || !client->ackWindow.open() || framesEncoding_ >= kMaxEncodingFrames)
        return;
    const bool forceKeyframe = client->needsKeyframe;
    client->needsKeyframe = false;
    lastEncodedSequence_ = latest_->sequence;
    ++framesEncoding_;
    VideoFrame frame = latest_->frame;
    postJob([this, frame, forceKeyframe, client](VideoEncoder& encoder) {
        const uint64_t start = nowMicros();
        EncodeResult result;
        result.ok = encoder.encode(frame, forceKeyframe, result.output, result.error);
        const double ms = (nowMicros() - start) / 1000.0;
        loop_.post([this, result = std::move(result), client, ms]() mutable {
            stats_.encodeMsTotal += ms;
            stats_.encodeMsMax = std::max(stats_.encodeMsMax, ms);
            handleEncoded(std::move(result), client);
        });
    });
}

void SenderPipeline::replaceEncoder(EncoderSettings settings) {
    postJob([this, settings](VideoEncoder&) {
        encoder_->shutdown();  // flushed: its last outputs already went out in order
        encoder_ = std::make_unique<VideoEncoder>(settings);
    });
    preparedGpu_ = nullptr;  // prepare the new one on the next capture
    server_.cachedConfig.reset();
    if (server_.activeClient()) server_.activeClient()->needsKeyframe = true;
    pump();
}

void SenderPipeline::setEncoderError(const std::string* error) {
    const bool same = error ? (encoderError_ && *encoderError_ == *error) : !encoderError_;
    if (same) return;
    if (error) encoderError_ = *error;
    else encoderError_.reset();
    if (onEncoderError) onEncoderError(error);
}

void SenderPipeline::handleEncoded(EncodeResult result, const std::shared_ptr<ClientSession>& client) {
    --framesEncoding_;
    if (!result.ok) {
        log("encoder: " + result.error);
        setEncoderError(&result.error);
        client->needsKeyframe = true;
        // Retry with a fresh session on the next capture (or now, for a static screen) after a pause.
        preparedGpu_ = nullptr;
        postJob([](VideoEncoder& encoder) { encoder.shutdown(); });
        loop_.postDelayed(std::chrono::milliseconds(1000), [this] { pump(); });
        return;
    }
    if (result.output) {
        setEncoderError(nullptr);
        EncodedOutput& out = *result.output;
        ++stats_.encoded;
        if (out.frame.keyframe) ++stats_.keyframes;
        server_.cachedConfig = out.config;
        if (client->active) {
            if (client->sentConfig != out.config) {
                if (client->sentConfig)
                    logf("server: stream format changed to %s %ux%u, sending CONFIG", proto::codecName(out.config.codec),
                         out.config.width, out.config.height);
                server_.send(client, out.config.encoded());
                client->sentConfig = out.config;
            }
            if (!out.frame.keyframe && client->ackWindow.framesSent() == 0) {
                // A client's first FRAME must be a keyframe (the encoder ignored the request).
                client->needsKeyframe = true;
            } else {
                ++client->framesUnsent;
                server_.send(client, out.frame.encoded(), [this, client] {
                    --client->framesUnsent;
                    pump();
                }, true);
                stats_.maxInFlight = std::max(stats_.maxInFlight, client->ackWindow.inFlight());
            }
        }
    }
    pump();
}

void SenderPipeline::logStats(const std::string& audioText) {
    const uint64_t now = nowMicros();
    const double dt = std::max(1e-3, (now - lastStatsMicros_) / 1e6);
    lastStatsMicros_ = now;
    const long long bytes = server_.bytesSent() - lastBytes_;
    lastBytes_ = server_.bytesSent();
    totalSkipped_ += stats_.skipped;
    std::string clientText = "no client";
    if (const auto& c = server_.activeClient()) {
        clientText = formatString("client %s | backlog %d frames %zu KiB", c->address.c_str(), c->framesUnsent,
                                  c->unsentBytes() / 1024);
        if (c->ackWindow.active())
            clientText += formatString(" | in flight %u/%u (max %u)", c->ackWindow.inFlight(), c->ackWindow.limit(),
                                       stats_.maxInFlight);
    }
    std::string encodeText;
    if (stats_.encoded > 0)
        encodeText = formatString(" | encode avg %.1f max %.1f ms", stats_.encodeMsTotal / stats_.encoded, stats_.encodeMsMax);
    logf("capture %.1f fps | encoded %.1f fps (%d key)%s | skipped %d (total %lld) | %.2f Mbit/s | %s | %s",
         stats_.captured / dt, stats_.encoded / dt, stats_.keyframes, encodeText.c_str(), stats_.skipped, totalSkipped_,
         bytes * 8 / dt / 1e6, audioText.c_str(), clientText.c_str());
    stats_ = Stats{};
}

}  // namespace vr
