#include "xr_app.h"

#include "log.h"
#include "screen_source.h"
#include "xr_math.h"

#include <GLES3/gl3.h>
#include <android/native_window_jni.h>
#include <sys/stat.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace qm {
namespace {

#define XR_CHECK(call)                                                       \
    do {                                                                     \
        const XrResult xrCheckResult = (call);                               \
        if (XR_FAILED(xrCheckResult)) {                                      \
            LOGE("%s failed: %s", #call, resultString(xrCheckResult).c_str()); \
            return false;                                                    \
        }                                                                    \
    } while (0)

// Quad filtering via XR_FB_composition_layer_settings, cycled with B/Y or debug.questmirror.filter.
struct FilterMode {
    const char* name;
    XrCompositionLayerSettingsFlagsFB flags;
};
constexpr FilterMode kFilterModes[] = {
    {"none", 0},
    {"sharpen-normal", XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SHARPENING_BIT_FB},
    {"sharpen-quality", XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB},
    {"supersample-normal", XR_COMPOSITION_LAYER_SETTINGS_NORMAL_SUPER_SAMPLING_BIT_FB},
    {"supersample-quality", XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB},
    {"supersample+sharpen-quality",
     XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB |
         XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB},
    {"auto(supersample+sharpen-quality)",
     XR_COMPOSITION_LAYER_SETTINGS_AUTO_LAYER_FILTER_BIT_META |
         XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB |
         XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB},
};
constexpr int32_t kFilterModeCount = static_cast<int32_t>(sizeof(kFilterModes) / sizeof(kFilterModes[0]));
constexpr int32_t kAutoFilterMode = 6;
constexpr int32_t kDefaultFilterMode = 2;  // sharpen-quality

constexpr const char* kFilterProperty = "debug.questmirror.filter";
constexpr const char* kRecenterProperty = "debug.questmirror.recenter";
constexpr const char* kDistanceProperty = "debug.questmirror.distance";
constexpr const char* kWidthProperty = "debug.questmirror.width";
// "dx dy dz [yawDegrees]": simulate a grab that starts at the head pose and moves the grabber by
// (dx, dy, dz) metres in LOCAL space, turned by yaw, then releases (exercises Grab without buttons).
constexpr const char* kSimGrabProperty = "debug.questmirror.simgrab";

// Initial screen Surface size; the stream's CONFIG resizes it.
constexpr int32_t kInitialSurfaceWidth = 1920;
constexpr int32_t kInitialSurfaceHeight = 1080;

constexpr float kMinDistance = 0.5f, kMaxDistance = 3.0f;
constexpr float kMinWidth = 0.5f, kMaxWidth = 5.0f;
constexpr float kStatusWidthFraction = 0.6f;  // status panel width relative to the screen width
constexpr float kStickDeadzone = 0.25f;
constexpr float kDistanceSpeed = 0.8f;  // m/s at full deflection
constexpr float kWidthDoublingsPerSecond = 0.7f;

// Android Surface buffers are top-row-first while the runtime samples Android-surface swapchains
// with the GL bottom-left origin, which shows them upside down; XR_FB_composition_layer_image_layout
// flips them back. Measured with screencaps for both CPU (ANativeWindow_lock) and MediaCodec output.
constexpr bool kScreenVerticalFlip = true;
constexpr bool kStatusVerticalFlip = true;

const char* sessionStateName(XrSessionState state) {
    switch (state) {
        case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "?";
    }
}

std::string readProperty(const char* name) {
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}

bool hasExtension(const std::vector<XrExtensionProperties>& exts, const char* name) {
    return std::any_of(exts.begin(), exts.end(),
                       [name](const XrExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

template <typename Pfn>
bool loadFunction(XrInstance instance, const char* name, Pfn* out) {
    const XrResult r = xrGetInstanceProcAddr(instance, name, reinterpret_cast<PFN_xrVoidFunction*>(out));
    if (XR_FAILED(r) || *out == nullptr) {
        LOGE("xrGetInstanceProcAddr(%s) failed: %d", name, r);
        return false;
    }
    return true;
}

}  // namespace

XrApp::XrApp(ANativeActivity* activity, JNIEnv* env, ScreenSource& source)
    : activity_(activity), env_(env), source_(source) {
    source_.attachHost(*this);
    if (activity_->internalDataPath != nullptr) {
        mkdir(activity_->internalDataPath, 0700);
        placementPath_ = std::string(activity_->internalDataPath) + "/placement.txt";
    }
}

XrApp::~XrApp() { shutdown(); }

std::string XrApp::resultString(XrResult result) const {
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance_ != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance_, result, buf))) return buf;
    return "XrResult(" + std::to_string(result) + ")";
}

std::string XrApp::pathString(XrPath path) const {
    if (path == XR_NULL_PATH) return "(none)";
    char buf[XR_MAX_PATH_LENGTH] = {};
    uint32_t len = 0;
    if (XR_FAILED(xrPathToString(instance_, path, sizeof(buf), &len, buf))) return "(invalid)";
    return buf;
}

bool XrApp::initialize() {
    loadPlacement();
    if (!(createInstance() && initSystem() && initEgl() && createSession() && createActions() &&
          createPassthrough())) {
        return false;
    }
    setFilterMode(hasLayerSettings_ ? kDefaultFilterMode : 0, "default");
    lastRecenterProp_ = readProperty(kRecenterProperty);
    lastDistanceProp_ = readProperty(kDistanceProperty);
    lastWidthProp_ = readProperty(kWidthProperty);
    lastSimGrabProp_ = readProperty(kSimGrabProperty);
    lastFilterProp_ = readProperty(kFilterProperty);
    if (!lastFilterProp_.empty()) setFilterMode(std::atoi(lastFilterProp_.c_str()), kFilterProperty);

    if (!createSurfaceSwapchain(StatusPanel::kWidth, StatusPanel::kHeight, &status_)) return false;
    statusPanel_.attach(status_.window);
    LOGI("Status panel swapchain created: %dx%d", status_.width, status_.height);
    return createScreenSwapchain(kInitialSurfaceWidth, kInitialSurfaceHeight);
}

bool XrApp::createInstance() {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (!loadFunction(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &initializeLoader)) return false;
    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = activity_->vm;
    loaderInfo.applicationContext = activity_->clazz;
    XR_CHECK(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loaderInfo)));

    uint32_t count = 0;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr));
    std::vector<XrExtensionProperties> available(count, {XR_TYPE_EXTENSION_PROPERTIES});
    XR_CHECK(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, available.data()));
    std::string names;
    for (const XrExtensionProperties& e : available) (names += ' ') += e.extensionName;
    LOGI("Runtime offers %u instance extensions:%s", count, names.c_str());

    std::vector<const char*> enabled;
    for (const char* required : {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
                                 XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME, XR_FB_PASSTHROUGH_EXTENSION_NAME}) {
        if (!hasExtension(available, required)) {
            LOGE("Required extension %s is not available", required);
            return false;
        }
        enabled.push_back(required);
    }
    hasLayerSettings_ = hasExtension(available, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    if (hasLayerSettings_) enabled.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    hasAutoFilter_ = hasLayerSettings_ && hasExtension(available, XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME);
    if (hasAutoFilter_) enabled.push_back(XR_META_AUTOMATIC_LAYER_FILTER_EXTENSION_NAME);
    hasImageLayout_ = hasExtension(available, XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
    if (hasImageLayout_) enabled.push_back(XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);
    for (const char* name : enabled) LOGI("Enabling extension %s", name);
    if (!hasLayerSettings_) LOGW("%s not available: quad sharpening/super-sampling disabled",
                                 XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    if (!hasImageLayout_) LOGW("%s not available: the quad will show the Surface upside down",
                               XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = activity_->vm;
    androidInfo.applicationActivity = activity_->clazz;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidInfo;
    std::strncpy(createInfo.applicationInfo.applicationName, "QuestMirror", XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName, "none", XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    createInfo.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&createInfo, &instance_));

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    XR_CHECK(xrGetInstanceProperties(instance_, &props));
    LOGI("OpenXR runtime: %s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
         XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));

    return loadFunction(instance_, "xrGetOpenGLESGraphicsRequirementsKHR", &pfnGetOpenGLESGraphicsRequirementsKHR) &&
           loadFunction(instance_, "xrCreateSwapchainAndroidSurfaceKHR", &pfnCreateSwapchainAndroidSurfaceKHR) &&
           loadFunction(instance_, "xrCreatePassthroughFB", &pfnCreatePassthroughFB) &&
           loadFunction(instance_, "xrDestroyPassthroughFB", &pfnDestroyPassthroughFB) &&
           loadFunction(instance_, "xrCreatePassthroughLayerFB", &pfnCreatePassthroughLayerFB) &&
           loadFunction(instance_, "xrDestroyPassthroughLayerFB", &pfnDestroyPassthroughLayerFB);
}

bool XrApp::initSystem() {
    XrSystemGetInfo getInfo{XR_TYPE_SYSTEM_GET_INFO};
    getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(instance_, &getInfo, &systemId_));

    XrSystemPassthroughProperties2FB passthroughProps{XR_TYPE_SYSTEM_PASSTHROUGH_PROPERTIES2_FB};
    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    props.next = &passthroughProps;
    XR_CHECK(xrGetSystemProperties(instance_, systemId_, &props));
    LOGI("System: %s (vendor 0x%x), max layers %u, max swapchain %ux%u, passthrough capabilities 0x%" PRIx64,
         props.systemName, props.vendorId, props.graphicsProperties.maxLayerCount,
         props.graphicsProperties.maxSwapchainImageWidth, props.graphicsProperties.maxSwapchainImageHeight,
         static_cast<uint64_t>(passthroughProps.capabilities));
    if ((passthroughProps.capabilities & XR_PASSTHROUGH_CAPABILITY_BIT_FB) == 0) {
        LOGE("System reports no passthrough capability");
        return false;
    }
    return true;
}

