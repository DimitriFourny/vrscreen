#pragma once

#include <android/native_window.h>
#include <jni.h>

#include <cstdint>
#include <string>

namespace qm {

// Implemented by XrApp; lets a ScreenSource drive the quad's Surface.
class ScreenHost {
public:
    virtual ~ScreenHost() = default;

    // Thread-safe. Recreate the screen quad's Surface at this pixel size (the quad's aspect ratio
    // follows). XrApp does it on the XR thread: onSurfaceDestroyed for the old Surface, then
    // onSurfaceReady with the new one. No-op if the size is already current.
    virtual void requestSurfaceSize(int32_t width, int32_t height) = 0;
};

// Producer of the mirrored screen image: the seam between the XR shell and the content.
//
// XrApp owns an Android-surface swapchain (XR_KHR_android_surface_swapchain) that the compositor
// shows as the world-locked screen quad; whatever the source posts to its Surface appears there.
// A Surface has exactly one producer: the source (MediaCodec). Status text is shown by XrApp on a
// separate panel while pictureReady() is false.
//
// Lifecycle callbacks run on the XR thread. Each Surface gets onSurfaceReady ... onSurfaceDestroyed;
// onVisible(true|false) reports visibility changes independently of Surface (re)creation (a resize
// while visible swaps the Surface without onVisible calls). The first onSurfaceReady precedes the
// first onVisible. Callbacks return promptly; heavy work belongs on the source's own threads.
class ScreenSource {
public:
    virtual ~ScreenSource() = default;

    // Called once, before any other callback.
    virtual void attachHost(ScreenHost& host) = 0;

    // The quad's Surface exists. `window` and `surface` (a JNI global ref to the same
    // android.view.Surface) are owned by XrApp and stay valid until onSurfaceDestroyed returns.
    virtual void onSurfaceReady(ANativeWindow* window, jobject surface, int32_t width, int32_t height) = 0;

    // The session became visible (true: VISIBLE/FOCUSED) or stopped being visible (false). On
    // false, return only once nothing writes to the Surface any more: XrApp may call
    // xrEndSession right after (XR_KHR_android_surface_swapchain forbids writes past that point).
    virtual void onVisible(bool visible) = 0;

    // The swapchain is about to be destroyed. Stop writing to `window` before returning.
    virtual void onSurfaceDestroyed() = 0;

    // Thread-safe, polled every frame. True once a picture has been posted to the current
    // Surface and the stream is live; XrApp then shows the screen quad instead of the status panel.
    virtual bool pictureReady() const = 0;

    // Thread-safe status text for the panel (lines separated by '\n'). `generation` changes
    // whenever the text does, so callers can skip unchanged text cheaply.
    virtual uint64_t statusGeneration() const = 0;
    virtual std::string statusText() const = 0;
};

}  // namespace qm
