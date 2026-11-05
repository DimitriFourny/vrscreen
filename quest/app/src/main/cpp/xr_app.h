#pragma once

#include "boundary_suppression.h"
#include "dim_controller.h"
#include "grab.h"
#include "hand_gestures.h"
#include "hand_tracker.h"
#include "screen_source.h"
#include "status_panel.h"
#include "walking_detector.h"

#include <EGL/egl.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <jni.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace qm {

// Where the screen is, in metres, relative to the head at the last recenter or grab release.
// Adjusted with the thumbstick and by grabbing; persisted (LOCAL space resets every session, so an
// absolute pose would be meaningless at the next launch).
struct Placement {
    float distance = 1.2f;      // from the eyes to the screen centre
    float width = 1.8f;         // quad height follows the stream's aspect ratio
    float heightOffset = 0.0f;  // screen centre above (+) or below (-) eye height
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
    void screenCommand(const ScreenCommand& command) override;

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
    void pollInput(double dtSeconds, XrTime time);
    // Runs the grabbers (controller grips, hand gestures, debug simulations) and moves the screen
    // while held. One grabber at a time: the first wins.
    void updateGrabs(double dtSeconds, XrTime time);
    // Hand samples for this frame: the debug.vrscreen.simpinch script while it runs, else tracking.
    void readHands(double dtSeconds, XrTime time, HandSample hands[2]);
    bool screenGrabbed() const;
    void onGrabReleased(const char* who);
    XrVector3f headUp() const;
    // Puts the screen centre at `position`, re-deriving direction/distance/height offset from the
    // anchor; `faceAnchor` turns it towards the anchor, otherwise its orientation is kept.
    void moveScreenTo(const XrVector3f& position, bool faceAnchor);
    // Logs the pending stream of continuous Mac CONTROL commands as one before -> after line.
    void flushControlBurstLog();
    void pollDebugProperties();
    bool recenter(XrTime time);
    // Screen in front of `head` (yaw only) at the current distance and height offset.
    void placeInFront(const XrPosef& head);
    void updateQuadPose();
    void applyScreenCommands();
    // Sets the passthrough layer's opacity (1 = normal, 0 = black surroundings) if it changed.
    void setPassthroughOpacity(float opacity);
    void setPlacement(float distance, float width, const char* reason);
    void loadPlacement();
    void savePlacement();
    void setFilterMode(int32_t mode, const char* reason);
    void startSimulatedGrab(const std::string& spec);
    void startSimulatedPinch(const std::string& spec);
    // Fills the highlight frame's Surface with the accent colour (once).
    bool drawHighlight();
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
    bool hasHandTracking_ = false;   // XR_EXT_hand_tracking
    bool hasHandTrackingAim_ = false;  // XR_FB_hand_tracking_aim
    bool hasBoundaryVisibility_ = false;  // XR_META_boundary_visibility, supported by the system
    PFN_xrRequestBoundaryVisibilityMETA pfnRequestBoundaryVisibilityMETA = nullptr;
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

    // Input: A/X recenter, grips grab the screen, right thumbstick distance (Y) and size (X).
    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction recenterAction_ = XR_NULL_HANDLE;
    XrAction grabAction_ = XR_NULL_HANDLE;      // squeeze, per hand
    XrAction gripPoseAction_ = XR_NULL_HANDLE;  // grip pose, per hand
    XrAction adjustAction_ = XR_NULL_HANDLE;
    XrPath handPaths_[2] = {XR_NULL_PATH, XR_NULL_PATH};  // left, right
    XrSpace gripSpaces_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool focused_ = false;  // input is only read while FOCUSED

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
    std::mutex commandMutex_;
    std::vector<ScreenCommand> commands_;  // from the Mac (network thread), guarded by commandMutex_

