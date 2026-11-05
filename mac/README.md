# VRScreen — Mac side

VRScreen — Mac screen mirroring for Meta Quest. This package is the Mac side: it
mirrors the Mac's display to a Quest 3 (view-only) and contains:

- `vrscreen-sender`: captures a display and the system audio with ScreenCaptureKit,
  encodes the picture with VideoToolbox (HEVC or H.264, hardware, low latency) and
  serves both over TCP.
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

With only the Command Line Tools installed, SwiftPM does not find the swift-testing
macro plugin on its own; `Package.swift` adds the CLT plugin path to the test target
in that setup (and only then).

## Run

```sh
# Sender: capture the main display (needs Screen Recording permission, see below)
.build/release/vrscreen-sender

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

| Option                     | Default | Meaning                                                      |
|----------------------------|---------|--------------------------------------------------------------|
| `--source screen\|test`    | screen  | ScreenCaptureKit capture, or the synthetic test pattern (flashes white once per second, with a 1 kHz beep at the flash while audio is on) |
| `--display N`              | main    | Display index, from the list printed at startup              |
| `--port N`                 | 7777    | TCP listen port (all interfaces)                             |
| `--discovery-port N`       | 7778    | UDP port answering LAN discovery (DISCOVER → ANNOUNCE)       |
| `--no-discovery`           | off     | Do not answer LAN discovery                                  |
| `--fps N`                  | 60      | Target frame rate                                            |
| `--max-width N`            | 3840    | Scale the display's native pixel size down to fit this width |
| `--codec hevc\|h264`       | hevc    | Video codec                                                  |
| `--bitrate MBPS`           | 30      | Average bitrate, Mbit/s                                      |
| `--keyframe-interval S`    | 5       | Maximum seconds between keyframes                            |
| `--max-in-flight N`        | 3       | For clients that send FRAME_ACK: max frames sent but not yet consumed |
| `--width N --height N`     | 1920x1080 | Test-pattern size                                          |
| `--no-hotkeys`             | off     | Do not register the global screen-placement shortcuts        |
| `--no-mouse`               | off     | Do not use ⌃⌥⌘ + mouse (orbit) / scroll (zoom)               |
| `--no-menu`                | off     | No menu-bar icon                                             |
| `--no-audio`               | off     | Never capture or send audio, whatever the menu says          |
| `--audio-source auto\|tap\|sck` | auto | System audio from the Core Audio process tap or ScreenCaptureKit; `auto` = tap, else ScreenCaptureKit (see [Audio](#audio)) |
| `--settings PATH`          | standard | Settings file to read/write instead of `~/Library/Application Support/VRScreen/settings.json` (e.g. a second sender for testing) |
| `--adb-serial S`           | none    | Headset on USB for the menu's travel-mode items (`scripts/mirror.sh` passes it) |
| `--mouse-sensitivity DEG`  | 0.1     | Orbit degrees per point of mouse movement                    |
| `--scroll-sensitivity X`   | 1       | Scroll zoom multiplier (a wheel notch is a quarter step)     |
| `--debug-control-on-connect SPEC` | none | Testing: CONTROL commands sent to each client on connect, paced at 60 Hz; `wait:MS` pauses; e.g. `"translate:1,0;orbit:50,0*50;curvature:500;wait:3000;dim:1000"` |

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
follows light/dark mode; "QM" if the symbol is missing). Its menu has:

- a status line: `Headset: USB (127.0.0.1)` (the adb tunnel), `Headset: Wi-Fi <ip>`
  or `No headset connected`, updated live, also while the menu is open;
- **Curved screen** 0–100 %: 0 = flat, 100 % = the screen wraps around you on a
  cylinder whose radius is the viewing distance;
- **Dim surroundings** 0–100 %: darkens the passthrough around the screen, up to
  black; the screen itself is never dimmed;
- **Hand gestures** (checkbox, default off): lets you pinch & hold to move the screen
  and use two hands to resize it; while off, the headset ignores hand input entirely;
- **Audio in headset** (checkbox, default on) and **Mute Mac while mirroring**
  (checkbox, default off), see [Audio](#audio);
- **Re-center** and **Reset placement** (like ⌃⌥⌘R / ⌃⌥⌘0);
- **Travel mode: On / Off** and **Open Quick Settings in headset…** (USB only, see below);
- **Quit VRScreen**: the same clean shutdown as Ctrl-C (cursor released, Mac output
  unmuted if the sender muted it; when started by `scripts/mirror.sh`, the script then
  removes its adb reverse).

**Travel mode** (for using the headset in a moving vehicle) can't be switched from
the Mac: Horizon OS only lets the headset's own Quick Settings toggle it. The menu
shows its state, read with `adb shell dumpsys tracking` (the ` * Travel Mode:` line),
and "Open Quick Settings in headset…" opens Quick Settings on the headset
(`am start -a com.oculus.action.QUICK_SETTINGS`), where you tap the Travel mode tile.
The state is read when the menu opens, every 3 s while it stays open, and 3, 6 and 10 s
after opening Quick Settings. Each adb call runs in the background with a 3 s timeout,
always with `ADB_MDNS=0`, and only if an adb server is already running: the sender
never starts or stops the server. This needs the headset's serial (`--adb-serial`,
which `scripts/mirror.sh` passes when the headset is on USB) and `adb` on PATH (or in
`~/Library/Android/sdk/platform-tools`, `/opt/homebrew/bin`, `/usr/local/bin`).
Otherwise the line explains why ("headset not on USB", "adb not found", "adb server
not running", or "unavailable (dumpsys format changed)" if a Horizon OS update changes
the output) and the Quick Settings item is disabled. Over Wi-Fi (`--wifi-only`) the
items show "headset not on USB".

The sliders update the headset while you drag (at most 30 messages per second, the
final value always sent); the checkboxes apply at once. The sender owns these
settings: it sends curvature, dimming and hand gestures to every headset that connects
and stores everything in `~/Library/Application Support/VRScreen/settings.json`
(`{"curvature": 0-1000, "dim": 0-1000, "handGestures": true|false, "audio": true|false,
"muteMac": true|false}`, permille; written half a second after the last change, read at
startup; defaults 0, 0, off, on and off; a key missing from an older file takes its
default). The stored values apply even with `--no-menu`.

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
- If the terminal app running the sender already has Accessibility permission (the
  sender never asks for it), it uses an event tap instead and swallows those mouse and
  scroll events while ⌃⌥⌘ is held: `mouse: event tap, events swallowed …`.
- The modifier state is polled 60 times a second from the session (no permission
  needed). `--no-mouse` turns all of this off.

The sender runs an AppKit event loop for the menu, shortcuts and mouse as a
background (accessory) app: a menu-bar icon, but no Dock icon and no app menu.

## Screen Recording permission

macOS grants Screen Recording to the app that launches the sender (Terminal, iTerm,
your IDE...), not to the binary. If capture fails with a permission error or no
displays:

1. System Settings → Privacy & Security → Screen & System Audio Recording.
2. Enable the terminal app you run the sender from.
3. Quit and restart that terminal app.

Recent macOS versions also periodically show a prompt asking whether that app may keep
capturing the screen while bypassing the system's private window picker. That
decision is yours; capture keeps working while the prompt is open. `--source test`
needs no permission at all.

**System Audio Recording** (for the process-tap audio source) is a separate permission,
also granted to the terminal app (its Info.plist must have `NSAudioCaptureUsageDescription`;
Terminal and Tern do; the sender binary needs none). macOS asks once, the first time the
sender starts the tap ("… would like to access your system's audio"); the start waits
for the answer (the video keeps running). The app then appears under Screen & System
Audio Recording → System Audio Recording Only. The sender checks the permission with
TCC's read-only preflight before creating the tap: if it was denied, it logs `audio:
process tap unavailable (System Audio Recording permission denied …), using
ScreenCaptureKit` and uses ScreenCaptureKit audio (covered by Screen Recording).

## Using it with the Quest

One command from the repo root, for USB and Wi-Fi alike:

```sh
scripts/mirror.sh                  # headset on USB: adb reverse + launch app; always: sender with discovery
scripts/mirror.sh --install        # also build and install the Quest app first (USB)
scripts/mirror.sh --no-launch --codec h264   # app already running; extra args go to the sender
scripts/mirror.sh --wifi-only      # never touch adb; the headset finds the Mac over Wi-Fi
```

The script:

- sets `ADB_MDNS=0` (adb 37.0.1's mDNS backend crashes the adb server);
- if exactly one authorized headset is attached over USB (`--serial` to choose), sets up
  `adb reverse tcp:7777 tcp:7777` (`--port` to change; the app is then told via the
  `debug.vrscreen.port` property) and launches the app unless `--no-launch`;
  without a usable USB headset it carries on for Wi-Fi;
- always runs the sender with LAN discovery (UDP 7778, `--discovery-port`) and prints
  how the headset will connect (USB tunnel first, else Wi-Fi to the Mac's addresses);
- on Ctrl-C stops the sender and removes the reverse it created (a reverse that already
  existed is left alone).

By hand, USB is:

```sh
adb reverse tcp:7777 tcp:7777
.build/release/vrscreen-sender
```

The Quest app tries `127.0.0.1:7777` first (the adb tunnel); re-run `adb reverse` after
reconnecting the headset or restarting adb. Over Wi-Fi it broadcasts DISCOVER on UDP
7778 and connects to the first Mac that answers; the sender answers by default. Test it
Mac-only with `vrscreen-viewer --discover`.

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
