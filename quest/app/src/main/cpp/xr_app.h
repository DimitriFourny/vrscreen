#pragma once

#include <EGL/egl.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <string>

namespace qm {

class ScreenSource;

// Geometry of the mirrored-screen quad.
struct QuadConfig {
    int32_t surfaceWidth = 1920;  // swapchain/Surface pixel size; milestone 3 takes it from the stream
    int32_t surfaceHeight = 1080;
    float widthMeters = 1.6f;     // quad height follows the surface aspect ratio
    float distanceMeters = 1.2f;  // in front of the head when (re)centred
};

// OpenXR shell: instance/session lifecycle, passthrough underlay and a world-locked quad backed by
// an Android-surface swapchain whose Surface is handed to a ScreenSource.
// Everything runs on the android_main thread.
class XrApp {
public:
    XrApp(ANativeActivity* activity, JNIEnv* env, ScreenSource& source, const QuadConfig& quad);
    ~XrApp();
    XrApp(const XrApp&) = delete;
    XrApp& operator=(const XrApp&) = delete;

    bool initialize();
    void shutdown();

    // Drains the OpenXR event queue and drives the session state machine.
    void pollEvents();
    // One xrWaitFrame/xrBeginFrame/xrEndFrame iteration; only call while sessionRunning().
    void renderFrame();

    bool sessionRunning() const { return sessionRunning_; }
    // The runtime asked the app to exit (EXITING / LOSS_PENDING / instance loss).
    bool exitRequested() const { return exitRequested_; }

private:
    bool createInstance();
    bool initSystem();
    bool initEgl();
    bool createSession();
    bool createActions();
    bool createPassthrough();
    bool createQuadSwapchain();
    void destroyQuadSwapchain();
    void shutdownEgl();

    void handleSessionStateChanged(const XrEventDataSessionStateChanged& event);
    void updateVisibility();
    void logInteractionProfiles();
    void pollInput();
    void pollDebugProperties();
    bool recenter(XrTime time);
    void setFilterMode(int32_t mode, const char* reason);
    void cycleFilterMode();
    bool filterModeSupported(int32_t mode) const;
    std::string resultString(XrResult result) const;
    std::string pathString(XrPath path) const;

    ANativeActivity* activity_;
    JNIEnv* env_;
    ScreenSource& source_;
    QuadConfig quadConfig_;

    // Instance / system
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    bool hasLayerSettings_ = false;  // XR_FB_composition_layer_settings
    bool hasAutoFilter_ = false;     // XR_META_automatic_layer_filter
    bool hasImageLayout_ = false;    // XR_FB_composition_layer_image_layout
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetOpenGLESGraphicsRequirementsKHR = nullptr;
    PFN_xrCreateSwapchainAndroidSurfaceKHR pfnCreateSwapchainAndroidSurfaceKHR = nullptr;
    PFN_xrCreatePassthroughFB pfnCreatePassthroughFB = nullptr;
    PFN_xrDestroyPassthroughFB pfnDestroyPassthroughFB = nullptr;
    PFN_xrCreatePassthroughLayerFB pfnCreatePassthroughLayerFB = nullptr;
    PFN_xrDestroyPassthroughLayerFB pfnDestroyPassthroughLayerFB = nullptr;

    // EGL context for the XR_KHR_opengl_es_enable graphics binding (no GL rendering happens).
    EGLDisplay eglDisplay_ = EGL_NO_DISPLAY;
    EGLConfig eglConfig_ = nullptr;
    EGLContext eglContext_ = EGL_NO_CONTEXT;
    EGLSurface eglSurface_ = EGL_NO_SURFACE;

    // Session
    XrSession session_ = XR_NULL_HANDLE;
    XrSessionState sessionState_ = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning_ = false;
    bool exitRequested_ = false;
    bool visible_ = false;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;

    // Input: recenter on A/X, cycle the quad's layer filter on B/Y.
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction recenterAction_ = XR_NULL_HANDLE;
    XrAction filterAction_ = XR_NULL_HANDLE;

    // Passthrough
    XrPassthroughFB passthrough_ = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer_ = XR_NULL_HANDLE;

    // Quad
    XrSwapchain quadSwapchain_ = XR_NULL_HANDLE;
    jobject quadSurface_ = nullptr;  // global ref
    ANativeWindow* quadWindow_ = nullptr;
    XrPosef quadPose_{{0, 0, 0, 1}, {0, 0, 0}};
    bool quadPlaced_ = false;
    bool recenterRequested_ = true;  // place the quad on the first frame with a tracked head pose
    XrTime recenterAtTime_ = 0;      // pending XrEventDataReferenceSpaceChangePending::changeTime
    int32_t filterMode_ = 0;

    // Stats / debug controls
    uint64_t frameIndex_ = 0;
    uint64_t framesWithContent_ = 0;
    std::string lastFilterProp_;
    std::string lastRecenterProp_;
};

}  // namespace qm
