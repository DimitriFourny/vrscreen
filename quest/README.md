# Quest Mirror — Quest 3 app

Native C++/OpenXR app (no Java code, `android.app.NativeActivity`) that shows a world-locked
screen quad over full passthrough. The quad is an `XR_KHR_android_surface_swapchain`: an
`android.view.Surface` the compositor samples directly. Milestone 2 feeds that Surface with a CPU
test pattern; milestone 3 will decode the Mac's HEVC/H.264 stream into it with MediaCodec.

## Build

Requirements: JDK 17, Android SDK (platform 36, build-tools 36.1.0, NDK 30.0.16248370,
CMake 4.1.2). Gradle comes from the committed wrapper (Gradle 8.14.3, AGP 8.13.2).

```sh
cd quest
export JAVA_HOME=/opt/homebrew/opt/openjdk@17/libexec/openjdk.jdk/Contents/Home
export ANDROID_HOME=$HOME/Library/Android/sdk   # or put sdk.dir in local.properties
./gradlew assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk
```

The OpenXR loader and headers come from Maven (`org.khronos.openxr:openxr_loader_for_android`,
prefab); no Meta SDK is used.

## Install, run, stop

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb shell am start -n com.df0.questmirror/android.app.NativeActivity
adb shell am force-stop com.df0.questmirror
```

It also appears in the headset library as "Quest Mirror" (Unknown Sources).

## Logs

Everything the app logs uses the tag `QuestMirror`:

```sh
adb logcat -d -s QuestMirror        # dump
adb logcat -s QuestMirror           # follow
adb logcat -d -b crash              # native crashes
```

Startup prints the runtime name/version, the offered and enabled extensions, session state
transitions (`Session state: A -> B`), `Passthrough started`, `Surface swapchain created`, quad
placement and the test-pattern producer's frame rate every 5 s. Android lifecycle callbacks are
logged as `Activity onPause` etc.

## Controls

| Input | Action |
| --- | --- |
| **A** (right controller) or **X** (left) | Recenter: put the screen 1.2 m in front of where you are looking (yaw only, at eye height) |
| **B** (right) or **Y** (left) | Cycle the screen's compositor filter (see below) |
| Hold the Meta button (system recenter) | The runtime re-centers LOCAL space; the app re-places the screen in front of you |

The screen is also placed in front of the head automatically at startup.

Debug equivalents over adb (polled about twice a second; handy with the headset on the desk):

```sh
adb shell setprop debug.questmirror.recenter $RANDOM   # any new value recenters
adb shell setprop debug.questmirror.filter 2           # select filter mode (also applied at startup)
adb shell setprop debug.questmirror.filter "''"        # clear: default (2) at next launch
```

Filter modes (`XR_FB_composition_layer_settings`, plus `XR_META_automatic_layer_filter` for 6):

| Mode | Flags |
| --- | --- |
| 0 | none |
| 1 | normal sharpening |
| 2 | quality sharpening (default) |
| 3 | normal super-sampling |
| 4 | quality super-sampling |
| 5 | quality super-sampling + quality sharpening |
| 6 | automatic choice between quality super-sampling and quality sharpening |

## Code map (`app/src/main/cpp`)

- `main.cpp` — `android_main`: Android lifecycle (native_app_glue) and the frame loop.
- `xr_app.{h,cpp}` — OpenXR instance/session state machine, EGL context for the GLES binding,
  passthrough layer, the Android-surface quad swapchain, input/recentering, layer filtering.
- `screen_source.h` — the seam for milestone 3 (see below).
- `test_pattern_source.{h,cpp}` — milestone-2 `ScreenSource`: 1-px grids, resolution patches,
  5x7 bitmap text at 1-4 px per dot, colour bars, grey ramp, a moving marker and a frame counter,
  drawn with `ANativeWindow_lock`/`unlockAndPost` (RGBA_8888) at ~30 fps.

## Milestone-3 seam: `ScreenSource`

```cpp
class ScreenSource {
    virtual void onSurfaceReady(ANativeWindow* window, jobject surface, int32_t width, int32_t height) = 0;
    virtual void onVisible(bool visible) = 0;   // false: must stop writing before returning
    virtual void onSurfaceDestroyed() = 0;
};
```

All callbacks run on the XR thread. `window`/`surface` (JNI global ref) are owned by `XrApp` and
stay valid until `onSurfaceDestroyed` returns, so a MediaCodec source can pass `window` to
`AMediaCodec_configure` (or `surface` to the Java API). The swapchain size is
`QuadConfig::surfaceWidth/surfaceHeight` (1920x1080 today); the quad is `QuadConfig::widthMeters`
wide (1.6 m) with height following that aspect ratio.

Runtime facts found while building this (Quest 3, runtime "Oculus 209.166.0"):

- `xrCreateSwapchainAndroidSurfaceKHR` requires `format`, `sampleCount`, `faceCount`,
  `arraySize`, `mipCount` to be 0 (otherwise `XR_ERROR_VALIDATION_FAILURE`).
- The compositor samples the Surface bottom-up; without `XR_FB_composition_layer_image_layout`'s
  `VERTICAL_FLIP` bit on the quad, the image is upside down. The app sets it.
- RGBA_8888 Surface content is displayed with no extra gamma: an sRGB grey ramp comes back
  unchanged in screen captures.
- No projection layer is needed: frames contain only the passthrough layer and the quad.
