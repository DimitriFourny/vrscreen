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
../scripts/mirror.sh                              # on the Mac: the sender (see ../mac/README.md)
adb shell am start -n com.df0.questmirror/android.app.NativeActivity
adb shell am force-stop com.df0.questmirror       # stop
```

It also appears in the headset library as "Quest Mirror" (Unknown Sources). Start order does not
matter: the app keeps looking and shows what it is doing on a panel in front of you ("Looking for
the Mac over USB and Wi-Fi ...", what each transport answered, "Connected over Wi-Fi 192.168.1.20
(Mac name)", "... waiting for a picture").

### USB or Wi-Fi

Nothing to configure. Before every connection the app

1. tries USB: `127.0.0.1:7777`, which `adb reverse tcp:7777 tcp:7777` tunnels to the Mac (without
   a reverse it is refused at once; with a reverse but no sender, adbd closes it before the hello);
2. meanwhile broadcasts a discovery request (UDP 7778, to 255.255.255.255 and the Wi-Fi subnet's
   broadcast address) once per second; senders answer with their TCP port and name;
3. if USB gave no hello, connects to the sender that answered, or else to the last Wi-Fi sender it
   used (`files/last_host.txt`, so a network that drops broadcasts still works after one success).

USB therefore wins whenever both are available. A running stream never switches; after a
disconnect the policy runs again, so unplugging the cable falls back to Wi-Fi and a Wi-Fi drop falls
back to USB within about half a second of the sender being reachable. A dead Wi-Fi link (nothing
received for 3 s although the app pings every 500 ms) also counts as a disconnect.

To pin a sender (only that one is tried; must be an IP address):

```sh
adb shell setprop debug.questmirror.host 192.168.1.20
adb shell setprop debug.questmirror.port 7777            # optional, default 7777
adb shell setprop debug.questmirror.host "''"            # back to automatic
```

## Logs

Everything the app logs uses the tag `QuestMirror`:

```sh
adb logcat -s QuestMirror           # follow
adb logcat -d -b crash              # native crashes
```

Besides session/lifecycle transitions it logs `No sender yet (USB: ... | Wi-Fi: ...)` when that
changes, `Connected over Wi-Fi 192.168.1.20 (name)`, every `CONFIG`, the decoder
(`Decoder c2.qti.hevc.decoder configured ...`), Surface resizes, status changes, and once per second:

```
Stats: recv 59.9 fps, decoded 59.9 fps, 0.81 Mbit/s, capture->decoded avg 13.5 ms max 86.2 ms,
       decode avg 3.6 ms max 5.9 ms, rtt 5.19 ms, in decoder 1, c2.qti.hevc.decoder 1920x1080,
       Wi-Fi 192.168.1.20 (Mac mini)
