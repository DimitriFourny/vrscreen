# Quest Mirror — Quest 3 app

Native C++/OpenXR app (no Java code, `android.app.NativeActivity`) that shows the Mac's screen as
a world-locked quad over full passthrough. It connects to `quest-mirror-sender` (see `../mac`,
protocol in `../docs/protocol.md`) and decodes the HEVC/H.264 stream with MediaCodec straight into
the quad's `XR_KHR_android_surface_swapchain` Surface — no copies, no GL rendering.

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

## Install and run

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
adb reverse tcp:7777 tcp:7777                     # USB: the app connects to 127.0.0.1:7777
../mac/.build/release/quest-mirror-sender         # on the Mac (--source test needs no permission)
adb shell am start -n com.df0.questmirror/android.app.NativeActivity
adb shell am force-stop com.df0.questmirror       # stop
```

It also appears in the headset library as "Quest Mirror" (Unknown Sources). Start order does not
matter: the app retries every second and shows what it is waiting for on a panel in front of you
("Waiting for the Mac on 127.0.0.1:7777 ...", "Connected ... waiting for a picture", last error).

Wi-Fi instead of USB: point the app at the Mac before launching (read at every connect attempt):

```sh
adb shell setprop debug.questmirror.host 192.168.1.20   # Mac's IP; "''" to clear
adb shell setprop debug.questmirror.port 7777
```

## Logs

Everything the app logs uses the tag `QuestMirror`:

```sh
adb logcat -s QuestMirror           # follow
adb logcat -d -b crash              # native crashes
```

Besides session/lifecycle transitions it logs `Connected to ...`, every `CONFIG`, the decoder
(`Decoder c2.qti.hevc.decoder configured ...`), Surface resizes, status changes, and once per second:

```
Stats: recv 53.2 fps, decoded 53.2 fps, 1.18 Mbit/s, capture->decoded avg 23.0 ms max 47.0 ms,
       decode avg 8.0 ms max 16.0 ms, rtt 0.60 ms, in decoder 1, c2.qti.hevc.decoder 3840x2160
```

- *capture->decoded*: Mac capture time to decoded picture handed to the compositor, via the
  PING/PONG clock offset. Only meaningful while the screen changes: after a pause, the sender's
  periodic keyframe re-encodes an old capture, which shows up as seconds of "latency".
- *decode*: `queueInputBuffer` to output buffer, i.e. the decoder alone.
- *in decoder*: frames queued but not yet output.

## Controls

| Input | Action |
| --- | --- |
| **A** (right controller) or **X** (left) | Recenter: screen in front of where you are looking (yaw only, at eye height) |
| Right thumbstick up / down | Screen farther / nearer (0.5–3 m) |
| Right thumbstick right / left | Screen larger / smaller (0.5–5 m wide) |
| **B** (right) or **Y** (left) | Cycle the screen's compositor filter (see below) |
| Hold the Meta button (system recenter) | The app re-places the screen in front of you |

Default: 1.8 m wide at 1.2 m. Distance and width are saved to the app's files dir
(`files/placement.txt`) half a second after the stick is released and restored at launch.

Debug equivalents over adb (polled about twice a second; handy with the headset on the desk):

```sh
adb shell setprop debug.questmirror.recenter $RANDOM   # any new value recenters
adb shell setprop debug.questmirror.distance 1.5       # metres (saved like the thumbstick)
adb shell setprop debug.questmirror.width 2.0          # metres (saved like the thumbstick)
adb shell setprop debug.questmirror.filter 2           # filter mode (also applied at startup)
adb shell setprop debug.questmirror.lowlatency 0       # A/B: decoder without low-latency keys
adb shell setprop debug.questmirror.filter "''"        # clear any of these
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

## How it works (`app/src/main/cpp`)

- `main.cpp` — `android_main`: Android lifecycle (native_app_glue) and the frame loop.
- `xr_app.{h,cpp}` — OpenXR instance/session state machine, EGL context for the GLES binding,
  passthrough layer, the two Android-surface swapchains (screen + status panel), Surface resizes,
  input, placement and layer filtering.
- `screen_source.h` — the seam between the XR shell and the content (`ScreenSource`/`ScreenHost`).
- `mediacodec_source.{h,cpp}` — the `ScreenSource`: glues the network client to the decoder,
  owns the status text, asks `XrApp` to resize the Surface to the stream size.
- `stream_client.{h,cpp}` — protocol v1 client on its own thread: connect (TCP_NODELAY), hello
  check, message parsing (unknown types skipped), PING every 500 ms (5 fast ones after connect,
  offset from the lowest-RTT of the last 16), `FRAME_ACK` after every batch of consumed frames,
  reconnect every second.
- `video_decoder.{h,cpp}` — AMediaCodec into the ANativeWindow; `low-latency=1`,
  `vendor.qti-ext-dec-low-latency.enable=1`, `priority=0`; prefers
  `c2.qti.avc.decoder.low_latency` for H.264; an output thread renders every picture at once.
- `status_panel.{h,cpp}`, `bitmap_font.{h,cpp}` — CPU-drawn status panel (5x7 bitmap font).

Design points:

- **One producer per Surface.** MediaCodec owns the screen Surface; status text lives on its own
  small swapchain. The XR frame shows the status panel until the decoder has rendered a picture on
  the current Surface while connected, then the screen.
- **Stream size = Surface size.** A CONFIG with a new size makes the network thread ask `XrApp`
  for a new Surface; `XrApp` recreates the swapchain on the XR thread
  (`onSurfaceDestroyed` → `onSurfaceReady`), then the decoder starts on it. The quad's aspect
  follows.
- **Not visible → nothing writes.** Leaving VISIBLE closes the connection and releases the codec
  synchronously before `xrEndSession`; becoming visible reconnects (the sender sends CONFIG and a
  keyframe on every connect). Decoder errors also reconnect, for the same keyframe.
- Frames are fed to the decoder from the network thread without an extra queue; frames before
  the first keyframe after a CONFIG are discarded but still acknowledged.

Runtime facts found on this Quest 3 (runtime "Oculus 209.166.0"):

- `xrCreateSwapchainAndroidSurfaceKHR` requires `format`, `sampleCount`, `faceCount`,
  `arraySize`, `mipCount` to be 0 (otherwise `XR_ERROR_VALIDATION_FAILURE`).
- The compositor samples Android-surface swapchains bottom-up: both CPU-drawn buffers and
  MediaCodec output appear upside down unless the quad carries
  `XR_FB_composition_layer_image_layout`'s `VERTICAL_FLIP` bit. The app sets it on both quads.
- No projection layer is needed: frames contain only the passthrough layer and one quad.