bool XrApp::initEgl() {
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHECK(pfnGetOpenGLESGraphicsRequirementsKHR(instance_, systemId_, &reqs));

    eglDisplay_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay_ == EGL_NO_DISPLAY || !eglInitialize(eglDisplay_, nullptr, nullptr)) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint configAttribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                                    EGL_NONE};
    EGLint numConfigs = 0;
    if (!eglChooseConfig(eglDisplay_, configAttribs, &eglConfig_, 1, &numConfigs) || numConfigs < 1) {
        LOGE("eglChooseConfig failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 2, EGL_NONE};
    eglContext_ = eglCreateContext(eglDisplay_, eglConfig_, EGL_NO_CONTEXT, contextAttribs);
    if (eglContext_ == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }
    const EGLint pbufferAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    eglSurface_ = eglCreatePbufferSurface(eglDisplay_, eglConfig_, pbufferAttribs);
    if (eglSurface_ == EGL_NO_SURFACE || !eglMakeCurrent(eglDisplay_, eglSurface_, eglSurface_, eglContext_)) {
        LOGE("EGL pbuffer/makeCurrent failed: 0x%x", eglGetError());
        return false;
    }
    LOGI("EGL context: %s (runtime requires GLES %u.%u - %u.%u)", glGetString(GL_VERSION),
         XR_VERSION_MAJOR(reqs.minApiVersionSupported), XR_VERSION_MINOR(reqs.minApiVersionSupported),
         XR_VERSION_MAJOR(reqs.maxApiVersionSupported), XR_VERSION_MINOR(reqs.maxApiVersionSupported));
    return true;
}

bool XrApp::createSession() {
    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = eglDisplay_;
    binding.config = eglConfig_;
    binding.context = eglContext_;
    XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
    createInfo.next = &binding;
    createInfo.systemId = systemId_;
    XR_CHECK(xrCreateSession(instance_, &createInfo, &session_));
    LOGI("Session created");

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace = {{0, 0, 0, 1}, {0, 0, 0}};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(session_, &spaceInfo, &localSpace_));
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(session_, &spaceInfo, &viewSpace_));
    return true;
}

