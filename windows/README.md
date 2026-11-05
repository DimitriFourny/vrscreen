# VRScreen — Windows side

VRScreen for Windows mirrors a PC display and its sound to the VRScreen headset app on a
Meta Quest. It speaks the same wire protocol as the Mac sender
([`../docs/protocol.md`](../docs/protocol.md)), so the headset app is the same. Like the Mac
sender, it is view-only: you keep using your real keyboard, mouse or gamepad.

It is a native C++ tray app with no third-party dependencies:

| Stage | Windows API |
|---|---|
| Screen capture | DXGI Desktop Duplication (DWM present timestamps; the mouse pointer drawn in) |
| Scaling + colour conversion | Direct3D 11 video processor: BGRA → NV12, BT.709 limited range, on the GPU |
| Encoding | Media Foundation hardware encoder of the same GPU (NVIDIA NVENC, AMD, Intel Quick Sync), HEVC Main or H.264 High, low-latency mode, no B-frames, straight from the GPU texture |
| Audio | WASAPI loopback of the default output, re-chunked to 10 ms s16le |
| Network | Winsock TCP server :7777 with the Mac's flow control (FRAME_ACK window, bounded send buffer), UDP discovery :7778 |
| USB | `adb reverse`, launch and install of the headset app, travel mode (same logic as the Mac) |
| UI | Notification-area (tray) icon and menu, `Ctrl+Alt+Win` shortcuts and mouse gestures |

## Measured (RTX 4070, Windows 11, loopback to the test viewer on the same PC)

| Stream | Encode | Capture → received |
|---|---|---|
| Desktop 5K scaled to 3840×2160, 60 fps, HEVC | 8 ms | 14–15 ms |
| Desktop scaled to 1920×1080, 60 fps, HEVC | 3 ms | 7–8 ms |
| Test pattern 2560×1440, 120 fps, HEVC | 4.4 ms | 5.7 ms |

Add the network and the headset's decode for the full picture (see the top-level README).

## Requirements

- Windows 10 version 2004 or later, or Windows 11, 64-bit.
- A GPU with a hardware video encoder and its vendor driver (NVIDIA, AMD or Intel from the last
  several years). Tested: NVIDIA GeForce RTX 4070. HEVC needs a GPU that encodes HEVC; otherwise
  pick H.264 in the Video menu.
- For USB: Android platform-tools (`adb`) and, on many PCs, the Meta Quest ADB driver (below).
  Without them, Wi-Fi only.

## Build

Once: the Visual Studio Build Tools with the C++ workload (compiler, Windows SDK, CMake, Ninja):

```powershell
winget install Microsoft.VisualStudio.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
```

