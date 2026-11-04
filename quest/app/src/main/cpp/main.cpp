// Quest Mirror: native OpenXR app (NativeActivity + android_native_app_glue).
// Mirrors the Mac's screen: TCP stream from quest-mirror-sender, decoded with MediaCodec straight
// into a world-locked Android-surface quad over passthrough.

#include "log.h"
#include "mediacodec_source.h"
#include "xr_app.h"

#include <android_native_app_glue.h>

#include <chrono>
#include <thread>

namespace {

struct AppState {
    bool resumed = false;
};

void onAppCmd(android_app* app, int32_t cmd) {
    auto* state = static_cast<AppState*>(app->userData);
    switch (cmd) {
        case APP_CMD_START: LOGI("Activity onStart"); break;
        case APP_CMD_RESUME:
            LOGI("Activity onResume");
            state->resumed = true;
            break;
        case APP_CMD_PAUSE:
            LOGI("Activity onPause");
            state->resumed = false;
            break;
        case APP_CMD_STOP: LOGI("Activity onStop"); break;
        case APP_CMD_DESTROY: LOGI("Activity onDestroy"); break;
        case APP_CMD_INIT_WINDOW: LOGI("Activity window created"); break;
        case APP_CMD_TERM_WINDOW: LOGI("Activity window destroyed"); break;
        case APP_CMD_GAINED_FOCUS: LOGI("Activity gained focus"); break;
        case APP_CMD_LOST_FOCUS: LOGI("Activity lost focus"); break;
        default: break;
    }
}

// Processes pending Android lifecycle/input events. Sleeps in the looper while there is nothing
// to drive: paused with no running session, or finishing after a fatal error.
void pumpAndroidEvents(android_app* app, const AppState& state, bool sessionRunning, bool finishing) {
    for (;;) {
        const bool block = !app->destroyRequested && (finishing || (!sessionRunning && !state.resumed));
        android_poll_source* source = nullptr;
        const int ident = ALooper_pollOnce(block ? -1 : 0, nullptr, nullptr, reinterpret_cast<void**>(&source));
        if (ident == ALOOPER_POLL_TIMEOUT || ident == ALOOPER_POLL_ERROR) return;
        if (ident >= 0 && source != nullptr) source->process(app, source);
        if (app->destroyRequested) return;
    }
}

}  // namespace

void android_main(android_app* app) {
    LOGI("android_main start");
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    AppState state;
    app->userData = &state;
    app->onAppCmd = onAppCmd;

    {
        qm::MediaCodecSource source(app->activity->internalDataPath != nullptr ? app->activity->internalDataPath : "");
        qm::XrApp xr(app->activity, env, source);
        bool finishing = false;
        if (!xr.initialize()) {
            LOGE("OpenXR initialisation failed; finishing activity");
            finishing = true;
            ANativeActivity_finish(app->activity);
        }

        while (!app->destroyRequested) {
            pumpAndroidEvents(app, state, xr.sessionRunning(), finishing);
            if (app->destroyRequested || finishing) continue;

            xr.pollEvents();
            if (xr.exitRequested()) {
                LOGI("Finishing activity");
                finishing = true;
                ANativeActivity_finish(app->activity);
                continue;
            }
            if (!xr.sessionRunning()) {
                // IDLE / after STOPPING while resumed: nothing to submit; wait for READY without spinning.
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            xr.renderFrame();
        }
        xr.shutdown();
    }

    app->activity->vm->DetachCurrentThread();
    LOGI("android_main exit");
}
