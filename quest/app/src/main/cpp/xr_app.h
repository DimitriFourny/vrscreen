#pragma once

#include "screen_source.h"
#include "status_panel.h"

#include <EGL/egl.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace qm {

// Where the screen is, in metres. Adjusted with the right thumbstick and persisted.
struct Placement {
    float distance = 1.2f;  // from the head at the last recenter, along its gaze (yaw only)
    float width = 1.8f;     // quad height follows the stream's aspect ratio
};

// OpenXR shell: instance/session lifecycle, passthrough underlay, a world-locked screen quad backed
// by an Android-surface swapchain whose Surface is handed to a ScreenSource, and a status panel
// (its own Android-surface swapchain, CPU-drawn) shown while the source has no picture.
// Everything runs on the android_main thread except requestSurfaceSize().
class XrApp final : public ScreenHost {
public:
    XrApp(ANativeActivity* activity, JNIEnv* env, ScreenSource& source);
    ~XrApp() override;
    XrApp(const XrApp&) = delete;
    XrApp& operator=(const XrApp&) = delete;

    bool initialize();
    void shutdown();

    // Drains the OpenXR event queue, drives the session state machine and applies Surface resizes.
    void pollEvents();
    // One xrWaitFrame/xrBeginFrame/xrEndFrame iteration; only call while sessionRunning().
    void renderFrame();

    bool sessionRunning() const { return sessionRunning_; }
    // The runtime asked the app to exit (EXITING / LOSS_PENDING / instance loss).
    bool exitRequested() const { return exitRequested_; }

    // ScreenHost
    void requestSurfaceSize(int32_t width, int32_t height) override;

private:
    struct SurfaceSwapchain {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        jobject surface = nullptr;  // global ref
        ANativeWindow* window = nullptr;
        int32_t width = 0;
        int32_t height = 0;
    };

    bool createInstance();
    bool initSystem();
    bool initEgl();
    bool createSession();
    bool createActions();
    bool createPassthrough();
    bool createSurfaceSwapchain(int32_t width, int32_t height, SurfaceSwapchain* out);
    void destroySurfaceSwapchain(SurfaceSwapchain* sc);
    bool createScreenSwapchain(int32_t width, int32_t height);
    void destroyScreenSwapchain();
    void applySurfaceResize();
    void shutdownEgl();

    void handleSessionStateChanged(const XrEventDataSessionStateChanged& event);
    void updateVisibility();
    void logInteractionProfiles();
    void pollInput(double dtSeconds);
    void pollDebugProperties();
    bool recenter(XrTime time);
    void updateQuadPose();
    void setPlacement(float distance, float width, const char* reason);
    void loadPlacement();
    void savePlacement();
    void setFilterMode(int32_t mode, const char* reason);
    void cycleFilterMode();
    bool filterModeSupported(int32_t mode) const;
    std::string resultString(XrResult result) const;
    std::string pathString(XrPath path) const;

    ANativeActivity* activity_;
    JNIEnv* env_;
    ScreenSource& source_;

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

    // Input: A/X recenter, B/Y cycle the layer filter, right thumbstick distance (Y) and size (X).
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction recenterAction_ = XR_NULL_HANDLE;
    XrAction filterAction_ = XR_NULL_HANDLE;
    XrAction adjustAction_ = XR_NULL_HANDLE;

    // Passthrough
    XrPassthroughFB passthrough_ = XR_NULL_HANDLE;
    XrPassthroughLayerFB passthroughLayer_ = XR_NULL_HANDLE;

    // Screen quad (MediaCodec's Surface) and status panel.
    SurfaceSwapchain screen_;
    SurfaceSwapchain status_;
    StatusPanel statusPanel_;
    uint64_t statusDrawnGeneration_ = 0;
    bool statusDrawn_ = false;
    std::mutex resizeMutex_;
    int32_t requestedWidth_ = 0;  // guarded by resizeMutex_
    int32_t requestedHeight_ = 0;

    // Placement
    Placement placement_;
    std::string placementPath_;
    bool placementDirty_ = false;
    double placementIdleSeconds_ = 0;
    XrVector3f anchorPosition_{0, 0, 0};  // head position at the last recenter
    float anchorForwardX_ = 0.0f;         // horizontal gaze direction at the last recenter
    float anchorForwardZ_ = -1.0f;
    XrPosef quadPose_{{0, 0, 0, 1}, {0, 0, 0}};
    bool quadPlaced_ = false;
    bool recenterRequested_ = true;  // place the quad on the first frame with a tracked head pose
    XrTime recenterAtTime_ = 0;      // pending XrEventDataReferenceSpaceChangePending::changeTime
    int32_t filterMode_ = 0;

    // Stats / debug controls
    uint64_t frameIndex_ = 0;
    uint64_t framesWithPicture_ = 0;
    XrTime lastDisplayTime_ = 0;
    std::string lastFilterProp_;
    std::string lastRecenterProp_;
    std::string lastDistanceProp_;
    std::string lastWidthProp_;
};

}  // namespace qm
