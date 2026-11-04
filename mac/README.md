# Quest Mirror — Mac side

Mirrors the Mac's display to a Quest 3 (view-only). This package contains:

- `quest-mirror-sender`: captures a display with ScreenCaptureKit, encodes it with
  VideoToolbox (HEVC or H.264, hardware, low latency) and serves it over TCP.
- `quest-mirror-viewer`: a Mac test client (window or headless) that speaks the same
  protocol as the Quest app.
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
.build/release/quest-mirror-sender

# Sender with the synthetic test pattern (no permission needed)
.build/release/quest-mirror-sender --source test

# Viewer in a window
.build/release/quest-mirror-viewer

# Viewer headless: stats only, one PNG snapshot, then exit
.build/release/quest-mirror-viewer --headless --snapshot /tmp/mirror.png --exit-after-snapshot
```

Stop either with Ctrl-C.

### Sender options

| Option                     | Default | Meaning                                                      |
|----------------------------|---------|--------------------------------------------------------------|
| `--source screen\|test`    | screen  | ScreenCaptureKit capture, or the synthetic test pattern       |
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
| `--debug-control-on-connect SPEC` | none | Testing: send CONTROL commands to each client on connect, e.g. `"move:1,0;distance:-2;size:1;recenter;reset"` |

Once per second it logs capture fps, encoded fps (and keyframes), captures skipped by
backpressure, the bitrate actually sent, and the client with its unsent backlog and,
for clients that send FRAME_ACK, frames in flight (current/limit and the per-second max).

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

Once per second it logs received and decoded fps, bitrate, capture→decoded latency
(avg/max, measured through the PING/PONG clock offset the way the Quest client does
it), the last PING round trip, and the stream format. It acknowledges every FRAME
(FRAME_ACK) as it hands it to the decoder, and reconnects every second while the
sender is unreachable or after the sender restarts. CONTROL messages are printed
(`control: MOVE dx=+1 dy=0`).

## Placing the screen: global shortcuts

While the sender runs, these shortcuts work from any app and move the screen in the
headset (they send CONTROL messages to the connected client; with no client the
sender logs and drops them):

| Shortcut     | Action                                   | Held down |
|--------------|------------------------------------------|-----------|
| ⌃⌥⌘ ← / →    | Move the screen left / right (5° steps)  | repeats   |
| ⌃⌥⌘ ↑ / ↓    | Move the screen up / down (5° steps)     | repeats   |
| ⌃⌥⌘⇧ ↑ / ↓   | Farther / closer (×1.1 per step)         | repeats   |
| ⌃⌥⌘⇧ → / ←   | Bigger / smaller (×1.1 per step)         | repeats   |
| ⌃⌥⌘ R        | Recenter in front of where you look      | —         |
| ⌃⌥⌘ 0        | Reset distance and size, and recenter    | —         |

(⌃ control, ⌥ option, ⌘ command, ⇧ shift; step sizes are the Quest app's.) Held keys
repeat after 300 ms, every 80 ms. Shortcuts are physical keys, so they are the same on
QWERTY and AZERTY: arrows, the R key, and the 0 key of the number row (`à`/`0` on a
French keyboard). `[ ] = -` were avoided because AZERTY puts them behind dead keys or
on other keys.

They use Carbon `RegisterEventHotKey`, which needs no Accessibility or Input
Monitoring permission. The sender runs an AppKit event loop for them as a background
(accessory) app: no Dock icon, no menu bar. Registration is exclusive: if another app
(or a second sender) already owns a combination, the sender logs
`hotkeys: could not register ⌃⌥⌘← (taken by another app)` at startup and keeps running
without it. Every press is logged, e.g. `hotkey ⌃⌥⌘→ → MOVE dx=+1 dy=0 → 192.168.1.35:51234`.

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
  `debug.questmirror.port` property) and launches the app unless `--no-launch`;
  without a usable USB headset it carries on for Wi-Fi;
- always runs the sender with LAN discovery (UDP 7778, `--discovery-port`) and prints
  how the headset will connect (USB tunnel first, else Wi-Fi to the Mac's addresses);
- on Ctrl-C stops the sender and removes the reverse it created (a reverse that already
  existed is left alone).

By hand, USB is:

```sh
adb reverse tcp:7777 tcp:7777
.build/release/quest-mirror-sender
```

The Quest app tries `127.0.0.1:7777` first (the adb tunnel); re-run `adb reverse` after
reconnecting the headset or restarting adb. Over Wi-Fi it broadcasts DISCOVER on UDP
7778 and connects to the first Mac that answers; the sender answers by default. Test it
Mac-only with `quest-mirror-viewer --discover`.

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