bool XrApp::createActions() {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(setInfo.actionSetName, "main");
    std::strcpy(setInfo.localizedActionSetName, "Main");
    XR_CHECK(xrCreateActionSet(instance_, &setInfo, &actionSet_));

    auto path = [this](const char* s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(instance_, s, &p);
        return p;
    };
    handPaths_[0] = path("/user/hand/left");
    handPaths_[1] = path("/user/hand/right");

    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strcpy(actionInfo.actionName, "recenter");
    std::strcpy(actionInfo.localizedActionName, "Recenter screen");
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &recenterAction_));
    actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    std::strcpy(actionInfo.actionName, "adjust_screen");
    std::strcpy(actionInfo.localizedActionName, "Screen distance and size");
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &adjustAction_));
    actionInfo.countSubactionPaths = 2;
    actionInfo.subactionPaths = handPaths_;
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::strcpy(actionInfo.actionName, "grab_screen");
    std::strcpy(actionInfo.localizedActionName, "Grab screen");
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &grabAction_));
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    std::strcpy(actionInfo.actionName, "grip_pose");
    std::strcpy(actionInfo.localizedActionName, "Grip pose");
    XR_CHECK(xrCreateAction(actionSet_, &actionInfo, &gripPoseAction_));

    const XrActionSuggestedBinding bindings[] = {
        {recenterAction_, path("/user/hand/right/input/a/click")},
        {recenterAction_, path("/user/hand/left/input/x/click")},
        {adjustAction_, path("/user/hand/right/input/thumbstick")},
        {grabAction_, path("/user/hand/left/input/squeeze/value")},  // runtime thresholds the float
        {grabAction_, path("/user/hand/right/input/squeeze/value")},
        {gripPoseAction_, path("/user/hand/left/input/grip/pose")},
        {gripPoseAction_, path("/user/hand/right/input/grip/pose")},
    };
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    suggested.suggestedBindings = bindings;
    suggested.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
    XR_CHECK(xrSuggestInteractionProfileBindings(instance_, &suggested));

    XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &actionSet_;
    XR_CHECK(xrAttachSessionActionSets(session_, &attachInfo));

    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.action = gripPoseAction_;
        spaceInfo.subactionPath = handPaths_[hand];
        spaceInfo.poseInActionSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        XR_CHECK(xrCreateActionSpace(session_, &spaceInfo, &gripSpaces_[hand]));
    }
    return true;
}