Then, from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File windows\build.ps1          # Release build + unit tests
powershell -ExecutionPolicy Bypass -File windows\build.ps1 -Dist    # also build\VRScreen-<version>-windows.zip
```

Binaries land in `windows\build\Release\`: `vrscreen-sender.exe` (the app), `vrscreen-viewer.exe`
(a test client) and `vrscreen-tests.exe`. The executables are statically linked (no Visual C++
redistributable needed). `-Dist` copies the sender as `VRScreen.exe` and, if the headset app has
been built (`quest\app\build\outputs\apk\debug\app-debug.apk`) or put next to the script as
`windows\vrscreen-headset.apk`, bundles it as `vrscreen-headset.apk` for the Headset menu.

The tray/app icon comes from the Mac icon (`windows\scripts\make-ico.ps1` regenerates
`windows\resources\vrscreen.ico`).

## Install and first start

1. Put `VRScreen.exe` (or `vrscreen-sender.exe`) somewhere permanent, e.g.
   `%LOCALAPPDATA%\Programs\VRScreen\`, with `vrscreen-headset.apk` next to it if you have it.
   Windows Firewall rules and "Start with Windows" are tied to that path.
2. Start it. It lives in the notification area (click the `^` next to the clock if it is hidden;
   drag it onto the taskbar to keep it visible). On the first start Windows Firewall asks whether
   VRScreen may communicate on networks: **allow it on private networks** (needed for Wi-Fi only).
3. **Wi-Fi**: the PC and the headset must be on the same network, and Windows must consider it a
   *Private* network (Settings → Network & internet → your Wi-Fi/Ethernet → Private network). If
   the firewall blocks VRScreen, the menu says so at the top with a fix ("Allow ... (administrator)",
   which adds a rule for this program from the local subnet only).
4. **USB** (lowest latency): install platform-tools (`winget install Google.PlatformTools`, then
   restart VRScreen), plug in the headset with a USB-C data cable and accept "Allow USB debugging"
   inside the headset. If the menu says the Quest is on USB but adb can't see it, install the
   [Meta Quest ADB driver](https://developers.meta.com/horizon/downloads/package/oculus-adb-drivers/)
   (and check Developer Mode is on). VRScreen then sets up `adb reverse` and starts the headset app.
5. Optionally turn on **Start with Windows** in the menu.

The headset app is the one from `quest/` (install it from the Mac app, with **Headset ▸ Install
headset app** when the APK is next to the exe, or with `adb install -r vrscreen-headset.apk`).

## Using it for games

- **Borderless/windowed fullscreen** works best. Exclusive fullscreen also works on Windows 10/11
  (fullscreen optimizations), but a mode switch interrupts capture for a moment.
- **Frame rate**: Video ▸ 60 / 90 / 120 fps. Capture can't exceed the monitor's refresh rate, and
  frames are only sent when the picture changes. The Quest 3 shows at most its own refresh rate.
- **Resolution / bitrate**: 1920 or 2560 px wide at 50–80 Mbit/s over USB is a good gaming
  setting (lower encode and decode time than 4K). HEVC is the default: H.264 decodes about 3.5×
  slower at 4K on the Quest 3.
- **HDR**: a display in HDR mode is captured as SDR (8-bit, BT.709); colours may look washed out.
  Turn HDR off for the mirrored display if that bothers you.
- **Input**: VRScreen never touches game input. The low-level mouse hook used for the
  `Ctrl+Alt+Win` gestures is only installed while those three keys are held.
- **Protected content** (DRM video, some anti-cheat overlays) shows as black, like any screen
  recorder.

## Controls

Global shortcuts (`RegisterHotKey`, no permission needed). They are the Mac's ⌃⌥⌘ shortcuts with
**Ctrl+Alt+Win**:

| Input | Action |
|---|---|
| Ctrl+Alt+Win + arrows | Slide the screen left / right / up / down (repeats while held) |
| Ctrl+Alt+Win+Shift + ↑ ↓ / → ← | Distance / size in steps |
| Ctrl+Alt+Win + R | Re-center in front of where you look |
| Ctrl+Alt+Win + 0 | Reset to the default placement |
| Ctrl+Alt+Win + mouse move | Orbit the screen around your head (the cursor stays put) |
| Ctrl+Alt+Win + wheel | Closer / farther (a notch is a quarter step) |
| Ctrl+Alt+Win+Shift + wheel | Bigger / smaller |

If another app already owns a combination, the log says `hotkeys: could not register ...`.

## Tray menu

Problems (each with its fix) · connection status · headset battery · travel mode · Curved screen
and Dim surroundings (0–100 %) · Hand gestures · Audio in headset · Mute PC while mirroring ·
**Video ▸** resolution (3840 / 2560 / 1920 px wide), frame rate (120 / 90 / 60 / 30), codec
(HEVC / H.264), bitrate (15 / 30 / 50 / 80 Mbit/s), display (with 2+ displays) · **Controls ▸**
shortcuts, mouse gestures, sensitivities · **Headset ▸** (USB) install/update/launch the headset
app, Quick Settings (travel mode), restart adb, open headset app when connected · Re-center ·
Reset placement · Start with Windows · Allow Wi-Fi through Windows Firewall · Show Logs · About ·
Quit.

Problems it detects: port in use (another VRScreen), screen capture failure, encoder error,
Wi-Fi blocked by Windows Firewall (and whether the network is Public), adb missing while a Quest
is plugged in, Quest on USB but invisible to adb (Developer Mode / ADB driver), "Allow USB
debugging" pending, headset offline, several adb devices, headset app missing or out of date, no
audio device, headset battery low.

## Settings

`%APPDATA%\VRScreen\settings.json`, written half a second after a change. The keys match the Mac
sender's (`curvature`, `dim`, `handGestures`, `audio`, `maxWidth`, `fps`, `codec`, `bitrateMbps`,
`hotkeys`, `mouseGestures`, `mouseSensitivity`, `scrollSensitivity`, `launchHeadsetApp`) plus
`mutePC` and `display` (a GDI device name like `\\.\DISPLAY2`; absent = primary display). Editing
the file applies it at once (the Mac sender's `kill -USR1`); invalid values take their default.

## Command line

`vrscreen-sender --help` lists everything. The same session overrides and developer options as
the Mac sender (`--fps`, `--max-width`, `--codec`, `--bitrate`, `--no-hotkeys`, `--no-mouse`,
`--no-launch`, `--source test`, `--port`, `--discovery-port`, `--no-discovery`,
`--keyframe-interval`, `--max-in-flight`, `--no-menu`, `--no-audio`, `--settings`, `--no-adb`,
`--adb-serial`, `--debug-control-on-connect`), plus:

| Option | Meaning |
|---|---|
| `--console` | Show the log in a console (the sender is a GUI app; it otherwise logs to the file only) |
| `--list-displays` / `--display N` | List the displays / capture display N for this session |
| `--audio-offset-ms MS` | Add MS to audio timestamps (default 0, see Audio) |
| `--encoder NAME` | Prefer the hardware encoder whose name contains NAME |

A second VRScreen on the same port exits at once. To test next to a running VRScreen, use other
ports and settings: `--port 7790 --discovery-port 7791 --no-adb --no-hotkeys --no-mouse --settings %TEMP%\test.json`.

## Test viewer

`vrscreen-viewer` is a headless client like the Mac's: hello check, CONFIG/FRAME/CONTROL/AUDIO,
PING/PONG clock offset, FRAME_ACK, optional software decoding with Media Foundation.

```powershell
vrscreen-viewer --port 7790 --duration 10                       # stats once per second
vrscreen-viewer --discover --snapshot shot.png --exit-after-snapshot
vrscreen-sender --source test --console                         # then:
vrscreen-viewer --av-sync --audio-wav out.wav --duration 10     # audio - video per flash/beep pair
```

Decoding HEVC needs the "HEVC Video Extensions" from the Microsoft Store (the sender does not).

## Logs

`%LOCALAPPDATA%\VRScreen\Logs\vrscreen.log` (UTF-8, rotated to `vrscreen.1.log` at 5 MB), opened
by Show Logs…. Once per second: capture fps, encoded fps and keyframes, encode time, captures
skipped by backpressure, Mbit/s, audio chunks and drops, and the client's backlog and frames in
flight.

## Audio

The default output device is captured with WASAPI loopback (whatever its format: the channels are
mixed down to stereo, the rate is kept, e.g. 48000 Hz) and sent in 10 ms chunks. A silent stream
is played to the same device so the capture keeps a continuous timeline while nothing plays.
Switching the default output device is followed within a second.

**Timestamps**: WASAPI stamps loopback audio with its playback time on the same clock as the
video (QueryPerformanceCounter), a few ms ahead of delivery. Unlike the Mac sender, no extra
output latency is added (games do not delay their picture to match a Bluetooth headset's delay);
`--audio-offset-ms` adds one if you want it.

**Mute PC while mirroring** mutes the default output device while audio goes to a headset; the
loopback capture is unaffected (measured: same level muted and unmuted). It only unmutes a device
it muted itself, when the headset disconnects, the option or audio is switched off, the output
device changes, or VRScreen quits. A VRScreen killed from Task Manager while muting leaves the PC
muted: unmute it with the volume keys.

## Design notes

- **One D3D11 device** on the captured display's adapter serves capture, conversion and the
  encoder (`MFT_ENUM_ADAPTER_LUID`), so frames never leave the GPU. Desktop Duplication's
  `AcquireNextFrame` holds the device lock while it waits, which stalled the encoder for the whole
  timeout (50–150 ms per frame at 4K); it is therefore polled without waiting, once per ms.
- **Frame pacing**: Desktop Duplication delivers every DWM present. Frames are delivered at most
  `fps` times per second on a steady cadence (a 144 Hz desktop gives 60 fps, not 48), and the
  newest picture of a burst is never lost: a skipped one goes out when its slot comes.
- **Cursor**: Desktop Duplication reports the pointer separately; it is composed onto the frame
  (colour pointers alpha-blended, monochrome/masked-colour inverting pixels with an XOR-like
  blend), so it shows in the headset as on the Mac.
- **Backpressure** is the Mac's: never drop encoded frames; while the window is full keep only the
  newest capture. Winsock accepts a whole non-blocking send whatever `SO_SNDBUF` says, so sends go
  out in 64 KiB slices to keep "frame fully written" meaningful.
- **Encoder**: Media Foundation async hardware MFT, `CODECAPI_AVLowLatencyMode`, CBR at the chosen
  bitrate, GOP = fps × keyframe interval, keyframes forced with `CODECAPI_AVEncVideoForceKeyFrame`.
  Access units are rebuilt to the protocol's form (4-byte start codes, no AUDs, keyframes start
  with VPS/SPS/PPS).
