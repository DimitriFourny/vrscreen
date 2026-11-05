// Video sources. Both run on their own thread and hand NV12 frames (GPU textures, already scaled
// to fit the maximum width) to `onFrame`:
// - ScreenCapture: DXGI Desktop Duplication of one display, with the mouse pointer drawn in.
//   Frames come only when the screen changes, at most `fps` per second; the newest picture of a
//   burst is always delivered (a skipped one is sent when its slot comes).
// - TestPattern: a synthetic pattern (moving bar, white flash once per second) and, through
//   `onAudio`, silence with a 60 ms 1 kHz beep at each flash, for A/V checks without a screen.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "cursor.h"
#include "d3d.h"

namespace vr {

struct CaptureSettings {
    std::string display;              // GDI device name; empty = primary
    std::optional<int> displayIndex;  // --display N, wins over `display`
    int maxWidth = 3840;
    int fps = 60;
    bool operator==(const CaptureSettings&) const = default;
};

struct CaptureStatus {
    enum class Kind { Running, Failed } kind = Kind::Running;
    std::string message;     // Failed: why
    std::string deviceName;  // Running: the captured display
};

class VideoSource {
public:
    virtual ~VideoSource() = default;
    std::function<void(VideoFrame)> onFrame;
    std::function<void(const CaptureStatus&)> onStatus;
    // Synthesized audio (test pattern only): interleaved stereo float, first sample's time.
    std::function<void(const float* samples, int frames, uint64_t micros)> onAudio;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual void update(const CaptureSettings& settings) = 0;
};

class ScreenCapture : public VideoSource {
public:
    explicit ScreenCapture(CaptureSettings settings) : settings_(std::move(settings)) {}
    ~ScreenCapture() override { stop(); }
    void start() override;
    void stop() override;
    void update(const CaptureSettings& settings) override;

private:
    void run();
    bool open(std::string& error);
    void close();
    bool deliver(uint64_t captureMicros);
    void report(const CaptureStatus& status);

    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    CaptureSettings settings_;     // guarded by mutex_
    bool settingsChanged_ = false;  // guarded by mutex_

    // Capture thread state.
    CaptureSettings active_;
    std::shared_ptr<GpuDevice> gpu_;
    ComPtr<IDXGIOutputDuplication> dup_;
    std::string deviceName_;
    DXGI_MODE_ROTATION rotation_ = DXGI_MODE_ROTATION_IDENTITY;
    int imageW_ = 0, imageH_ = 0;      // duplicated image (unrotated)
    int desktopW_ = 0, desktopH_ = 0;  // rotated
    int outW_ = 0, outH_ = 0;
    ComPtr<ID3D11Texture2D> desktop_;
    ComPtr<ID3D11ShaderResourceView> desktopView_;
    ComPtr<ID3D11Texture2D> composite_;
    ComPtr<ID3D11RenderTargetView> compositeTarget_;
    Compositor compositor_;
    CursorRenderer cursor_;
    VideoConverter converter_;
    std::shared_ptr<TexturePool> pool_;
    std::vector<uint8_t> pointerBuffer_;
    std::string lastReported_;
    bool everRunning_ = false;
};

class TestPattern : public VideoSource {
public:
    TestPattern(int width, int height, CaptureSettings settings) : width_(width), height_(height), settings_(std::move(settings)) {}
    ~TestPattern() override { stop(); }
    void start() override;
    void stop() override;
    void update(const CaptureSettings& settings) override;

private:
    void run();
    int width_, height_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    CaptureSettings settings_;
};

}  // namespace vr