bool XrApp::createPassthrough() {
    XrPassthroughCreateInfoFB createInfo{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
    createInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    XR_CHECK(pfnCreatePassthroughFB(session_, &createInfo, &passthrough_));

    XrPassthroughLayerCreateInfoFB layerInfo{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
    layerInfo.passthrough = passthrough_;
    layerInfo.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
    layerInfo.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
    XR_CHECK(pfnCreatePassthroughLayerFB(session_, &layerInfo, &passthroughLayer_));
    LOGI("Passthrough started (reconstruction layer running)");
    return true;
}

bool XrApp::createSurfaceSwapchain(int32_t width, int32_t height, SurfaceSwapchain* out) {
    // XR_KHR_android_surface_swapchain: format, sampleCount, faceCount, arraySize and mipCount must
    // be zero; the Surface producer defines the pixel format.
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    createInfo.width = static_cast<uint32_t>(width);
    createInfo.height = static_cast<uint32_t>(height);
    jobject surface = nullptr;
    XR_CHECK(pfnCreateSwapchainAndroidSurfaceKHR(session_, &createInfo, &out->swapchain, &surface));
    if (surface == nullptr) {
        LOGE("xrCreateSwapchainAndroidSurfaceKHR returned no Surface");
        return false;
    }
    out->surface = env_->NewGlobalRef(surface);
    out->window = ANativeWindow_fromSurface(env_, out->surface);
    out->width = width;
    out->height = height;
    if (out->window == nullptr) {
        LOGE("ANativeWindow_fromSurface failed");
        return false;
    }
    return true;
}

void XrApp::destroySurfaceSwapchain(SurfaceSwapchain* sc) {
    if (sc->window != nullptr) ANativeWindow_release(sc->window);
    if (sc->surface != nullptr) env_->DeleteGlobalRef(sc->surface);
    if (sc->swapchain != XR_NULL_HANDLE) xrDestroySwapchain(sc->swapchain);
    *sc = SurfaceSwapchain{};
}

bool XrApp::createScreenSwapchain(int32_t width, int32_t height) {
    if (!createSurfaceSwapchain(width, height, &screen_)) {
        destroySurfaceSwapchain(&screen_);
        return false;
    }
    LOGI("Screen surface swapchain created: %dx%d", width, height);
    source_.onSurfaceReady(screen_.window, screen_.surface, width, height);
    return true;
}

void XrApp::destroyScreenSwapchain() {
    if (screen_.swapchain == XR_NULL_HANDLE) return;
    source_.onSurfaceDestroyed();
    destroySurfaceSwapchain(&screen_);
    LOGI("Screen surface swapchain destroyed");
}

void XrApp::requestSurfaceSize(int32_t width, int32_t height) {
    std::lock_guard<std::mutex> lock(resizeMutex_);
    requestedWidth_ = width;
    requestedHeight_ = height;
}

void XrApp::applySurfaceResize() {
    int32_t width, height;
    {
        std::lock_guard<std::mutex> lock(resizeMutex_);
        width = requestedWidth_;
        height = requestedHeight_;
        requestedWidth_ = requestedHeight_ = 0;
    }
    if (width <= 0 || height <= 0 || session_ == XR_NULL_HANDLE) return;
    if (width == screen_.width && height == screen_.height) return;  // the source sees the size already
    LOGI("Resizing the screen Surface %dx%d -> %dx%d", screen_.width, screen_.height, width, height);
    destroyScreenSwapchain();
    if (!createScreenSwapchain(width, height)) LOGE("Screen swapchain recreation failed");
}

void XrApp::shutdown() {
    if (instance_ == XR_NULL_HANDLE && eglDisplay_ == EGL_NO_DISPLAY) return;
    LOGI("Shutting down (session state %s, running %d)", sessionStateName(sessionState_), sessionRunning_);
    if (visible_) {
        visible_ = false;
        source_.onVisible(false);
    }
    if (placementDirty_) savePlacement();
    destroyScreenSwapchain();
    statusPanel_.detach();
    destroySurfaceSwapchain(&status_);
    if (passthroughLayer_ != XR_NULL_HANDLE) pfnDestroyPassthroughLayerFB(passthroughLayer_);
    passthroughLayer_ = XR_NULL_HANDLE;
    if (passthrough_ != XR_NULL_HANDLE) pfnDestroyPassthroughFB(passthrough_);
    passthrough_ = XR_NULL_HANDLE;
    for (XrSpace& space : gripSpaces_) {
        if (space != XR_NULL_HANDLE) xrDestroySpace(space);
        space = XR_NULL_HANDLE;
    }
    if (actionSet_ != XR_NULL_HANDLE) xrDestroyActionSet(actionSet_);
    actionSet_ = XR_NULL_HANDLE;
    recenterAction_ = grabAction_ = gripPoseAction_ = adjustAction_ = XR_NULL_HANDLE;
    if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
    viewSpace_ = XR_NULL_HANDLE;
    if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
    localSpace_ = XR_NULL_HANDLE;
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    session_ = XR_NULL_HANDLE;
    sessionRunning_ = false;
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    instance_ = XR_NULL_HANDLE;
    shutdownEgl();
    LOGI("Shutdown complete");
}

void XrApp::shutdownEgl() {
    if (eglDisplay_ == EGL_NO_DISPLAY) return;
    eglMakeCurrent(eglDisplay_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (eglSurface_ != EGL_NO_SURFACE) eglDestroySurface(eglDisplay_, eglSurface_);
    if (eglContext_ != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay_, eglContext_);
    eglTerminate(eglDisplay_);
    eglSurface_ = EGL_NO_SURFACE;
    eglContext_ = EGL_NO_CONTEXT;
    eglDisplay_ = EGL_NO_DISPLAY;
}

void XrApp::pollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (instance_ != XR_NULL_HANDLE && xrPollEvent(instance_, &event) == XR_SUCCESS) {
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                handleSessionStateChanged(*reinterpret_cast<XrEventDataSessionStateChanged*>(&event));
                break;
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOGW("Instance loss pending; exiting");
                exitRequested_ = true;
                break;
            case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                LOGW("Lost %u OpenXR events", reinterpret_cast<XrEventDataEventsLost*>(&event)->lostEventCount);
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                const auto& e = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&event);
                const bool local = e.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL;
                LOGI("Reference space %d change pending (changeTime %" PRId64 ", poseValid %d)%s",
                     e.referenceSpaceType, e.changeTime, e.poseValid, local ? "; recentering quad" : "");
                // The quad lives in LOCAL space; recenter once the new LOCAL origin is in effect.
                if (local) recenterAtTime_ = std::max<XrTime>(e.changeTime, 1);
                break;
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                logInteractionProfiles();
                break;
            case XR_TYPE_EVENT_DATA_PASSTHROUGH_STATE_CHANGED_FB:
                LOGW("Passthrough state changed: flags 0x%" PRIx64,
                     static_cast<uint64_t>(reinterpret_cast<XrEventDataPassthroughStateChangedFB*>(&event)->flags));
                break;
            default:
                LOGI("Unhandled OpenXR event type %d", event.type);
                break;
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    applySurfaceResize();
}

void XrApp::handleSessionStateChanged(const XrEventDataSessionStateChanged& event) {
    LOGI("Session state: %s -> %s", sessionStateName(sessionState_), sessionStateName(event.state));
    sessionState_ = event.state;
    focused_ = event.state == XR_SESSION_STATE_FOCUSED;
    // Leaving VISIBLE/FOCUSED pauses the source synchronously, so nothing writes to the quad's
    // Surface by the time STOPPING calls xrEndSession (required by XR_KHR_android_surface_swapchain).
    updateVisibility();
    switch (event.state) {
        case XR_SESSION_STATE_READY: {
            XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
            beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            const XrResult r = xrBeginSession(session_, &beginInfo);
            sessionRunning_ = XR_SUCCEEDED(r);
            LOGI("xrBeginSession: %s", resultString(r).c_str());
            break;
        }
        case XR_SESSION_STATE_STOPPING: {
            const XrResult r = xrEndSession(session_);
            sessionRunning_ = false;
            LOGI("xrEndSession: %s", resultString(r).c_str());
            break;
        }
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
            LOGI("Runtime requested exit (%s)", sessionStateName(event.state));
            exitRequested_ = true;
            break;
        default:
            break;
    }
}

void XrApp::updateVisibility() {
    const bool visible = sessionRunning_ &&
                         (sessionState_ == XR_SESSION_STATE_VISIBLE || sessionState_ == XR_SESSION_STATE_FOCUSED);
    if (visible == visible_) return;
    visible_ = visible;
    LOGI("Content %s", visible ? "visible: starting screen source" : "hidden: stopping screen source");
    statusDrawn_ = false;  // redraw the status panel once visible again
    source_.onVisible(visible);
}

void XrApp::logInteractionProfiles() {
    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
        XrPath handPath = XR_NULL_PATH;
        xrStringToPath(instance_, hand, &handPath);
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session_, handPath, &state))) {
            LOGI("Interaction profile %s: %s", hand, pathString(state.interactionProfile).c_str());
        }
    }
}

