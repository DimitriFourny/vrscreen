#pragma once

#include "audio_output.h"
#include "audio_playout.h"
#include "screen_source.h"
#include "stream_client.h"
#include "video_decoder.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

namespace qm {

// The real ScreenSource: receives the Mac's stream (StreamClient), decodes the video with
// MediaCodec straight into the screen quad's Surface (VideoDecoder) and plays the audio through
// AAudio (AudioPlayout + AudioOutput).
//
// Threads: XR thread (ScreenSource callbacks), network thread (StreamListener callbacks; feeds
// the decoder without queueing, pushes audio, owns the audio stream), decoder output thread
// (inside VideoDecoder), AAudio callback thread (inside AudioOutput).
// Lock order: decoderMutex_ before mutex_.
class MediaCodecSource final : public ScreenSource, private StreamListener {
public:
    // `filesDir`: app-private directory where the client remembers the last Wi-Fi sender.
    explicit MediaCodecSource(const std::string& filesDir);
    ~MediaCodecSource() override;

    // ScreenSource
    void attachHost(ScreenHost& host) override { host_ = &host; }
    void onSurfaceReady(ANativeWindow* window, jobject surface, int32_t width, int32_t height) override;
    void onVisible(bool visible) override;
    void onSurfaceDestroyed() override;
    bool pictureReady() const override;
    uint64_t statusGeneration() const override { return statusGeneration_.load(); }
    std::string statusText() const override;

    // Thread-safe: the latest battery reading, reported to the Mac (HEADSET_STATUS).
    void setHeadsetStatus(const HeadsetStatus& status) { client_.setHeadsetStatus(status); }

private:
    // StreamListener (network thread)
    void onSearching(const SearchState& state) override;
    void onConnected(const std::string& transport) override;
    bool onConfig(const StreamConfig& config) override;
    bool onFrame(const EncodedFrame& frame) override;
    void onControl(const ScreenCommand& command) override;
    void onAudioConfig(const wire::AudioFormat& format) override;
    void onAudio(int64_t captureMicros, const uint8_t* pcm, size_t bytes) override;
    void onDisconnected(const std::string& reason) override;
    bool onStatsTick(const LinkStats& stats) override;

    // Sets stopping_ (under mutex_) and wakes a network thread waiting for a Surface.
    void requestStop();

    // Starts the decoder for config_ on a Surface of the stream's size (asking XrApp to resize
    // the Surface first if needed). Network thread only.
    bool startDecoder(std::string* error);
    void stopDecoder();
    void setStatus(const std::string& text);

    ScreenHost* host_ = nullptr;
    StreamClient client_;

    // Surface state, guarded by mutex_.
    mutable std::mutex mutex_;
    std::condition_variable surfaceCv_;
    ANativeWindow* window_ = nullptr;
    int32_t surfaceWidth_ = 0;
    int32_t surfaceHeight_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};

    std::mutex decoderMutex_;
    VideoDecoder decoder_;

    // Audio (network thread; the playout's consumer side runs on the AAudio callback).
    AudioPlayout audioPlayout_;
    AudioOutput audioOutput_{audioPlayout_};
    wire::AudioFormat audioFormat_;

    // Network thread only.
    StreamConfig config_;
    bool waitingForKeyframe_ = true;
    std::string transport_;

    mutable std::mutex statusMutex_;
    std::string status_;
    std::atomic<uint64_t> statusGeneration_{0};
};

}  // namespace qm
