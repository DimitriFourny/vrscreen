# VRScreen — Quest 3 app

Native C++/OpenXR app (no Java code, `android.app.NativeActivity`) that shows the Mac's screen as
a world-locked quad over full passthrough. It connects to `vrscreen-sender` (see `../mac`,
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

Host unit tests (gesture state machine, `facing()` near the zenith, highlight geometry, hello,
discovery and AUDIO_CONFIG framing, the audio jitter buffer/playout scheduler; plain CMake + the
host compiler, using the OpenXR headers Gradle extracted, so build the APK once first):

```sh
cmake -S tests -B build/host-tests && cmake --build build/host-tests && ctest --test-dir build/host-tests --output-on-failure
```

## Install and run

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
../scripts/mirror.sh                              # on the Mac: the sender (see ../mac/README.md)
adb shell am start -n com.df0.vrscreen/android.app.NativeActivity
adb shell am force-stop com.df0.vrscreen       # stop
```

It also appears in the headset library as "VRScreen" (Unknown Sources). Start order does not
matter: the app keeps looking and shows what it is doing on a panel in front of you ("Looking for
the Mac over USB and Wi-Fi ...", what each transport answered, "Connected over Wi-Fi 192.168.1.10
(My Mac)", "... waiting for a picture").

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
adb shell setprop debug.vrscreen.host 192.168.1.10
adb shell setprop debug.vrscreen.port 7777            # optional, default 7777
adb shell setprop debug.vrscreen.host "''"            # back to automatic
```

## Logs

Everything the app logs uses the tag `VRScreen`:

```sh
adb logcat -s VRScreen              # follow
adb logcat -d -b crash              # native crashes
```

Besides session/lifecycle transitions it logs `No sender yet (USB: ... | Wi-Fi: ...)` when that
changes, `Connected over Wi-Fi 192.168.1.10 (My Mac)`, every `CONFIG`, the decoder
(`Decoder c2.qti.hevc.decoder configured ...`), Surface resizes, status changes, and once per second:

```
Stats: recv 59.9 fps, decoded 59.9 fps, 0.81 Mbit/s, capture->decoded avg 13.5 ms max 86.2 ms,
       decode avg 3.6 ms max 5.9 ms, rtt 5.19 ms, in decoder 1, c2.qti.hevc.decoder 1920x1080,
       Wi-Fi 192.168.1.10 (My Mac)
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

## Audio

The Mac's sound plays in the headset (protocol `AUDIO_CONFIG` / `AUDIO`: 48 kHz stereo s16 PCM in
10 ms chunks stamped with their capture time; the Mac's menu turns it on or off). The headset's
volume buttons control it like any media.

- **Output**: AAudio, low-latency performance mode, s16 at the stream's rate and channel count
  (48 kHz stereo), media usage. It asks for an *exclusive* stream (MMAP straight to the device, no
  system mixer: the lowest latency) and falls back to *shared* if the system refuses or grants
  shared instead; the log says which (`Audio output started: ... exclusive (asked exclusive) ...`).
  The device buffer starts at 1 burst (exclusive) or 2 (shared) and grows by a burst each time
  the device reports xruns, up to 4. It opens on `AUDIO_CONFIG` (reopened if the format changes)
  and closes when the connection ends or the app is not visible (headset off, another app in
  front; it keeps playing behind the Meta menu, like the picture), and every connection starts
  with an empty buffer. A device change (e.g. Bluetooth headphones) reopens it. If the device
  refused the source rate the playout resamples linearly (logged as "source rate refused").
  A/B switches: `adb shell setprop debug.vrscreen.audioshare shared` (force shared) and
  `debug.vrscreen.audiobursts 1..8` (starting buffer), read when the stream opens (reconnect).
- **Timing** (`audio_playout.{h,cpp}`): every sample is played at its capture time (mapped to the
  headset clock with the PING clock offset) plus a target delay. The target is the larger of the
  video's capture→display delay (its capture→decoded average + 14 ms for the compositor: the sound
  is never ahead of the picture) and the worst arrival delay of the last 8 s + the device lead +
  5 ms, capped at 150 ms. The *device lead* is measured in every callback: a callback runs before
  its frames are heard (time until heard + the callback's own length), and the data must already
  be there then. During the first 4 s of a stream, while the arrival window is still learning,
  the target is at least the device lead + 30 ms. The target rises at once when the link gets
  worse and comes back down 8 s after. Underruns within 3 s of each other (the arrival delay
  keeps growing) add 20 ms each, which fades away at 2 ms/s after 5 s without one: it re-syncs
  with more headroom a few times instead of crackling. (With a ScreenCaptureKit sender, whose
  audio arrives early during the first second and then up to ~70 ms late in a 4 s sawtooth, that
  means a few re-syncs during the first ~6 s.)
- **Clock**: the AAudio callback knows when its frames will be heard (`AAudioStream_getTimestamp`,
  refreshed every 50 ms from the network thread). If what plays is more than 5 ms later (or 2 ms
  earlier) than due, the playout speeds up or slows down by a tiny resampling ratio (error ×
  0.25/s, at most 2 %) until it is within 1 ms: clock drift between the Mac and the headset and
  target changes are absorbed without clicks. More than 200 ms off: it fades out and jumps.
- **Gaps**: a hole in the capture times (the sender drops audio that would be more than 150 ms
  late, or audio was paused) is filled with silence at its place. If the buffer runs dry first
  (underrun) it fades out, plays silence and re-starts at the right spot as soon as audio is back.
  Chunks that arrive after their playout time are counted as late and skipped.
- **Playback-time stamps**: the Mac's tap sender stamps each chunk with when the Mac itself would
  play it (capture + the Mac output's latency: ~6 ms for its speakers, ~160 ms for AirPods), so
  with AirPods chunks arrive ~150 ms *before* their timestamp. They simply wait in the buffer
  (1.3 s of room); the arrival term of the target is then negative and the video delay sets it.
  When the Mac's output changes mid-stream the stamps step: back (to a faster output) → the
  overlapping audio is skipped and what follows plays at its new time; forward → that much
  silence is inserted. Neither needs a re-sync or causes underruns.

Once per second, while audio is on:

```
Audio: buffer <ms> (target <ms>, device lead <ms>), capture->speaker avg <ms> max <ms>, underruns <n>,
       resyncs <n>, late <n>, dropped <n>, gaps <n> (<ms>), corrections <n> (<+/-ms>),
       AAudio <rate> Hz <n> ch, device buffer <ms>, device xruns <n>
```

- *buffer*: received and not yet played. *target*: the delay aimed at (with any underrun boost).
  *device lead*: how long before being heard a callback needs its frames. *capture->speaker*: from
  the Mac's capture of a sample to the moment the speaker plays it (AAudio timestamps + clock
  offset), per callback.
- *underruns* / *resyncs*: ran dry / jumped. *late*: chunks that came after their time.
  *dropped*: duplicates. *gaps*: holes filled with silence. *corrections*: drift corrections
  started and how much source time they skipped (+) or stretched (−). *device xruns*: AAudio's own
  underruns (the callback was late), which should stay at 0.

## Headset battery

The app reports the headset's battery to the Mac (protocol `HEADSET_STATUS`: percent, charging,
plugged in) right after connecting, on every change and at least once a minute. It reads it every
10 s on its own thread from Android's sticky battery intent (`Context.registerReceiver(null,
ACTION_BATTERY_CHANGED)` through JNI, `battery_monitor.{h,cpp}`; encoding and send rules in
`headset_status.h`, host-tested) and logs changes as `Battery: 87 %, charging, plugged in`.

## Controls

| Input | Action |
| --- | --- |
| **Grip** (squeeze, right or left controller) | Grab the screen: while held it follows the controller rigidly (move the hand to carry it, turn the wrist to swing it), staying upright and turned towards your head; let go and it stays there |
| **A** (right controller) or **X** (left) | Recenter: screen in front of where you are looking, same distance, size and height offset |
| Right thumbstick up / down | Screen farther / nearer (0.5–3 m), along the line from your head to the screen |
| Right thumbstick right / left | Screen larger / smaller (0.5–5 m wide) |
| Hold the Meta button (system recenter) | The app re-places the screen in front of you |

While the screen is grabbed (grip or hand), a thin blue frame shows around it (an extra layer of
the same shape, 2.4 cm larger on each side at the default 1.8 m width, behind the screen; a
cylinder of the same radius when curved). It is only submitted while grabbing, so it costs nothing
the rest of the time.

### Hand gestures

Off by default. To use them:

1. On the Mac: menu-bar icon → **Hand gestures** (protocol `SET_HAND_GESTURES`; the Mac remembers
   it and re-sends it on every connect, the headset does not store it).
2. On the headset: Settings → Movement tracking → **Hand and body tracking** on, with
   **auto-switch between hands and controllers** on. Then put the controllers down: hands take over.

| Gesture | Action |
| --- | --- |
| **Pinch and hold** (thumb + index, about 0.4 s), one hand | Grab: the screen follows your hand rigidly, like a grip grab. Moving the hand carries it; pointing (the hand's aim ray) swings it around your hand, so a small arm movement places it far away or high up. Open the fingers to let go; it stays there, saved |
| Pinch with the **other hand too** (0.2 s) | Resize: width × (distance between the hands now ÷ when the second hand joined), 0.5–5 m; the screen's centre follows the midpoint between your hands (translation only) |
| Open one of the two hands | Back to a one-hand grab with the other hand, from where the screen is (no jump); the new size is kept |

Details and limits:

- The pose is the runtime's aim pose (`XR_FB_hand_tracking_aim`): it is the pinch-ray origin near
  the thumb and index, oriented from the shoulder through the hand, and the runtime already
  stabilises it for pointing; it does not twist when the fingers close or open, unlike the wrist or
  the fingertips. A light speed-adaptive filter (one-euro, 3 Hz at rest, ~12 Hz when moving)
  removes the remaining jitter, which a lever of a metre or more would otherwise magnify.
- The 0.4 s hold avoids grabbing by accident (hands moving in your lap). A pinch that flickers for
  less than 0.15 s does not drop the screen; while the pinch is loosening (or the hand is briefly
  lost) the screen holds still instead of following the fingers opening, and a hand lost for more
  than 0.5 s lets go. After a hand lets go for any other reason than opening (lost, Meta gesture,
  disabled), it has to open and pinch again.
- The system gesture (palm towards your face, pinch: Meta menu / menu button) never grabs and ends
  a grab by that hand.
- Grips and hands never grab at once: whichever starts first holds the screen until released; a
  pinch made meanwhile has to be repeated.
- While the Mac has gestures off, hand tracking is not even queried; turning them off during a
  hand grab releases it.
- Logged: `Hand trackers created` (or `Hand tracking unavailable: <why>`), `Mac CONTROL
  SET_HAND_GESTURES on|off`, `Hand tracking: left hand tracked|not tracked`, `Hand grab started`,
  `Two-hand resize ...`, `Grab released (hand, pinch released): ... (elevation 72 deg)`, and a hint
  if gestures are on but no hand was tracked for 10 s (controllers in use or hand tracking off).

In bed: lie back, pinch and hold, then point up: the screen swings up around your hand and can go
anywhere overhead, including straight up. See "Orientation" below for how it faces you there.

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

From the Mac menu-bar icon (protocol `SET_CURVATURE` / `SET_DIM`, absolute values the Mac re-sends
on every connect; the headset does not store them, so until the first one arrives the screen is
flat and the surroundings normal):

- **Curvature** 0–1000 ‰. 0 is the flat screen. Above 0 the screen becomes a section of a cylinder
  (`XR_KHR_composition_layer_cylinder`) of radius = viewing distance ÷ (curvature/1000), so 1000
  curves it around your eyes. Its centre, width (= arc length), facing and all the placement
  controls stay the same; the arc is capped at 2.8 rad (160°), beyond which the radius grows. The
  status panel stays flat. If the runtime lacked the extension the screen would stay flat (logged).
- **Dim** 0–1000 ‰ darkens the passthrough surroundings towards black, perceptually: 500 looks
  about half as bright. The compositor fades in linear light, so the passthrough layer's opacity
  is (1 − dim)^1.9: on the headset, exponent 1.8 gave 76/54/30 % brightness at 250/500/750 and
  2.0 gave 71/46/20 %, so 1.9 should land near 75/50/25 % (`dim_controller.h`; tune with
  `debug.vrscreen.dimgamma`). No edge rendering, and the screen itself is never dimmed.
  For safety the room is only dimmed while the screen shows a picture: when the stream goes away
  (searching, connecting, waiting for the first frame, decoder restart) the dim is kept for 1.5 s,
  so a quick reconnect does not flash, then fades out over 0.3 s; it fades back in over 0.3 s
  once a picture is shown again. Coming back to the app (session visible again) starts undimmed.

Orientation: grabbing, orbiting, recenter and reset turn the screen towards your head (yaw, and
pitch when it sits above or below eye height); it never rolls, so it stays upright (except
overhead or lying down, see below). Sliding and distance/size changes keep the orientation it has.
While grabbed it turns towards your current head position; it ignores the thumbstick, A/X and Mac
commands until released (one grabber at a time; the first one wins).

Overhead and lying down (`facing()` in `xr_math.h`): "upright" relative to gravity breaks down
near the zenith (a screen straight above has no "towards you" direction, so tiny head movements
would spin it and moving it over the top would flip it by 180°) and for someone lying down (on
your back with the screen towards the top of your head, gravity's up reads upside down). So the
screen's up blends from gravity to your head's up when it is 65–85° above (or below) eye level, or
when your head is tilted 50–80° from upright. Standing and looking up at a high screen this changes
nothing (your head's up is gravity's up seen from there); lying on your back the picture's top
points to the top of your head at any elevation, straight overhead included; lying on your side
the picture lies on its side with you. Limit: if your head's up points straight at the screen (the
screen overhead while you look straight ahead) the up is undefined and it falls back to gravity.
Recenter (A/X, Meta button, Mac, launch) uses your head's heading with its pitch undone, so lying
on your back and looking at the ceiling, "in front" is towards your feet; the saved height offset
is kept up to 85° of elevation.

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
adb shell setprop debug.vrscreen.recenter $RANDOM   # any new value recenters
adb shell setprop debug.vrscreen.distance 1.5       # metres (saved like the thumbstick)
adb shell setprop debug.vrscreen.width 2.0          # metres (saved like the thumbstick)
adb shell "setprop debug.vrscreen.simgrab '0.3 0.2 0 45'"  # simulated grab: the grabber starts
                                                     # at the head, moves by (x, y, z) m, turns 45° left
adb shell "setprop debug.vrscreen.simpinch '1 6 0 0.2 0'"  # simulated pinch (see below)
adb shell setprop debug.vrscreen.filter 1           # compositor filter mode (default 0 = none)
adb shell setprop debug.vrscreen.lowlatency 0       # A/B: decoder without low-latency keys
adb shell setprop debug.vrscreen.filter "''"        # clear any of these
```

`simpinch` runs a scripted pinch through the hand-gesture state machine instead of tracked hands
(only while gestures are on, like real hands; otherwise it is logged and ignored):
`'hands seconds [dx dy dz [spread [pitchDegrees]]]'`. One hand (the right) or both start 35 cm in
front of the head, 25 cm below it (both: 30 cm apart), aiming where the head looks, pinch, hold
still 0.5 s (the grab starts after 0.4 s), then move by (dx, dy, dz) m in LOCAL space, spread
`spread` m further apart and pitch the aim up by `pitchDegrees` about the hand, hold still 0.5 s
and open the pinch at `seconds`. The expected end position or width is logged first. Examples:
`'1 6 0 0.2 0'` (raise the screen 20 cm), `'2 6 0 0 0 0.3'` (double the width), `'1 6 0 0 0 0 70'`
(point up: the screen swings overhead).

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
  passthrough layer, the Android-surface swapchains (screen, status panel, highlight frame),
  Surface resizes, input (A/X, grips, thumbstick, hand gestures), placement (anchor = head
  position, unit direction, distance; Mac `CONTROL` commands queued from the network thread) and
  layer filtering.
- `grab.{h,cpp}` — `Grab`: per-frame `update(pressed, grabberPose, allowStart, &position)`; keeps
  the object's offset in the grabber's frame. Used for grips, one-hand pinches and the `simgrab`
  debug prop.
- `hand_gestures.{h,cpp}` — `HandGestures`: the platform-independent gesture state machine (hold,
  release hysteresis, system-gesture rejection, one-hand grab via `Grab`, two-hand resize) fed one
  `HandSample` per hand per frame; `PoseFilter` (one-euro smoothing of the aim pose).
- `hand_tracker.{h,cpp}` — `HandTracker`: `XR_EXT_hand_tracking` trackers (created with the session,
  destroyed before it) read with `XR_FB_hand_tracking_aim` into `HandSample`s.
- `xr_math.h` — vector/quaternion helpers, `facing()` (upright, facing a point; head-up blend
  overhead/lying down), `headingOf()` (pitch-robust heading for recenter), the slide/orbit maths,
  `cylinderFor()` (cylinder pose, radius and arc for the curved screen) and `highlightFrame()`.
- `dim_controller.h` — `DimController` (when the surroundings may be dimmed: grace, fades) and
  `passthroughOpacity()` (perceptual SET_DIM mapping).
- `screen_source.h` — the seam between the XR shell and the content (`ScreenSource`/`ScreenHost`,
  `ScreenCommand`).
- `mediacodec_source.{h,cpp}` — the `ScreenSource`: glues the network client to the decoder and
  the audio output, owns the status text, asks `XrApp` to resize the Surface to the stream size.
- `audio_playout.{h,cpp}` — `AudioPlayout`: lock-free jitter buffer and playout scheduler between
  the network thread and the AAudio callback (see "Audio"; host-tested with a simulated sender and
  device).
- `audio_output.{h,cpp}` — `AudioOutput`: the AAudio stream, its timestamps, device-change
  reopening.
- `battery_monitor.{h,cpp}`, `headset_status.h` — battery reading (JNI, own thread) and the
  HEADSET_STATUS payload / send rules.
- `stream_client.{h,cpp}` — protocol v1 client on its own thread: the USB/Wi-Fi policy above
  (UDP discovery, last host), numeric-IP connects in 50 ms slices so `stop()` is immediate, hello
  check, CONFIG validation (1..8192, even), message parsing (unknown types skipped), PING every
  500 ms (5 fast ones after connect, offset from the lowest-RTT of the last 16), `FRAME_ACK` after
  every batch of consumed frames, `CONTROL` → `ScreenCommand`, `AUDIO_CONFIG`/`AUDIO` → audio,
  dead-link timeout.
- `wire.h` — socket-free protocol framing: the `VRSC` magic, hello check, DISCOVER datagram,
  ANNOUNCE and AUDIO_CONFIG parsing (host-tested).
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
  MediaCodec output appear upside down unless the layer carries
  `XR_FB_composition_layer_image_layout`'s `VERTICAL_FLIP` bit. The app sets it on the quads and
  on the cylinder.
- No projection layer is needed: frames contain only the passthrough layer and one screen layer
  (quad or cylinder), plus the highlight frame behind it while the screen is grabbed.