void XrApp::pollInput(double dtSeconds, XrTime time) {
    (void)time;
    if (!focused_) return;
    XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    if (XR_FAILED(xrSyncActions(session_, &syncInfo))) return;

    const bool grabbing = std::any_of(std::begin(grabs_), std::end(grabs_), [](const Grab& g) { return g.active(); });
    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = recenterAction_;
    XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &button)) && button.isActive &&
        button.changedSinceLastSync && button.currentState && !grabbing) {
        LOGI("Recenter requested (controller A/X)");
        recenterRequested_ = true;
    }
    if (grabbing) return;  // the grab owns the screen until released

    // Right thumbstick: Y moves the screen nearer/farther (along head -> screen), X shrinks/grows it.
    getInfo.action = adjustAction_;
    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_FAILED(xrGetActionStateVector2f(session_, &getInfo, &stick)) || !stick.isActive) return;
    auto deadzone = [](float v) {
        const float a = std::fabs(v);
        return a < kStickDeadzone ? 0.0f : std::copysign((a - kStickDeadzone) / (1.0f - kStickDeadzone), v);
    };
    const float x = deadzone(stick.currentState.x);
    const float y = deadzone(stick.currentState.y);
    if (x == 0.0f && y == 0.0f) return;
    const float dt = static_cast<float>(dtSeconds);
    setPlacement(placement_.distance + y * kDistanceSpeed * dt,
                 placement_.width * std::exp2(x * kWidthDoublingsPerSecond * dt), nullptr);
    placementIdleSeconds_ = 0;
}

void XrApp::updateGrabs(XrTime time) {
    static const char* const kNames[3] = {"left grip", "right grip", "simulated"};
    if (!quadPlaced_) return;
    for (int i = 0; i < 3; ++i) {
        bool pressed = false;
        XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
        bool tracked = false;
        if (i < 2) {
            if (focused_) {  // actions were synced by pollInput this frame
                XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
                getInfo.action = grabAction_;
                getInfo.subactionPath = handPaths_[i];
                XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
                pressed = XR_SUCCEEDED(xrGetActionStateBoolean(session_, &getInfo, &state)) && state.isActive &&
                          state.currentState;
                XrSpaceLocation grip{XR_TYPE_SPACE_LOCATION};
                constexpr XrSpaceLocationFlags kNeeded =
                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
                if (XR_SUCCEEDED(xrLocateSpace(gripSpaces_[i], localSpace_, time, &grip)) &&
                    (grip.locationFlags & kNeeded) == kNeeded) {
                    pose = grip.pose;
                    tracked = true;
                }
            }
        } else if (simGrabStep_ > 0) {
            // Three frames: press at the start pose, hold at the end pose, release.
            pressed = simGrabStep_ > 1;
            pose = simGrabStep_ == 3 ? simGrabStart_ : simGrabEnd_;
            tracked = true;
            --simGrabStep_;
        }

        bool othersActive = false;
        for (int j = 0; j < 3; ++j) othersActive |= j != i && grabs_[j].active();
        XrVector3f position = quadPose_.position;
        const Grab::Event event = grabs_[i].update(pressed, tracked ? &pose : nullptr, !othersActive, &position);
        if (event == Grab::Event::Started) {
            LOGI("Grab started (%s): screen at (%.2f, %.2f, %.2f), grabber at (%.2f, %.2f, %.2f)", kNames[i],
                 quadPose_.position.x, quadPose_.position.y, quadPose_.position.z, pose.position.x, pose.position.y,
                 pose.position.z);
        }
        if (grabs_[i].active()) {
            // Position follows the grabber; orientation stays upright, facing the head.
            quadPose_.position = position;
            if (headValid_) quadPose_.orientation = facing(position, headPose_.position);
        }
        if (event == Grab::Event::Released) onGrabReleased(kNames[i]);
    }
}