```

- *capture->decoded*: Mac capture time to the decoded picture being handed to the compositor, via
  the PING/PONG clock offset. Frames whose capture time is not newer than every earlier one on the
  connection (keyframes re-encoded from an old capture, after connecting or after a static pause)
  are left out.
- *decode*: `queueInputBuffer` to output buffer, i.e. the decoder alone.
- *in decoder*: frames queued but not yet output.

Measured on this Quest 3 (HEVC, Mac on 5 GHz Wi-Fi, same access point):

| Stream | Link | fps | Mbit/s | capture->decoded avg (max) | decode | RTT |
| --- | --- | --- | --- | --- | --- | --- |
| test pattern 1920x1080 | USB | 60.0 | 0.82 | 12.7 ms (48) | 3.7 ms | 0.7 ms |
| test pattern 1920x1080 | Wi-Fi | 60.0 | 0.80 | 12.8 ms (30) | 3.6 ms | 5.2 ms |
| test pattern 3840x2160 | USB | 60.0 | 1.64 | 28.4 ms (63) | 7.6 ms | 0.7 ms |
| test pattern 3840x2160 | Wi-Fi | 59.9 | 1.76 | 26.2 ms (96) | 7.5 ms | 4.2 ms |
| screen 3840x2160 | USB | 29.9 | 4.85 | 24.9 ms (71) | 8.3 ms | 0.7 ms |
| screen 3840x2160 | Wi-Fi | 30.0 | 5.48 | 29.7 ms (87) | 8.5 ms | 5.5 ms |
| screen 1920x1080 | USB | 30.0 | 1.30 | 10.1 ms (51) | 3.9 ms | 0.7 ms |
| screen 1920x1080 | Wi-Fi | 30.1 | 0.91 | 14.2 ms (67) | 3.8 ms | 5.2 ms |

(10–12 one-second samples each; the screen rows depend on what changed on the Mac at the time.)

## Controls

| Input | Action |
| --- | --- |
| **Grip** (squeeze, right or left controller) | Grab the screen: while held it follows the controller rigidly (move the hand to carry it, turn the wrist to swing it), staying upright and turned towards your head; let go and it stays there |
| **A** (right controller) or **X** (left) | Recenter: screen in front of where you are looking, same distance, size and height offset |
| Right thumbstick up / down | Screen farther / nearer (0.5–3 m), along the line from your head to the screen |
| Right thumbstick right / left | Screen larger / smaller (0.5–5 m wide) |
| Hold the Meta button (system recenter) | The app re-places the screen in front of you |

From the Mac (sender shortcuts, sent as protocol `CONTROL` messages; see `../mac/README.md`):

| Mac shortcut | Action |
| --- | --- |
| ⌃⌥⌘ + arrows | Slide the screen in its own plane, 5 cm per step; it keeps its orientation |
| ⌃⌥⌘ + mouse | Orbit the screen around your head (keeps the distance, turns to face you, stays upright) |
| ⌃⌥⌘ + scroll | Zoom: distance ×1.1 per 100 scroll units, farther/nearer |
| ⌃⌥⌘⇧ + scroll | Size: width ×1.1 per 100 scroll units |
| ⌃⌥⌘⇧ + ↑ / ↓ | Distance ×1.1 / ÷1.1 |
| ⌃⌥⌘⇧ + → / ← | Size ×1.1 / ÷1.1 |
| ⌃⌥⌘ + R | Recenter (same as A/X) |
| ⌃⌥⌘ + 0 | Reset to 1.8 m wide at 1.2 m, at eye height, in front of you |

Mouse and scroll arrive as a stream of small deltas (up to 60 per second); each is applied on the
next frame.

Orientation: grabbing, orbiting, recenter and reset turn the screen towards your head (yaw, and
pitch when it sits above or below eye height); it never rolls, so it stays upright. Sliding and
distance/size changes keep the orientation it has. While grabbed it turns towards your current
head position; it ignores the thumbstick, A/X and Mac commands until released (one grabber at a
time; the first grip wins).

What is saved (`files/placement.txt`, as `distance width heightOffset`): distance from the eyes,
width, and how far the centre sits above or below eye height. They are written when you release a
grab, and half a second after the last thumbstick or Mac change (a mouse drag writes the file
once, not 60 times a second). The OpenXR LOCAL space changes every session, so an absolute pose is
not stored: at launch (and on recenter) the screen goes in front of where you look, at the saved
distance, width and height offset. So a vertical slide or orbit survives a relaunch as the height
offset, while a sideways slide or orbit does not (the screen comes back centred in front of you).
Default: 1.8 m wide at 1.2 m, at eye height.

Debug equivalents over adb (polled about twice a second; handy with the headset on the desk):

```sh
adb shell setprop debug.questmirror.recenter $RANDOM   # any new value recenters
adb shell setprop debug.questmirror.distance 1.5       # metres (saved like the thumbstick)
adb shell setprop debug.questmirror.width 2.0          # metres (saved like the thumbstick)
adb shell "setprop debug.questmirror.simgrab '0.3 0.2 0 45'"  # simulated grab: the grabber starts
                                                       # at the head, moves by (x, y, z) m, turns 45° left
