# VRScreen — Mac side

VRScreen — Mac screen mirroring for Meta Quest. This package is the Mac side: it
mirrors the Mac's display to a Quest 3 (view-only) and contains:

- `vrscreen-sender`: captures a display and the system audio with ScreenCaptureKit,
  encodes the picture with VideoToolbox (HEVC or H.264, hardware, low latency) and
  serves both over TCP. It is also the menu-bar app: `scripts/build-app.sh` wraps it
  into `VRScreen.app` (see [Building VRScreen.app](#building-vrscreenapp)).
- `vrscreen-viewer`: a Mac test client (window or headless) that speaks the same
  protocol as the Quest app, and can write the received audio to a WAV file.
- `MirrorProtocol`: the shared protocol library (framing, message types,
  Annex-B/AVCC and parameter-set helpers).

The wire format is specified in [`../docs/protocol.md`](../docs/protocol.md).

## Requirements

- macOS 14 or later, Apple silicon recommended (hardware HEVC encoder).
- Swift 6 toolchain. Xcode is not required: the Command Line Tools are enough.

## Build and test

```sh
cd mac
swift build -c release
swift test
```

Binaries end up in `.build/release/`.

## Building VRScreen.app

```sh
scripts/build-app.sh             # build/VRScreen.app
scripts/build-app.sh --dist      # also build/VRScreen-<version>.zip
scripts/build-app.sh --install   # also replace /Applications/VRScreen.app (quits and restarts a running one)
```

From the repo root. It builds the sender (release) and the headset app (`quest/gradlew
assembleDebug`, so the Android toolchain from the top-level README is needed), then
assembles `build/VRScreen.app`:

| Path | Contents |
|---|---|
| `Contents/Info.plist` | `com.df0.vrscreen.mac`, version from the top-level `VERSION` file, `LSUIElement` (menu bar only), `LSMultipleInstancesProhibited`, minimum macOS 14.2 (process tap), the System Audio Recording and Local Network usage strings |
| `Contents/MacOS/VRScreen` | the `vrscreen-sender` binary |
| `Contents/Resources/AppIcon.icns` | from `mac/Resources/` (rendered by `swift scripts/make-icon.swift mac/Resources/AppIcon.icns preview.png`) |
| `Contents/Resources/vrscreen-headset.apk` | the headset app, same version, installed from the menu (Headset ▸) |

The bundle is signed ad hoc (`codesign --sign -`, no Developer ID) and checked with
`codesign --verify --deep --strict` and `plutil -lint`. macOS ties privacy permissions to
the code signature, and an ad-hoc signature changes with every build: after installing
a new build, Screen Recording and System Audio Recording may have to be granted again
(remove the old VRScreen entry in System Settings first if it doesn't take). The
version (Mac app and headset app, `versionCode` = MAJOR·10000 + MINOR·100 + PATCH) lives
in `VERSION`.

With only the Command Line Tools installed, SwiftPM does not find the swift-testing
macro plugin on its own; `Package.swift` adds the CLT plugin path to the test target
in that setup (and only then).

## Run

```sh
# Sender: capture the main display (needs Screen Recording permission, see below)
.build/release/vrscreen-sender

# Sender for testing next to a running VRScreen: other ports, no adb, own settings file
.build/release/vrscreen-sender --port 7790 --discovery-port 7791 --no-adb --no-hotkeys \
    --settings /tmp/test-settings.json --source test

# Sender with the synthetic test pattern (no permission needed)
.build/release/vrscreen-sender --source test

# Viewer in a window
.build/release/vrscreen-viewer

# Viewer headless: stats only, one PNG snapshot, then exit
.build/release/vrscreen-viewer --headless --snapshot /tmp/mirror.png --exit-after-snapshot

# A/V check: test pattern (white flash + 1 kHz beep every second) → WAV + sync report
.build/release/vrscreen-sender --source test
.build/release/vrscreen-viewer --headless --audio-wav /tmp/mirror.wav --av-sync --duration 10
```

Stop either with Ctrl-C.

### Sender options

Everything in the menu is a saved setting (see [Settings](#settings)). These options
override the saved value for this session only (they are never saved; changing the same
setting in the menu replaces the override):

| Option                     | Setting (default)            | Meaning                                                  |
|----------------------------|------------------------------|----------------------------------------------------------|
| `--fps N`                  | `fps` (60)                   | Target frame rate                                        |
| `--max-width N`            | `maxWidth` (3840)            | Scale the display's native pixel size down to fit this width |
| `--codec hevc\|h264`       | `codec` (hevc)               | Video codec                                              |
| `--bitrate MBPS`           | `bitrateMbps` (30)           | Average bitrate, Mbit/s                                  |
| `--no-hotkeys`             | `hotkeys` (on)               | Do not register the global screen-placement shortcuts    |
| `--no-mouse`               | `mouseGestures` (on)         | Do not use ⌃⌥⌘ + mouse (orbit) / scroll (zoom)           |
| `--mouse-sensitivity DEG`  | `mouseSensitivity` (0.1)     | Orbit degrees per point of mouse movement                |
| `--scroll-sensitivity X`   | `scrollSensitivity` (1)      | Scroll zoom multiplier (a wheel notch is a quarter step) |
| `--no-launch`              | `launchHeadsetApp` (on)      | Do not start the headset app when it appears on USB      |

Developer options (command line only):

| Option                     | Default | Meaning                                                      |
|----------------------------|---------|--------------------------------------------------------------|
| `--source screen\|test`    | screen  | ScreenCaptureKit capture, or the synthetic test pattern (flashes white once per second, with a 1 kHz beep at the flash while audio is on); `--max-width` also scales the pattern |
| `--display N`              | menu choice, else main | Display index, from the list printed at startup (for this session) |
| `--port N`                 | 7777    | TCP listen port (all interfaces)                             |
| `--discovery-port N`       | 7778    | UDP port answering LAN discovery (DISCOVER → ANNOUNCE)       |
| `--no-discovery`           | off     | Do not answer LAN discovery                                  |
| `--keyframe-interval S`    | 5       | Maximum seconds between keyframes                            |
| `--max-in-flight N`        | 3       | For clients that send FRAME_ACK: max frames sent but not yet consumed |
| `--width N --height N`     | 1920x1080 | Test-pattern size                                          |
| `--no-menu`                | off     | No menu-bar icon (a port in use is then fatal instead of retried) |
| `--no-audio`               | off     | Never capture or send audio, whatever the menu says          |
| `--audio-source auto\|tap\|sck` | auto | System audio from the Core Audio process tap or ScreenCaptureKit; `auto` = tap, else ScreenCaptureKit (see [Audio](#audio)) |
| `--settings PATH`          | standard | Settings file to read/write instead of `~/Library/Application Support/VRScreen/settings.json` (e.g. a second sender for testing) |
| `--no-adb`                 | off     | Do not use adb at all (Wi-Fi only; see [USB](#usb-the-headset-over-adb)) |
| `--adb-serial S`           | auto    | The headset's adb serial when several devices are attached   |
| `--debug-control-on-connect SPEC` | none | Testing: CONTROL commands sent to each client on connect, paced at 60 Hz; `wait:MS` pauses; e.g. `"translate:1,0;orbit:50,0*50;curvature:500;wait:3000;dim:1000"` |
| `--version`                |         | Print the version (`dev` for a bare binary)                  |

`kill -USR1 <pid>` makes the sender re-read its settings file, e.g. after editing it by
hand; video changes apply at once, as from the menu.

Once per second it logs capture fps, encoded fps (and keyframes), captures skipped by
backpressure, the bitrate actually sent, audio chunks queued per second, dropped as too
late, and how late the audio source delivered its buffers (`tap delivery` or `SCK
delivery p50/max`, after each buffer's last sample; or `audio off`), and the client
with its unsent backlog and, for clients that send FRAME_ACK, frames in flight
(current/limit and the per-second max).

### Viewer options

| Option                  | Default   | Meaning                                  |
|-------------------------|-----------|------------------------------------------|
| `--host HOST`           | 127.0.0.1 | Sender address                           |
| `--port N`              | 7777      | Sender port                              |
| `--discover`            | off       | Find the sender by UDP broadcast instead of `--host`/`--port` (again on every reconnect) |
| `--discovery-port N`    | 7778      | UDP discovery port                       |
| `--headless`            | off       | No window; decode and print stats        |
| `--snapshot PATH.png`   | none      | Write the first decoded frame as PNG     |
| `--exit-after-snapshot` | off       | Exit once the snapshot is written        |
| `--duration S`          | none      | Exit after S seconds                     |
| `--audio-wav PATH.wav`  | none      | Write the received audio to a WAV file   |
| `--av-sync`             | off       | With `--source test`: pair each white flash with its beep and log audio − video |
| `--fake-battery SPEC`   | none      | Testing: send HEADSET_STATUS like a headset, e.g. `72`, `72,charging`, `100,plugged`, `unknown` (on connect, then every 60 s) |

Once per second it logs received and decoded fps, bitrate, capture→decoded latency
(avg/max, measured through the PING/PONG clock offset the way the Quest client does
it), the last PING round trip, the stream format and, once audio arrives, audio chunks
per second, PCM kbit/s, gaps (a chunk starting more than 1 ms after the previous one
ended; total gap time in brackets) and arrival − capture time per chunk (p50/p95/max,
same clock offset). It acknowledges every FRAME
(FRAME_ACK) as it hands it to the decoder, and reconnects every second while the
sender is unreachable or after the sender restarts. CONTROL messages are printed
(`control: TRANSLATE dx=+1 dy=0`, `control: ORBIT yaw=+0.50° pitch=+0.00°`).

`--audio-wav` writes 16-bit PCM in the stream's format and fills gaps with silence
according to the timestamps (up to 10 s per gap), so a position in the file maps to
capture time: the log gives the capture time of the first sample. The header is
updated every second and on exit (`--duration`, Ctrl-C, SIGTERM). `--av-sync` detects
the test pattern's flash in the decoded frames (mean brightness above 200/255) and the
beep onset in the audio (first sample above 1000/32767 after 100 ms of quiet), and logs
`av-sync: … audio − video = +0.028 ms` per pair and a summary on exit.

## Menu-bar icon

The sender puts a Vision Pro–style icon in the menu bar (a template SF Symbol, so it
follows light/dark mode). While something needs attention it gets a "!" badge and the
menu starts with a [problem list](#problems). The menu:

```
[problems, each with its fixes]           (only while there are any)
Headset: USB (127.0.0.1) | Headset: Wi-Fi <ip> | No headset connected
Battery: 72 % — charging                  (while a headset is connected)
Travel mode: Off
Curved screen  ───●──────  25 %
Dim surroundings  ─●─────  10 %
✓ Hand gestures   (Pinch & hold to move, two hands to resize)
✓ Audio in headset
  Mute Mac while mirroring
Video ▸     Resolution: Up to 3840 px wide (4K) / 2560 / 1920 (1080p)
            Frame rate: 60 / 30 fps · Codec: HEVC (H.265) / H.264
            Bitrate: 15 / 30 / 50 Mbit/s · Display: one per screen (with 2+ displays)
Controls ▸  Keyboard shortcuts (⌃⌥⌘ + arrows, R, 0) · Mouse orbit & scroll zoom (⌃⌥⌘)
            Mouse sensitivity: Low / Normal / High · Scroll sensitivity: Low / Normal / High
Headset ▸   Quest 3 on USB (serial) · Headset app: 0.2.0 installed · 0.3.0 bundled
            Install / Update / Reinstall headset app · Launch headset app
            Open Quick Settings (Travel mode)… · [Restart adb] · ✓ Open headset app when connected
Re-center / Reset placement               (like ⌃⌥⌘R / ⌃⌥⌘0)
Launch at login                           (VRScreen.app only)
Show Logs… / About VRScreen / Quit VRScreen
```

- **Status lines**: the connection as the sender sees it (`127.0.0.1` is the adb tunnel),
  updated live, also while the menu is open. The battery line shows what the headset
  reports (HEADSET_STATUS): `Battery: 72 %`, `— charging`, `— plugged in`, or `—` until it
  reports; hidden while no headset is connected, reset on every new connection. Changes
  are logged once (`headset battery 72 % charging`); a malformed report is logged and
  ignored.
- **Curved screen** 0–100 %: 0 = flat, 100 % = the screen wraps around you on a cylinder
  whose radius is the viewing distance. **Dim surroundings** 0–100 %: darkens the
  passthrough around the screen, up to black; the screen itself is never dimmed. The
  sliders update the headset while you drag (at most 30 messages per second, the final
  value always sent).
- **Hand gestures** (default off): pinch & hold to move the screen, two hands to resize;
  while off, the headset ignores hand input entirely.
- **Audio in headset** (default on) and **Mute Mac while mirroring** (default off), see
  [Audio](#audio); greyed out with `--no-audio`.
- **Video**: changes apply live. A codec, bitrate or frame-rate change replaces the
  encoder (the old one is flushed first); a resolution, frame-rate or display change
  restarts the capture. The client then gets a keyframe, preceded by a new CONFIG when
  the stream format changed; nothing else restarts. A value set on the command line that
  matches no preset shows as an extra checked row ("Up to 1280 px wide (command line)").
- **Controls**: the shortcuts and the mouse gestures register/unregister at once; the
  sensitivities are 0.05 / 0.1 / 0.2° per point (mouse) and ×0.5 / ×1 / ×2 (scroll).
- **Headset** (USB only, see [USB](#usb-the-headset-over-adb)): the link state, the
  installed and bundled headset app versions, Install/Update (runs `adb install -r` with
  the APK inside VRScreen.app in the background; the menu shows "Installing the headset
  app…" and then the result), Launch, Quick Settings for travel mode, and the
  "Open headset app when connected" setting. Without adb the submenu says so, with the
  install command to copy and a link to Google's platform-tools page.
- **Launch at login** registers VRScreen.app as a login item (`SMAppService.mainApp`);
  if macOS wants approval, the item says so and opens System Settings › General › Login
  Items. Disabled for a bare `vrscreen-sender`.
- **Show Logs…** opens [the log file](#logs) in Console. **Quit VRScreen** is the same
  clean shutdown as Ctrl-C (cursor released, Mac output unmuted if the sender muted it,
  the adb reverse it created removed); so is a logout or a `quit` Apple Event.

**Travel mode** (for using the headset in a moving vehicle) can't be switched from the
Mac: Horizon OS only lets the headset's own Quick Settings toggle it. The status line
shows its state, read with `adb shell dumpsys tracking` (the ` * Travel Mode:` line), and
Headset ▸ Open Quick Settings opens Quick Settings on the headset (`am start -a
com.oculus.action.QUICK_SETTINGS`), where you tap the Travel mode tile. The state is
read when the menu opens, every 3 s while it stays open, and 3, 6 and 10 s after opening
Quick Settings, each read in the background with a 3 s timeout. Without a headset on
USB the line explains why ("headset not on USB", "adb not found", "USB off (--no-adb)",
or "unavailable (dumpsys format changed)" if a Horizon OS update changes the output).

### Settings

The sender owns the settings: it sends curvature, dimming and hand gestures to every
headset that connects (also with `--no-menu`) and stores everything in
`~/Library/Application Support/VRScreen/settings.json` (`--settings` for another file),
written half a second after the last change and read at startup:

| Key | Default | Menu |
|---|---|---|
| `curvature`, `dim` | 0, 0 | sliders, permille 0–1000 |
| `handGestures` | false | Hand gestures |
| `audio`, `muteMac` | true, false | Audio in headset, Mute Mac while mirroring |
| `maxWidth` | 3840 | Video ▸ Resolution |
| `fps` | 60 | Video ▸ Frame rate |
| `codec` | `"hevc"` | Video ▸ Codec (`"hevc"` or `"h264"`) |
| `bitrateMbps` | 30 | Video ▸ Bitrate |
| `displayID` | absent = main display | Video ▸ Display (a CGDirectDisplayID; the main display is used while it is not connected) |
| `hotkeys`, `mouseGestures` | true, true | Controls ▸ Keyboard shortcuts, Mouse orbit & scroll zoom |
| `mouseSensitivity`, `scrollSensitivity` | 0.1, 1 | Controls ▸ sensitivities |
| `launchHeadsetApp` | true | Headset ▸ Open headset app when connected |

A key missing from an older file takes its default, a value out of range is clamped,
and a key with a wrong type or unknown value takes its default without affecting the
others. Command-line overrides are applied on top and never written.

### Problems

Each problem shows a short title, a line of detail and its fixes; the icon gets a badge
while any is listed. They are re-evaluated on the relevant events and whenever the menu
opens (the permission checks are TCC's read-only preflights, which never prompt). Errors
come first, then warnings:

| Problem | Detected by | Offered |
|---|---|---|
| Port 7777 in use (another VRScreen running?) | `bind` fails with EADDRINUSE; the sender retries every 3 s and starts once the port is free | Show Logs… |
| Screen Recording not allowed | `CGPreflightScreenCaptureAccess()` false, or ScreenCaptureKit's "user declined" (screen capture only, not `--source test`) | Grant Screen Recording… (opens Privacy & Security › Screen & System Audio Recording), Relaunch VRScreen |
| Screen capture failed | any other capture start error (retried every 10 s) | Show Logs… |
| Video encoder error | a VideoToolbox error, until an encode succeeds again | Show Logs… |
| Local Network access denied | a send to the LAN fails with "no route to host" (discovery ANNOUNCE or a Wi-Fi client), which is what macOS 15+ produces when Local Network access is denied | Allow Local Network Access… (opens Privacy & Security › Local Network), Relaunch |
| USB needs Android platform-tools | no adb, while a Meta USB device is plugged in (without one, Headset ▸ explains it and Wi-Fi users are not nagged) | Copy Install Command, Download Platform-Tools… |
| adb isn't working | `adb start-server` fails even after `kill-server` (retried every 30 s), `adb devices` fails, or the server stopped 3 times in 2 minutes | Restart adb, Show Logs… |
| Quest 3 on USB, but USB debugging is off | IOKit sees a Meta USB device (vendor 0x2833) that adb does not list | how to turn on Developer Mode |
| Put on the headset and accept "Allow USB debugging" | adb lists the device as `unauthorized` | — |
| Headset offline on USB | `offline` or `no permissions` | replug hint |
| N adb devices, none is a Quest | several ready devices and none has a Quest model | `--adb-serial` hint |
| Headset app not installed / out of date | the headset's `versionName` vs. the APK bundled in VRScreen.app | Install / Update headset app |
| Headset audio uses the slower capture | the process tap is unavailable while audio is on (System Audio Recording denied → open the pane; granted since → relaunch; other errors → logs) | Grant System Audio Recording…, Relaunch |
| Headset battery low: 12 % | HEADSET_STATUS ≤ 15 % and not on power | — |

The problem list is logged on every change (`problems: …`).

### USB: the headset over adb

The sender drives USB itself (`--no-adb` turns all of this off):

- **adb lookup**: `PATH`, then `$ANDROID_HOME`/`$ANDROID_SDK_ROOT`, `~/Library/Android/sdk`,
  `/opt/homebrew/bin`, `/usr/local/bin` (apps started from Finder get a minimal `PATH`).
  Every adb call runs with `ADB_MDNS=0` (adb 37.0.1's mDNS backend crashes the server)
  and a timeout (3 s; 10 s to start the server; 5 minutes for an install).
- **Server**: started if it isn't running (`adb start-server`, so it inherits
  `ADB_MDNS=0`), restarted if it stops, `kill-server` + `start-server` if it won't start.
- **Headset**: `adb devices -l` every 3 s. A ready device whose model is a Quest wins
  (several devices: the Quest; `--adb-serial` to force one), then a device waiting for
  authorization, then a single ready device of any model.
- **When it appears**: `adb reverse tcp:P tcp:P` (P = `--port`; re-checked on every poll,
  so a reverse lost to an adb restart comes back; an existing one is left alone), the
  installed headset app version is read, and the app is started once (`am start`, if
  "Open headset app when connected" is on; for a non-default port the app is told via
  the `debug.vrscreen.port` property).
- **On exit** (Quit, Ctrl-C, SIGTERM, SIGHUP, logout, or any `exit()` via atexit) only the
  reverse this sender created is removed.

## Logs

Besides stdout, every line goes to `~/Library/Logs/VRScreen/vrscreen.log` (appending, so
a dev sender and the app can share it; rotated to `vrscreen.1.log` at 5 MB, so at most
about 10 MB are kept). Menu → Show Logs… opens it in Console.

## One at a time

A second VRScreen.app exits at once (`VRScreen is already running (pid N); exiting`;
`LSMultipleInstancesProhibited` also stops Finder from starting one). A sender whose port
is taken by another process (the app vs. a dev `vrscreen-sender`) does not exit: it shows
"Port 7777 in use (another VRScreen running?)" and takes over once the port is free;
without the menu (`--no-menu`) it exits instead.

## Audio

With **Audio in headset** on (the default), the sender captures the Mac's system audio
(48 kHz stereo, its own process excluded), converts it to signed 16-bit little-endian
PCM, re-chunks it into 10 ms pieces (480 frames) and sends AUDIO_CONFIG and AUDIO
messages to the connected client (protocol: [AUDIO](../docs/protocol.md)). About
1.55 Mbit/s. Audio runs on its own queue and is not part of the frame windows; chunks
more than 150 ms late are dropped. Switching the checkbox off stops the capture and the
sending at once; the headset drains what it has to silence. `--no-audio` forces it off
and greys out both audio items. No microphone is used.

**Source** (`--audio-source`, default `auto`):

- `tap`: a Core Audio **process tap** (macOS 14.2+), a private global tap of every
  process but the sender, read in 5 ms buffers. A chunk leaves ~10 ms after its first
  sample, i.e. as soon as it is complete. Needs the System Audio Recording permission
  (see below). `--audio-source tap` exits if the tap cannot be created.
- `sck`: the ScreenCaptureKit stream's own audio. Covered by Screen Recording, but it
  hands over 20 ms buffers 15–50 ms after their last sample, so chunks leave 30–70 ms
  after capture.
- `auto`: the tap, else (older macOS, permission denied, an error) ScreenCaptureKit,
  logged once as `audio: process tap unavailable (reason), using ScreenCaptureKit`.

Measured on loopback with the 4K screen captured and a video playing, 60 s each,
arrival − capture of each chunk's first sample: tap p50 10.2 ms, p95 10.5 ms, max 14 ms;
ScreenCaptureKit p50 29 ms, p95 43 ms, max 50 ms in that session (49/65/70 ms in another).

**Timestamps** are the time the Mac would play the sound: capture time plus the default
output device's presentation latency, followed when the output changes (logged: `audio:
Mac output "AirPods" plays 160.0 ms after capture`). Video players send audio that far
ahead of their picture (160 ms for AirPods, 6 ms for the Mac mini's speakers), so this
keeps the headset's sound in sync with the mirrored picture. Chunks can therefore arrive
before their timestamp.

With `--source test`, the audio is synthesized: silence, plus a 60 ms 1 kHz beep
(−10.5 dBFS) starting at exactly the capture time of the pattern's once-per-second white
flash, for A/V sync checks.

**Mac volume and mute do not affect the capture.** Measured: `afplay
/System/Library/Sounds/Ping.aiff` captured through ScreenCaptureKit peaked at the same
0.297 (−10.5 dBFS) with the Mac at volume 38, muted, and at volume 10. So the Mac's own
speakers can be silenced while the headset keeps the sound:

**Mute Mac while mirroring** (default off) silences the Mac while audio is actually going
to a headset (audio on and a client connected), and stops when that stops: checkbox off,
Audio in headset off, headset disconnected, or the sender exiting.

- With the process tap, it uses the tap's own mute (`mutedWhenTapped`): the tapped
  audio is not played on the Mac's outputs while the tap is read. No system setting
  changes, so there is nothing to restore: macOS ends it with the tap, also on a crash or
  `kill -9`.
- With ScreenCaptureKit audio, it mutes the default output device instead. It only
  mutes a device that was not already muted, and only unmutes the device it muted, so a
  Mac you had muted stays muted. If the default output changes while it is engaged, the
  previous device is restored and the new one muted. The unmute runs on every exit path:
  Quit, Ctrl-C, SIGTERM, SIGHUP, normal exit (atexit) and crash signals (SIGSEGV, SIGBUS,
  SIGILL, SIGABRT, SIGFPE, SIGTRAP), like the cursor freeze. Only `kill -9` (or a power
  loss) can leave the Mac muted; unmute it with the volume keys. Devices without a
  settable mute (some HDMI/DisplayPort outputs) are left playing, and the log says so.

## Placing the screen: shortcuts and mouse

While the sender runs, these work from any app and move the screen in the headset
(they send CONTROL messages to the connected client; with no client the sender logs and
drops them):

| Input              | Action                                             | Held down |
|--------------------|----------------------------------------------------|-----------|
| ⌃⌥⌘ ← / →          | Slide the screen left / right (5 cm steps)         | repeats   |
| ⌃⌥⌘ ↑ / ↓          | Slide the screen up / down (5 cm steps)            | repeats   |
| ⌃⌥⌘⇧ ↑ / ↓         | Farther / closer (×1.1 per step)                   | repeats   |
| ⌃⌥⌘⇧ → / ←         | Bigger / smaller (×1.1 per step)                   | repeats   |
| ⌃⌥⌘ R              | Recenter in front of where you look                | —         |
| ⌃⌥⌘ 0              | Reset distance and size, and recenter              | —         |
| ⌃⌥⌘ + move mouse   | Orbit the screen around your head (0.1° per point) | continuous |
| ⌃⌥⌘ + scroll       | Scroll up / away: closer; down / toward you: farther | continuous |
| ⌃⌥⌘⇧ + scroll      | Scroll up / away: bigger; down / toward you: smaller | continuous |

(⌃ control, ⌥ option, ⌘ command, ⇧ shift; step sizes are the Quest app's.)

**Keys.** Held keys repeat after 300 ms, every 80 ms. Shortcuts are physical keys, so
they are the same on QWERTY and AZERTY: arrows, the R key, and the 0 key of the number
row (`à`/`0` on a French keyboard). `[ ] = -` were avoided because AZERTY puts them
behind dead keys or on other keys. They use Carbon `RegisterEventHotKey`, which needs
no Accessibility or Input Monitoring permission. Registration is exclusive: if another
app (or a second sender) already owns a combination, the sender logs
`hotkeys: could not register ⌃⌥⌘← (taken by another app)` at startup and keeps running
without it. Every press is logged, e.g. `hotkey ⌃⌥⌘→ → TRANSLATE dx=+1 dy=0 → 192.168.1.42:51234`.

**Mouse and scroll.** Orbit uses the mouse's movement deltas, not the cursor position,
and only while exactly ⌃⌥⌘ is held (with ⇧, the mouse does nothing and scroll resizes).
Scroll follows the physical gesture whatever the "natural scrolling" setting: a wheel
notch is a quarter step (×1.1^0.25), a 200-point trackpad swipe one step; trackpad
inertia after lifting the fingers is ignored. Input is coalesced to at most 60 messages
per second, and the log shows one summary line per second of activity (e.g.
`mouse: ORBIT ×42 Σ yaw +12.30° pitch -1.10°`).

- Without Accessibility (the normal case) the sender only observes mouse and scroll
  with a global event monitor, which needs no permission: the startup log says
  `mouse: observe-only (scroll passes through to apps)`. Apps under the cursor still
  receive the scroll, so scroll over something that ignores it (the desktop, a window's
  title bar). While exactly ⌃⌥⌘ is held the cursor is frozen in place
  (`CGAssociateMouseAndMouseCursorPosition`), so orbiting doesn't drag it across the
  screen; it is released when the keys are, and on every exit path (normal exit,
  Ctrl-C, SIGTERM/SIGHUP, crash signals). Only `kill -9` can't be covered.
- If the app running the sender (VRScreen.app, or the terminal) already has
  Accessibility permission (the sender never asks for it), it uses an event tap instead
  and swallows those mouse and scroll events while ⌃⌥⌘ is held: `mouse: event tap,
  events swallowed …`.
- The modifier state is polled 60 times a second from the session (no permission
  needed). Controls ▸ Mouse orbit & scroll zoom (or `--no-mouse`) turns all of this off.

The sender runs an AppKit event loop for the menu, shortcuts and mouse as a
background (accessory) app: a menu-bar icon, but no Dock icon and no app menu.

## Screen Recording permission

macOS grants Screen Recording to the app that runs the sender: VRScreen.app itself, or,
for a bare `vrscreen-sender`, the terminal app it was started from (Terminal, iTerm, your
IDE...). VRScreen.app asks on its first start; the menu shows "Screen Recording not
allowed" with a button to the right System Settings pane until it is granted, and
macOS may require relaunching it (Menu → Relaunch VRScreen). For a terminal:

1. System Settings → Privacy & Security → Screen & System Audio Recording.
2. Enable the terminal app you run the sender from.
3. Quit and restart that terminal app.

Recent macOS versions also periodically show a prompt asking whether that app may keep
capturing the screen while bypassing the system's private window picker. That
decision is yours; capture keeps working while the prompt is open. `--source test`
needs no permission at all.

**System Audio Recording** (for the process-tap audio source) is a separate permission,
granted to the same app (its Info.plist must have `NSAudioCaptureUsageDescription`;
VRScreen.app, Terminal and Tern do; a bare binary needs none). macOS asks once, the
first time the sender starts the tap ("… would like to access your system's audio"); the
start waits for the answer (the video keeps running). The app then appears under Screen
& System Audio Recording → System Audio Recording Only. The sender checks the permission
with TCC's read-only preflight before creating the tap: if it was denied, it logs
`audio: process tap unavailable (System Audio Recording permission denied …), using
ScreenCaptureKit`, uses ScreenCaptureKit audio (covered by Screen Recording) and lists
the problem in the menu.

**Local Network** (macOS 15+): VRScreen.app asks the first time it answers the headset's
discovery over Wi-Fi. If it is denied, Wi-Fi can't work (USB still does); the menu
shows it once a send to the LAN fails.

## Using it with the Quest

Install VRScreen.app (top-level README) and start it, or run the sender from source:

```sh
scripts/mirror.sh                  # rebuild if needed, then the sender (USB + Wi-Fi)
scripts/mirror.sh --install        # also build and install the headset app first (USB)
scripts/mirror.sh --no-launch --codec h264   # extra args go to the sender
scripts/mirror.sh --no-adb         # never touch adb; the headset finds the Mac over Wi-Fi
```

`scripts/mirror.sh` only builds (the sender when its sources changed; with `--install`
the headset app, then `adb install -r`) and execs the sender with the remaining
arguments. The sender does the rest: [USB](#usb-the-headset-over-adb) (reverse, launch,
cleanup) and LAN discovery (UDP 7778, `--discovery-port`).

By hand, USB is:

```sh
adb reverse tcp:7777 tcp:7777
.build/release/vrscreen-sender --no-adb
```

The Quest app tries `127.0.0.1:7777` first (the adb tunnel). Over Wi-Fi it broadcasts
DISCOVER on UDP 7778 and connects to the first Mac that answers; the sender answers by
default. Test it Mac-only with `vrscreen-viewer --discover`.

Measured on a 5 GHz network to a Quest 3: ping RTT avg 11 ms at 5 pings/s (36 ms at
1/s, with spikes over 100 ms, likely Wi-Fi power saving), TCP about 285 Mbit/s, versus
about 550 Mbit/s through the USB tunnel.

## Design notes

- **Pixel format and colour**: capture is 420v (8-bit 4:2:0, BT.709 limited/video
  range); the VUI says BT.709 primaries/transfer/matrix, limited range.
- **Encoder**: `kVTVideoEncoderSpecification_EnableLowLatencyRateControl` is requested
  for both codecs. On Apple silicon this selects the real-time encoders
  (`com.apple.videotoolbox.videoencoder.hevc.rtvc` / `h264.rtvc`); if a machine rejects
  it the sender falls back to standard rate control and logs it. No B-frames,
  real-time mode, HEVC Main / H.264 Constrained High.
- **Keyframes**: forced for every newly connected client (re-encoding the latest
  capture, so a static screen shows up at once), and at most every 5 s while frames
  flow. Keyframes carry the parameter sets.
- **Backpressure**: encoded frames are never dropped; while a client's window is full,
  new captures are not encoded, only the newest is kept and encoded once the window
  opens. Two windows apply:
  - *FRAME_ACK* (clients that acknowledge, like this viewer and the Quest app): at most
    `--max-in-flight` (3) frames sent but not yet consumed. Needed over USB: adb's
    tunnel buffers about 3.5 MB before the sender's socket blocks (measured), i.e.
    seconds of video at desktop bitrates that the socket alone cannot see.
  - *Socket backlog* (all clients): at most two encoded frames unsent, plus a fixed
    send buffer on a plain BSD socket: 128 KiB (the latency bound for clients that
    never ack), raised to 512 KiB once a client acks (128 KiB caps Wi-Fi TCP at about
    120 Mbit/s). `NWConnection` was not used because it reports sends as done once they
    reach the auto-tuned kernel buffer (up to 4 MiB).
- **Encoding thread**: the low-latency encoder encodes synchronously inside
  `VTCompressionSessionEncodeFrame` (~15 ms at 4K on an M-series Mac), so it runs on its
  own queue, one frame at a time, and does not delay networking or PONGs.
- **Audio path**: the process tap is read through a private aggregate device that
  contains only the tap, in 240-frame (5 ms) IO buffers (any size from 15 to 960 frames
  is accepted). The aggregate deliberately has no output device: with the default output
  as its clock device, the tap's input is aligned to that device's latency and arrives
  141 ms later than ScreenCaptureKit's with AirPods (measured by cross-correlating the
  two captures of the same audio); without one, its timestamps match ScreenCaptureKit's
  to the millisecond and the data comes ~13 ms earlier. ScreenCaptureKit (fallback)
  delivers Float32 planar 20 ms buffers 15–50 ms after their last sample, the level set
  per capture session (measured the same with and without 4K video in the stream). A
  re-chunker converts to s16le and cuts 10 ms chunks whose timestamps follow the
  capture clock (a buffer more than 5 ms off the expected time is a discontinuity: the
  partial chunk is dropped and timing restarts), then adds the output's presentation
  latency. Queued AUDIO more than 150 ms past its timestamp is removed from the client's
  send queue before it reaches the socket; audio already in the kernel buffer cannot be
  recalled. AUDIO overtakes FRAMEs still waiting unstarted in the send queue.