void XrApp::onGrabReleased(const char* who) {
    // Re-anchor the placement on the head so the thumbstick, recenter and persistence keep working
    // relative to the new spot.
    if (headValid_) anchorPosition_ = headPose_.position;
    const XrVector3f offset = quadPose_.position - anchorPosition_;
    const float distance = length(offset);
    if (distance > 1e-3f) anchorDirection_ = offset * (1.0f / distance);
    placement_.distance = std::clamp(distance, kMinDistance, kMaxDistance);
    placement_.heightOffset = anchorDirection_.y * placement_.distance;
    updateQuadPose();
    LOGI("Grab released (%s): screen at (%.2f, %.2f, %.2f), %.2f m away, %.2f m above eye height", who,
         quadPose_.position.x, quadPose_.position.y, quadPose_.position.z, placement_.distance,
         placement_.heightOffset);
    savePlacement();
}

void XrApp::startSimulatedGrab(const std::string& spec) {
    float dx = 0, dy = 0, dz = 0, yawDegrees = 0;
    if (std::sscanf(spec.c_str(), "%f %f %f %f", &dx, &dy, &dz, &yawDegrees) < 3) {
        LOGW("%s: expected \"dx dy dz [yawDegrees]\", got \"%s\"", kSimGrabProperty, spec.c_str());
        return;
    }
    if (!headValid_ || !quadPlaced_ || simGrabStep_ > 0) return;
    // The grabber starts at the head pose, then moves by (dx, dy, dz) and turns by yaw about +Y.
    simGrabStart_ = headPose_;
    const float half = yawDegrees * 3.14159265f / 360.0f;
    const XrQuaternionf yaw{0.0f, std::sin(half), 0.0f, std::cos(half)};
    const XrQuaternionf& q = simGrabStart_.orientation;
    simGrabEnd_.orientation = {yaw.w * q.x + yaw.y * q.z, yaw.w * q.y + yaw.y * q.w, yaw.w * q.z - yaw.y * q.x,
                               yaw.w * q.w - yaw.y * q.y};  // yaw * q
    simGrabEnd_.position = simGrabStart_.position + XrVector3f{dx, dy, dz};
    const XrVector3f expected =
        simGrabEnd_.position +
        rotate(simGrabEnd_.orientation, rotate(conjugate(q), quadPose_.position - simGrabStart_.position));
    LOGI("Simulated grab: move (%.2f, %.2f, %.2f), yaw %.0f deg; expect the screen at (%.2f, %.2f, %.2f)", dx, dy,
         dz, yawDegrees, expected.x, expected.y, expected.z);
    simGrabStep_ = 3;
}

void XrApp::pollDebugProperties() {
    const std::string recenter = readProperty(kRecenterProperty);
    if (recenter != lastRecenterProp_) {
        lastRecenterProp_ = recenter;
        LOGI("Recenter requested (%s=%s)", kRecenterProperty, recenter.c_str());
        recenterRequested_ = true;
    }
    const std::string filter = readProperty(kFilterProperty);
    if (filter != lastFilterProp_) {
        lastFilterProp_ = filter;
        if (!filter.empty()) setFilterMode(std::atoi(filter.c_str()), kFilterProperty);
    }
    const std::string distance = readProperty(kDistanceProperty);
    if (distance != lastDistanceProp_) {
        lastDistanceProp_ = distance;
        if (!distance.empty()) setPlacement(std::strtof(distance.c_str(), nullptr), placement_.width, kDistanceProperty);
    }
    const std::string width = readProperty(kWidthProperty);
    if (width != lastWidthProp_) {
        lastWidthProp_ = width;
        if (!width.empty()) setPlacement(placement_.distance, std::strtof(width.c_str(), nullptr), kWidthProperty);
    }
    const std::string simGrab = readProperty(kSimGrabProperty);
    if (simGrab != lastSimGrabProp_) {
        lastSimGrabProp_ = simGrab;
        if (!simGrab.empty()) startSimulatedGrab(simGrab);
    }
}

void XrApp::setPlacement(float distance, float width, const char* reason) {
    placement_.distance = std::clamp(distance, kMinDistance, kMaxDistance);
    placement_.width = std::clamp(width, kMinWidth, kMaxWidth);
    placement_.heightOffset = anchorDirection_.y * placement_.distance;  // same direction, new distance
    placementDirty_ = true;
    updateQuadPose();
    if (reason != nullptr) {
        LOGI("Placement: %.2f m wide at %.2f m (%s)", placement_.width, placement_.distance, reason);
        savePlacement();
    }
}

void XrApp::placementCommand(const PlacementCommand& command) {
    std::lock_guard<std::mutex> lock(commandMutex_);
    commands_.push_back(command);
}

