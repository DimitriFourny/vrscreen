#pragma once

#include "screen_source.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace qm {

// Milestone-2 ScreenSource: draws a sharpness test pattern on the CPU into the quad's Surface
// (ANativeWindow_lock / unlockAndPost, RGBA_8888) from its own thread at ~30 fps.
class TestPatternSource final : public ScreenSource {
public:
    TestPatternSource() = default;
    ~TestPatternSource() override;

    void onSurfaceReady(ANativeWindow* window, jobject surface, int32_t width, int32_t height) override;
    void onVisible(bool visible) override;
    void onSurfaceDestroyed() override;

private:
    void run();
    void stopThread();
    void renderBackground();
    void drawFrame(uint32_t* pixels, int32_t stride, double seconds, double fps);

    std::mutex mutex_;
    std::condition_variable cv_;
    bool visible_ = false;    // guarded by mutex_
    bool stop_ = false;       // guarded by mutex_
    bool producing_ = false;  // guarded by mutex_; a frame is being written to the window
    std::thread thread_;

    // Owned by the producer thread while it runs.
    ANativeWindow* window_ = nullptr;
    int32_t width_ = 0;
    int32_t height_ = 0;
    std::vector<uint32_t> background_;  // static part of the pattern, width_ x height_
    uint64_t framesPosted_ = 0;
};

}  // namespace qm