adb shell setprop debug.questmirror.filter 1           # compositor filter mode (default 0 = none)
adb shell setprop debug.questmirror.lowlatency 0       # A/B: decoder without low-latency keys
adb shell setprop debug.questmirror.filter "''"        # clear any of these
```

Filter modes (`XR_FB_composition_layer_settings`, plus `XR_META_automatic_layer_filter` for 6),
only settable through the prop. No filtering is the default: compared in the headset it shows
desktop text best. Quality sharpening (2) draws a visible circle in the centre of the view (the
runtime seems to apply it to a foveal region only), and normal sharpening (1) degrades text
compared with none. Mode 0 submits the quad without any layer-settings struct.

| Mode | Flags |
| --- | --- |
| 0 | none (default) |
| 1 | normal sharpening (text worse than none) |
| 2 | quality sharpening (visible circle in the centre) |
| 3 | normal super-sampling |
| 4 | quality super-sampling |
| 5 | quality super-sampling + quality sharpening |
| 6 | automatic choice between quality super-sampling and quality sharpening |

## How it works (`app/src/main/cpp`)

- `main.cpp` — `android_main`: Android lifecycle (native_app_glue) and the frame loop.
- `xr_app.{h,cpp}` — OpenXR instance/session state machine, EGL context for the GLES binding,
  passthrough layer, the two Android-surface swapchains (screen + status panel), Surface resizes,
  input (A/X, grips, thumbstick), placement (anchor = head position, unit direction, distance;
  Mac `CONTROL` commands queued from the network thread) and layer filtering.
- `grab.{h,cpp}` — `Grab`: per-frame `update(pressed, grabberPose, allowStart, &position)`; keeps
  the object's offset in the grabber's frame. Used for both grips and the `simgrab` debug prop;
  a hand-tracking pinch can reuse it with a different pose source.
- `xr_math.h` — vector/quaternion helpers and `facing()` (upright, facing a point).
- `screen_source.h` — the seam between the XR shell and the content (`ScreenSource`/`ScreenHost`,
  `PlacementCommand`).
- `mediacodec_source.{h,cpp}` — the `ScreenSource`: glues the network client to the decoder,
  owns the status text, asks `XrApp` to resize the Surface to the stream size.
- `stream_client.{h,cpp}` — protocol v1 client on its own thread: the USB/Wi-Fi policy above
  (UDP discovery, last host), numeric-IP connects in 50 ms slices so `stop()` is immediate, hello
  check, CONFIG validation (1..8192, even), message parsing (unknown types skipped), PING every
  500 ms (5 fast ones after connect, offset from the lowest-RTT of the last 16), `FRAME_ACK` after
  every batch of consumed frames, `CONTROL` → `PlacementCommand`, dead-link timeout.
- `video_decoder.{h,cpp}` — AMediaCodec into the ANativeWindow; `low-latency=1`,
  `vendor.qti-ext-dec-low-latency.enable=1`, `priority=0`; prefers
  `c2.qti.avc.decoder.low_latency` for H.264 and falls back to the default decoder if it rejects
  the format; an output thread presents every picture at once (`releaseOutputBufferAtTime` with
  the local clock, not the Mac-clock PTS) and flags fatal codec errors.
- `status_panel.{h,cpp}`, `bitmap_font.{h,cpp}` — CPU-drawn status panel (5x7 bitmap font; UTF-8
  is decoded, no-break spaces become spaces, Latin-1 accents are dropped, anything else is `?`).

Design points:

- **One producer per Surface.** MediaCodec owns the screen Surface; status text lives on its own
  small swapchain. The XR frame shows the status panel until the decoder has rendered a picture on
  the current Surface while connected, then the screen.
- **Stream size = Surface size.** A CONFIG with a new size makes the network thread ask `XrApp`
  for a new Surface; `XrApp` recreates the swapchain on the XR thread
  (`onSurfaceDestroyed` → `onSurfaceReady`), then the decoder starts on it. The quad's aspect
  follows.
- **Not visible → nothing writes.** Leaving VISIBLE closes the connection and releases the codec
  synchronously before `xrEndSession` (measured 2–70 ms); becoming visible reconnects (the sender
  sends CONFIG and a keyframe on every connect). A failed or restarted decoder also reconnects, for
  the same keyframe, instead of waiting for one that a static screen would never send.
- Frames are fed to the decoder from the network thread without an extra queue; frames before
  the first keyframe after a CONFIG are discarded but still acknowledged.
- The status panel is only drawn on frames where it is submitted.

Runtime facts found on this Quest 3 (runtime "Oculus 209.166.0"):

- `xrCreateSwapchainAndroidSurfaceKHR` requires `format`, `sampleCount`, `faceCount`,
  `arraySize`, `mipCount` to be 0 (otherwise `XR_ERROR_VALIDATION_FAILURE`).
- The compositor samples Android-surface swapchains bottom-up: both CPU-drawn buffers and
  MediaCodec output appear upside down unless the quad carries
  `XR_FB_composition_layer_image_layout`'s `VERTICAL_FLIP` bit. The app sets it on both quads.
- No projection layer is needed: frames contain only the passthrough layer and one quad.