void XrApp::applyPlacementCommands() {
    std::vector<PlacementCommand> commands;
    {
        std::lock_guard<std::mutex> lock(commandMutex_);
        commands.swap(commands_);
    }
    for (const PlacementCommand& c : commands) {
        const char* name = c.kind == PlacementCommand::Kind::Recenter   ? "RECENTER"
                           : c.kind == PlacementCommand::Kind::Move     ? "MOVE"
                           : c.kind == PlacementCommand::Kind::Distance ? "DISTANCE"
                           : c.kind == PlacementCommand::Kind::Size     ? "SIZE"
                                                                        : "RESET";
        if (std::any_of(std::begin(grabs_), std::end(grabs_), [](const Grab& g) { return g.active(); })) {
            LOGW("Mac CONTROL %s ignored while the screen is grabbed", name);
            continue;
        }
        if (!headValid_ || !quadPlaced_) {
            LOGW("Mac CONTROL %s ignored: no head pose yet", name);
            continue;
        }
        const XrPosef before = quadPose_;
        switch (c.kind) {
            case PlacementCommand::Kind::Recenter:
                placeInFront(headPose_);
                break;
            case PlacementCommand::Kind::Move: {
                // Orbit the head: re-anchor on the current head position, then turn the direction.
                anchorPosition_ = headPose_.position;
                const XrVector3f offset = quadPose_.position - anchorPosition_;
                const float distance = std::max(length(offset), 1e-3f);
                const XrVector3f d = offset * (1.0f / distance);
                constexpr float kStep = 5.0f * 3.14159265f / 180.0f;
                constexpr float kMaxElevation = 80.0f * 3.14159265f / 180.0f;
                const float azimuth = std::atan2(d.x, -d.z) + static_cast<float>(c.a) * kStep;  // + = right
                const float elevation =
                    std::clamp(std::asin(std::clamp(d.y, -1.0f, 1.0f)) + static_cast<float>(c.b) * kStep,
                               -kMaxElevation, kMaxElevation);  // + = up
                anchorDirection_ = {std::sin(azimuth) * std::cos(elevation), std::sin(elevation),
                                    -std::cos(azimuth) * std::cos(elevation)};
                placement_.distance = std::clamp(distance, kMinDistance, kMaxDistance);
                placement_.heightOffset = anchorDirection_.y * placement_.distance;
                updateQuadPose();
                break;
            }
            case PlacementCommand::Kind::Distance:
                setPlacement(placement_.distance * std::pow(1.1f, static_cast<float>(c.a)), placement_.width, nullptr);
                break;
            case PlacementCommand::Kind::Size:
                setPlacement(placement_.distance, placement_.width * std::pow(1.1f, static_cast<float>(c.a)), nullptr);
                break;
            case PlacementCommand::Kind::Reset:
                placement_ = Placement{};
                placeInFront(headPose_);
                break;
        }
        savePlacement();
        LOGI("Mac CONTROL %s (%d, %d): screen (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f), %.2f m wide at %.2f m, "
             "%.2f m above eye height",
             name, c.a, c.b, before.position.x, before.position.y, before.position.z, quadPose_.position.x,
             quadPose_.position.y, quadPose_.position.z, placement_.width, placement_.distance,
             placement_.heightOffset);
    }
}

void XrApp::loadPlacement() {
    if (placementPath_.empty()) return;
    FILE* f = std::fopen(placementPath_.c_str(), "r");
    if (f == nullptr) {
        LOGI("Placement: default %.2f m wide at %.2f m (no %s yet)", placement_.width, placement_.distance,
             placementPath_.c_str());
        return;
    }
    float distance = 0, width = 0, height = 0;
    const int fields = std::fscanf(f, "%f %f %f", &distance, &width, &height);  // older files: no height
    if (fields >= 2) {
        placement_.distance = std::clamp(distance, kMinDistance, kMaxDistance);
        placement_.width = std::clamp(width, kMinWidth, kMaxWidth);
        placement_.heightOffset = fields == 3 ? height : 0.0f;
    }
    std::fclose(f);
    LOGI("Placement: %.2f m wide at %.2f m, %.2f m above eye height (loaded from %s)", placement_.width,
         placement_.distance, placement_.heightOffset, placementPath_.c_str());
}

void XrApp::savePlacement() {
    placementDirty_ = false;
    if (placementPath_.empty()) return;
    FILE* f = std::fopen(placementPath_.c_str(), "w");
    if (f == nullptr) {
        LOGW("Placement: cannot write %s", placementPath_.c_str());
        return;
    }
    std::fprintf(f, "%.3f %.3f %.3f\n", placement_.distance, placement_.width, placement_.heightOffset);
    std::fclose(f);
    LOGI("Placement saved: %.2f m wide at %.2f m, %.2f m above eye height", placement_.width, placement_.distance,
         placement_.heightOffset);
}

bool XrApp::filterModeSupported(int32_t mode) const {
    if (mode < 0 || mode >= kFilterModeCount) return false;
    if (mode == 0) return true;
    return hasLayerSettings_ && (mode != kAutoFilterMode || hasAutoFilter_);
}

void XrApp::setFilterMode(int32_t mode, const char* reason) {
    if (!filterModeSupported(mode)) {
        LOGW("Quad filter mode %d not supported by this runtime (%s); keeping %s", mode, reason,
             kFilterModes[filterMode_].name);
        return;
    }
    filterMode_ = mode;
    LOGI("Quad filter (XR_FB_composition_layer_settings): mode %d = %s, layerFlags 0x%" PRIx64 " (%s)", mode,
         kFilterModes[mode].name, static_cast<uint64_t>(kFilterModes[mode].flags), reason);
}

bool XrApp::recenter(XrTime time) {
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(viewSpace_, localSpace_, time, &head))) return false;
    constexpr XrSpaceLocationFlags kNeeded =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((head.locationFlags & kNeeded) != kNeeded) return false;
    placeInFront(head.pose);
    LOGI("Screen placed at (%.2f, %.2f, %.2f), %.2f m wide at %.2f m, %.2f m above eye height; head at "
         "(%.2f, %.2f, %.2f)",
         quadPose_.position.x, quadPose_.position.y, quadPose_.position.z, placement_.width, placement_.distance,
         placement_.heightOffset, head.pose.position.x, head.pose.position.y, head.pose.position.z);
    return true;
}

