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
| `--fps N`                  | 60      | Target frame rate                                            |
| `--max-width N`            | 3840    | Scale the display's native pixel size down to fit this width |
| `--codec hevc\|h264`       | hevc    | Video codec                                                  |
| `--bitrate MBPS`           | 30      | Average bitrate, Mbit/s                                      |
| `--keyframe-interval S`    | 5       | Maximum seconds between keyframes                            |
| `--width N --height N`     | 1920x1080 | Test-pattern size                                          |

Once per second it logs capture fps, encoded fps (and keyframes), captures skipped by
backpressure, the bitrate actually sent, and the client with its unsent backlog.

### Viewer options

| Option                  | Default   | Meaning                                  |
|-------------------------|-----------|------------------------------------------|
| `--host HOST`           | 127.0.0.1 | Sender address                           |
| `--port N`              | 7777      | Sender port                              |
| `--headless`            | off       | No window; decode and print stats        |
| `--snapshot PATH.png`   | none      | Write the first decoded frame as PNG     |
| `--exit-after-snapshot` | off       | Exit once the snapshot is written        |
| `--duration S`          | none      | Exit after S seconds                     |

Once per second it logs received and decoded fps, bitrate, capture→decoded latency
(avg/max, measured through the PING/PONG clock offset the way the Quest client does
it), the last PING round trip, and the stream format. It reconnects every second
while the sender is unreachable or after the sender restarts.

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

USB (lowest latency, no Wi-Fi involved):

```sh
adb reverse tcp:7777 tcp:7777
.build/release/quest-mirror-sender
```

The Quest app then connects to `127.0.0.1:7777`; adb forwards it to the Mac. Re-run
`adb reverse` after reconnecting the headset or restarting adb.

Wi-Fi: start the sender and point the Quest app at the Mac's IP address, port 7777.
macOS may ask to allow incoming connections the first time.

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
- **Backpressure**: the sender uses a plain BSD socket with a fixed 128 KiB send
  buffer instead of `NWConnection`: NWConnection reports sends as done once they reach
  the auto-tuned kernel buffer (up to 4 MiB), which hid seconds of backlog in testing.
  While two encoded frames are still unsent, new captures are not encoded; only the
  newest is kept and encoded once the backlog drains. Encoded frames are never dropped.
- **Encoding thread**: the low-latency encoder encodes synchronously inside
  `VTCompressionSessionEncodeFrame` (~15 ms at 4K on an M-series Mac), so it runs on its
  own queue, one frame at a time, and does not delay networking or PONGs.
