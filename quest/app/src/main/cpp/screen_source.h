#pragma once

#include <android/native_window.h>
#include <jni.h>

#include <cstdint>

namespace qm {

// Producer of the mirrored screen image: the seam between the XR shell and the content.
//
// XrApp owns an Android-surface swapchain (XR_KHR_android_surface_swapchain) that the compositor
// shows as a world-locked quad. Whatever a ScreenSource posts to that swapchain's Surface appears
// on the quad. Milestone 2 plugs in TestPatternSource (CPU ANativeWindow_lock/unlockAndPost);
// milestone 3 plugs in the network receiver + AMediaCodec decoding straight into `window`.
//
// All callbacks are invoked on the XR (android_main) thread, per surface in this order:
//   onSurfaceReady -> onVisible(true|false)* -> onSurfaceDestroyed
// Implementations must return promptly; heavy work belongs on their own threads.
class ScreenSource {
public:
    virtual ~ScreenSource() = default;

    // The quad's Surface exists. `window` and `surface` (a JNI global ref to the same
    // android.view.Surface, e.g. for MediaCodec.configure) are owned by XrApp and stay valid until
    // onSurfaceDestroyed returns; ANativeWindow_acquire() the window to keep it longer.
    // width/height are the swapchain's pixel size; the compositor scales it onto the quad.
    virtual void onSurfaceReady(ANativeWindow* window, jobject surface, int32_t width, int32_t height) = 0;

    // The session became visible (true: VISIBLE/FOCUSED) or stopped being visible (false). While
    // not visible the compositor does not consume the Surface, so stop producing frames. On false,
    // return only once nothing is writing to the Surface: XrApp may call xrEndSession right after
    // (XR_KHR_android_surface_swapchain forbids Surface writes past that point).
    virtual void onVisible(bool visible) = 0;

    // The swapchain is about to be destroyed. Stop producing and stop touching `window`/`surface`
    // before returning.
    virtual void onSurfaceDestroyed() = 0;
};

}  // namespace qm