void XrApp::placeInFront(const XrPosef& head) {
    // Gravity-aligned: keep only the head's yaw; the persisted height offset sets the elevation.
    const XrVector3f fwd = rotate(head.orientation, {0.0f, 0.0f, -1.0f});
    float fx = fwd.x, fz = fwd.z;
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len < 1e-3f) {
        fx = 0.0f;
        fz = -1.0f;
    } else {
        fx /= len;
        fz /= len;
    }
    const float distance = placement_.distance;
    const float height = std::clamp(placement_.heightOffset, -0.9f * distance, 0.9f * distance);
    const float horizontal = std::sqrt(distance * distance - height * height);
    anchorPosition_ = head.position;
    anchorDirection_ = XrVector3f{fx * horizontal, height, fz * horizontal} * (1.0f / distance);
    placement_.heightOffset = height;
    quadPlaced_ = true;
    updateQuadPose();
}

void XrApp::updateQuadPose() {
    quadPose_.position = anchorPosition_ + anchorDirection_ * placement_.distance;
    quadPose_.orientation = facing(quadPose_.position, anchorPosition_);
}

void XrApp::renderFrame() {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(session_, &waitInfo, &frameState);
    if (XR_FAILED(r)) {
        LOGE("xrWaitFrame: %s", resultString(r).c_str());
        return;
    }
    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    r = xrBeginFrame(session_, &beginInfo);
    if (XR_FAILED(r)) {
        LOGE("xrBeginFrame: %s", resultString(r).c_str());
        return;
    }

    const double dt = lastDisplayTime_ == 0
                          ? 0.0
                          : std::min(0.1, static_cast<double>(frameState.predictedDisplayTime - lastDisplayTime_) / 1e9);
    lastDisplayTime_ = frameState.predictedDisplayTime;

    // This frame's head pose: grabs keep the screen facing it, Mac CONTROL commands orbit it.
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    constexpr XrSpaceLocationFlags kHeadNeeded =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    headValid_ = XR_SUCCEEDED(xrLocateSpace(viewSpace_, localSpace_, frameState.predictedDisplayTime, &head)) &&
                 (head.locationFlags & kHeadNeeded) == kHeadNeeded;
    if (headValid_) headPose_ = head.pose;

    pollInput(dt, frameState.predictedDisplayTime);
    if (frameIndex_ % 36 == 0) pollDebugProperties();
    if (recenterAtTime_ != 0 && frameState.predictedDisplayTime >= recenterAtTime_) {
        recenterRequested_ = true;
        recenterAtTime_ = 0;
    }
    if (recenterRequested_ && recenter(frameState.predictedDisplayTime)) recenterRequested_ = false;
    updateGrabs(frameState.predictedDisplayTime);
    applyPlacementCommands();
    if (placementDirty_) {
        // Persist once the thumbstick has been released for half a second.
        placementIdleSeconds_ += dt;
        if (placementIdleSeconds_ > 0.5) savePlacement();
    }

    const bool picture = source_.pictureReady();
    // Draw only when the panel is submitted this frame: a runtime that consumes buffers only from
    // composited layers would otherwise fill the queue and block ANativeWindow_lock.
    const bool statusSubmitted = visible_ && frameState.shouldRender && quadPlaced_ && !picture;
    if (statusSubmitted && (!statusDrawn_ || statusDrawnGeneration_ != source_.statusGeneration())) {
        const uint64_t generation = source_.statusGeneration();
        statusDrawn_ = statusPanel_.draw(source_.statusText());
        statusDrawnGeneration_ = generation;
    }

    XrCompositionLayerPassthroughFB passthroughLayer{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    passthroughLayer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    passthroughLayer.space = XR_NULL_HANDLE;
    passthroughLayer.layerHandle = passthroughLayer_;

    XrCompositionLayerSettingsFB settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    settings.layerFlags = kFilterModes[filterMode_].flags;
    XrCompositionLayerImageLayoutFB imageLayout{XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB};
    imageLayout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;

    // Either the screen (once the source shows a picture) or the status panel, same pose.
    const SurfaceSwapchain& shown = picture ? screen_ : status_;
    const bool flip = picture ? kScreenVerticalFlip : kStatusVerticalFlip;
    const void* chain = nullptr;
    if (settings.layerFlags != 0) chain = &settings;
    if (hasImageLayout_ && flip) {
        imageLayout.next = const_cast<void*>(chain);
        chain = &imageLayout;
    }
    const float width = picture ? placement_.width : placement_.width * kStatusWidthFraction;

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    quad.next = chain;
    quad.layerFlags = 0;  // opaque
    quad.space = localSpace_;
    quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    quad.subImage.swapchain = shown.swapchain;
    quad.subImage.imageRect = {{0, 0}, {shown.width, shown.height}};
    quad.subImage.imageArrayIndex = 0;
    quad.pose = quadPose_;
    quad.size = {width, width * static_cast<float>(shown.height) / static_cast<float>(std::max(shown.width, 1))};

    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t layerCount = 0;
    if (frameState.shouldRender && visible_) {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&passthroughLayer);
        const bool showQuad = quadPlaced_ && shown.swapchain != XR_NULL_HANDLE && (picture || statusDrawn_);
        if (showQuad) layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    r = xrEndFrame(session_, &endInfo);
    if (XR_FAILED(r)) LOGE("xrEndFrame (%u layers): %s", layerCount, resultString(r).c_str());

    if (picture && layerCount == 2 && framesWithPicture_++ == 0) {
        LOGI("First frame submitted with passthrough + screen layers");
    }
    if (++frameIndex_ % 720 == 0) {
        LOGI("Frame loop: %" PRIu64 " frames, %" PRIu64 " with the screen, period %.1f ms", frameIndex_,
             framesWithPicture_, static_cast<double>(frameState.predictedDisplayPeriod) / 1e6);
    }
}

}  // namespace qm