    // Presentation, set by the Mac (re-sent on every connect, never persisted here).
    bool hasCylinder_ = false;  // XR_KHR_composition_layer_cylinder enabled
    float curvature_ = 0.0f;    // 0 flat .. 1 cylinder centred on the viewer
    DimController dimmer_;         // wanted dim (SET_DIM) vs. what is shown (only with a picture)
    WalkingDetector walking_;      // lifts the dim while the user walks around (boundaryless app)
    float appliedOpacity_ = 1.0f;  // passthrough style currently set
    float dimGamma_ = kDimGamma;   // debug.vrscreen.dimgamma overrides it for tuning
    PFN_xrPassthroughLayerSetStyleFB pfnPassthroughLayerSetStyleFB = nullptr;
    // Hiding the Guardian boundary (passthrough-only app): when to request it, dim floor.
    BoundarySuppression boundary_;

    // Placement
    Placement placement_;
    std::string placementPath_;
    bool placementDirty_ = false;
    double placementIdleSeconds_ = 0;
    XrVector3f anchorPosition_{0, 0, 0};    // head position at the last recenter / grab release
    XrVector3f anchorDirection_{0, 0, -1};  // unit vector from the anchor to the screen centre
    XrPosef quadPose_{{0, 0, 0, 1}, {0, 0, 0}};
    bool quadPlaced_ = false;
    bool recenterRequested_ = true;  // place the quad on the first frame with a tracked head pose
    XrTime recenterAtTime_ = 0;      // pending XrEventDataReferenceSpaceChangePending::changeTime
    XrPosef headPose_{{0, 0, 0, 1}, {0, 0, 0}};  // this frame's head pose in LOCAL space
    bool headValid_ = false;

    // Grabbing: left grip, right grip, and the debug.vrscreen.simgrab simulation.
    Grab grabs_[3];
    int simGrabStep_ = 0;  // > 0 while a simulated grab runs (frames left)
    XrPosef simGrabStart_{{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef simGrabEnd_{{0, 0, 0, 1}, {0, 0, 0}};

    // Hand gestures (SET_HAND_GESTURES from the Mac; off until it arrives, never persisted here).
    HandTracker handTracker_;
    HandGestures handGestures_;
    bool handGesturesEnabled_ = false;
    bool handTracked_[2] = {false, false};  // for logging changes
    double handsUnseenSeconds_ = 0;         // enabled, focused and no hand tracked: hint once
    bool handsHintLogged_ = false;
    // debug.vrscreen.simpinch: scripted pinch through HandGestures (see startSimulatedPinch).
    double simPinchElapsed_ = -1;  // < 0: not running
    double simPinchSeconds_ = 0;
    int simPinchHands_ = 1;
    XrVector3f simPinchMove_{0, 0, 0};
    float simPinchSpread_ = 0;
    float simPinchPitch_ = 0;  // radians
    XrPosef simPinchHead_{{0, 0, 0, 1}, {0, 0, 0}};

    // Highlight frame behind the screen while it is grabbed (solid accent colour; not submitted
    // otherwise).
    SurfaceSwapchain highlight_;
    bool highlightDrawn_ = false;
    bool highlightLogged_ = false;
    int32_t filterMode_ = 0;

    // Stats / debug controls
    uint64_t frameIndex_ = 0;
    uint64_t framesWithPicture_ = 0;
    XrTime lastDisplayTime_ = 0;
    std::string lastFilterProp_;
    std::string lastRecenterProp_;
    std::string lastDistanceProp_;
    std::string lastWidthProp_;
    std::string lastSimGrabProp_;
    std::string lastSimPinchProp_;
    std::string lastDimGammaProp_;
    uint32_t controlBurstCount_ = 0;  // continuous Mac CONTROL commands since the last save
    XrPosef controlBurstStart_{{0, 0, 0, 1}, {0, 0, 0}};
    ScreenCommand::Kind controlBurstKind_ = ScreenCommand::Kind::Orbit;
    const char* controlBurstName_ = "";
};

}  // namespace qm
