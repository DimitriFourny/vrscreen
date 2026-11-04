#pragma once

#include <android/native_window.h>

#include <cstdint>
#include <string>

namespace qm {

// Text panel shown instead of the screen while there is no picture ("waiting for Mac...").
// Drawn on the CPU into its own Android-surface swapchain, so it never competes with
// MediaCodec for the screen quad's Surface. Used only on the XR thread, and only while the
// session is visible (no Surface writes after xrEndSession).
class StatusPanel {
public:
    static constexpr int32_t kWidth = 1280;
    static constexpr int32_t kHeight = 400;

    void attach(ANativeWindow* window);
    void detach() { window_ = nullptr; }
    // `text`: lines separated by '\n'; the first line is drawn as a heading.
    bool draw(const std::string& text);

private:
    ANativeWindow* window_ = nullptr;
};

}  // namespace qm
