# VRScreen wire protocol, version 1

This is the contract between the Mac sender (`vrscreen-sender`) and any client:
the Mac test viewer (`vrscreen-viewer`) and the Quest app (C++/NDK,
`AMediaCodec`). A client can be written from this document alone.

## Transport

- Plain TCP. The **sender is the server**: it listens on `0.0.0.0`, port **7777** by
  default (`--port`), with `TCP_NODELAY` set.
- Over USB: run `adb reverse tcp:7777 tcp:7777` on the Mac; the Quest app connects to
  `127.0.0.1:7777`. Over Wi-Fi: the Quest app connects to `<Mac IP>:7777`, found with
  the UDP discovery below. The byte stream is identical in both cases.
- **One active client.** When a new client connects, the server closes the
  previous connection. Clients should reconnect automatically (the Mac viewer
  retries every 1 s), which also covers sender restarts.
- There is no authentication or encryption. Use it on trusted links only.

## Discovery (UDP)

Lets a client on the same LAN find the sender without knowing its IP address. All
integers big-endian, as for TCP.

- The sender listens on UDP `0.0.0.0:7778` (`--discovery-port`; `--no-discovery`
  turns it off).
- While not connected, the client sends DISCOVER **once per second** to
  `255.255.255.255` **and** the directed broadcast address of its subnet (e.g.
  `192.168.1.255` for `192.168.1.42/24`), UDP port 7778. Sockets need `SO_BROADCAST`.
- The sender answers **every** valid DISCOVER with one ANNOUNCE, **unicast to the
  datagram's source address and port**, so the client just reads replies on the
  socket it sent from (no multicast lock or broadcast reception needed). A client may
  get several ANNOUNCEs for one round (one per broadcast address, or several Macs).
- Receivers ignore datagrams with the wrong magic, another version, an unknown type,
  or a malformed body. Bytes after the defined fields are ignored (forward
  compatibility).

DISCOVER (client → broadcast), 7 bytes:

| Offset | Size | Field   | Value                        |
|-------:|-----:|---------|------------------------------|
| 0      | 4    | magic   | ASCII `VRSC` = `56 52 53 43` |
| 4      | 1    | type    | `0x01` DISCOVER              |
| 5      | 2    | version | `u16` = `1`                  |

ANNOUNCE (sender → client, unicast), 10 + `nameLen` bytes:

| Offset | Size      | Field   | Value                                                  |
|-------:|----------:|---------|--------------------------------------------------------|
| 0      | 4         | magic   | ASCII `VRSC`                                           |
| 4      | 1         | type    | `0x02` ANNOUNCE                                        |
| 5      | 2         | version | `u16` = `1`                                            |
| 7      | 2         | tcpPort | `u16`, the sender's TCP port                           |
| 9      | 1         | nameLen | `u8`, 0-63                                             |
| 10     | `nameLen` | name    | UTF-8, the Mac's computer name (cut at a character boundary) |

The sender's address is the ANNOUNCE's **source IP**; connect to `sourceIP:tcpPort`.

Examples:

```
DISCOVER:                       56 52 53 43 01 00 01
ANNOUNCE port 7777, name "Mac": 56 52 53 43 02 00 01 1E 61 03 4D 61 63
```

Client connection policy (informative; what the Quest app does):

1. If `debug.vrscreen.host` is set, connect only there.
2. Otherwise try `127.0.0.1:<port>` first: with `adb reverse` set up this is the USB
   tunnel, and without it the connection is refused at once.
3. Otherwise connect to the first sender that ANNOUNCEs.

Once connected, a client stays on that connection (no switching mid-stream); it goes
through the same steps again only after the connection is lost.

## Encoding rules

- All integers are unsigned and **big-endian** (network byte order).
- `u8`/`u16`/`u32`/`u64` are 1/2/4/8-byte unsigned integers.

## Connection start: hello

Right after the TCP connection is established, the server sends 6 bytes:

| Offset | Size | Field   | Value                        |
|-------:|-----:|---------|------------------------------|
| 0      | 4    | magic   | ASCII `VRSC` = `56 52 53 43` |
| 4      | 2    | version | `u16` = `1` (`00 01`)        |

A client MUST check both fields and close the connection if either is wrong.
The client sends no hello.

## Message framing (both directions, after the hello)

Every message is:

| Offset | Size     | Field   | Notes                                   |
|-------:|---------:|---------|-----------------------------------------|
| 0      | 1        | type    | `u8`, see table below                   |
| 1      | 4        | length  | `u32`, payload size in bytes (may be 0) |
| 5      | `length` | payload | type-specific                           |

- TCP is a byte stream: a message can be split across reads, and one read can contain
  several messages. Buffer until `5 + length` bytes are available.
- **Unknown message types MUST be skipped** by discarding `length` payload bytes, then
  continuing with the next message. This is how later protocol revisions add messages
  without breaking v1 clients. Both sides follow this rule.
- Receivers MUST ignore flag bits they do not know, and any payload bytes beyond the
  fields defined below for AUDIO_CONFIG, PING, PONG and FRAME_ACK.
- Receivers may treat `length > 64 MiB` (67 108 864) as a protocol error and close the
  connection. Real messages are far smaller (a 4K keyframe is typically 150-500 KB).

| Type   | Name   | Direction       |
|--------|--------|-----------------|
| `0x01` | CONFIG | server → client |
| `0x02` | FRAME  | server → client |
| `0x03` | CONTROL | server → client |
| `0x04` | AUDIO_CONFIG | server → client |
| `0x05` | AUDIO  | server → client |
| `0x10` | PING   | client → server |
| `0x11` | PONG   | server → client |
| `0x12` | FRAME_ACK | client → server |

### `0x01` CONFIG

Describes the video stream that the following FRAMEs belong to.

| Payload offset | Size | Field          | Notes                                    |
|---------------:|-----:|----------------|------------------------------------------|
| 0              | 1    | codec          | `u8`: `1` = H.264/AVC, `2` = H.265/HEVC  |
| 1              | 4    | width          | `u32`, pixels (even)                     |
| 5              | 4    | height         | `u32`, pixels (even)                     |
| 9              | rest | parameter sets | Annex-B, see below                       |

Parameter sets are NAL units, each preceded by the 4-byte start code `00 00 00 01`:

- HEVC: VPS, SPS, PPS (NAL types 32, 33, 34), in that order.
- H.264: SPS, PPS (NAL types 7, 8), in that order.

No other NAL units appear in this field. NAL unit type is
`(byte0 >> 1) & 0x3F` for HEVC and `byte0 & 0x1F` for H.264.

When CONFIG is sent:

- Right after the hello, if the sender has already encoded with the current settings.
- Before the first FRAME of every stream (if it was not sent right after the hello).
- Whenever the format or the parameter sets change, e.g. when the captured display's
  resolution changes. The next FRAME after a CONFIG is always a keyframe.

A client that receives a CONFIG equal to the current one (same bytes) can ignore it.
On a different CONFIG, it reconfigures (or recreates) its decoder and drops FRAMEs
until the next keyframe.

Using it with Android `MediaCodec`:

- Mime type `video/hevc` (codec 2) or `video/avc` (codec 1); `width`/`height` from
  CONFIG.
- HEVC: pass the whole parameter-set field as `csd-0`.
- H.264: `csd-0` = SPS and `csd-1` = PPS, each with its `00 00 00 01` start code (split
  at the second start code). Alternatively, queue the whole field as one input buffer
  with `BUFFER_FLAG_CODEC_CONFIG`.

### `0x02` FRAME

One encoded picture.

| Payload offset | Size | Field             | Notes                                   |
|---------------:|-----:|-------------------|-----------------------------------------|
| 0              | 1    | flags             | `u8`; bit 0 = keyframe; others reserved |
| 1              | 8    | captureTimeMicros | `u64`, sender clock, see "Clocks"       |
| 9              | rest | access unit       | Annex-B, `00 00 00 01` start codes      |

- The access unit is exactly one picture, ready to queue as one `MediaCodec` input
  buffer. Use `captureTimeMicros` as the input buffer's presentation time if you like.
- **Keyframes** (bit 0 set) are IDR pictures and start with the stream's parameter
  sets (same NAL units as the CONFIG field), followed by the picture's slice NAL units.
  A decoder can therefore (re)start on any keyframe.
- No B-frames, no frame reordering: decode order = display order, and every decoded
  output can be shown right away.
- Frames never get dropped once encoded (TCP; the sender never discards encoded
  frames, see "Flow control"). The first FRAME after the hello, and after every CONFIG,
  is a keyframe.
- `captureTimeMicros` is non-decreasing within a connection, but not unique: the
  keyframe sent to a newly connected client can be a re-encode of a picture that was
  already sent to a previous client.

### `0x03` CONTROL (server → client)

Asks the client to move the virtual screen (the Mac sender sends these from its global
keyboard shortcuts and ⌃⌥⌘ mouse/scroll gestures). Sent to the current client only,
as soon as the user acts; it is not subject to the frame windows of "Flow control".

| Payload offset | Size | Field   | Notes                         |
|---------------:|-----:|---------|-------------------------------|
| 0              | 1    | command | `u8`, see below               |
| 1              | rest | args    | command-specific, see below   |

| Command | Name          | Args                   | Meaning |
|---------|---------------|------------------------|---------|
| `0x01`  | RECENTER      | none                   | Place the screen in front of the current head direction, keeping its distance and size. |
| `0x02`  | (reserved)    | —                      | Formerly MOVE, removed. Never sent; receivers ignore it like any unknown command. |
| `0x03`  | DISTANCE      | `steps i8`             | Positive = farther, negative = closer. |
| `0x04`  | SIZE          | `steps i8`             | Positive = bigger, negative = smaller. |
| `0x05`  | RESET         | none                   | Default distance and size, then recenter. |
| `0x06`  | TRANSLATE     | `dx i8`, `dy i8`       | Slide the screen in its own plane: `dx` steps right, `dy` steps up. Its orientation does not change. |
| `0x07`  | ORBIT         | `yaw i16`, `pitch i16` | Rotate the screen around the head position by `yaw`/100 degrees (positive = right) and `pitch`/100 degrees (positive = up), keeping its distance; the screen turns to keep facing the head. |
| `0x08`  | DISTANCE_FINE | `v i16`                | Distance × 1.1^(v/100); positive = farther. `v = 100` equals one DISTANCE step. |
| `0x09`  | SIZE_FINE     | `v i16`                | Width × 1.1^(v/100); positive = bigger. `v = 100` equals one SIZE step. |
| `0x0A`  | SET_CURVATURE | `value u16`            | **Absolute** curvature in permille, clamped to 0–1000: 0 = flat; otherwise the screen bends around the viewer on a cylinder of radius = viewing distance / (value/1000), so 1000 = radius equal to the viewing distance. |
| `0x0B`  | SET_DIM       | `value u16`            | **Absolute**, **perceptual** dimming of the surroundings (passthrough) in permille, clamped to 0–1000: 0 = normal passthrough, 500 ≈ half as bright, 1000 = fully black. The client maps the value to its compositor's controls so that brightness falls roughly linearly. Never dims the screen itself. |
| `0x0C`  | SET_HAND_GESTURES | `enabled u8`       | **Absolute**: 0 = off, any other value = on. While off, the client ignores hand input entirely (no hand-based moving or resizing). Default off. |

- `i8` is a signed byte, `i16` a signed and `u16` an unsigned big-endian 16-bit integer
  (two's complement for signed): `FF` = −1, `FF E7` = −25, `03 E8` = 1000.
- Step sizes of the step commands are up to the client. The reference client (Quest
  app) uses 5 cm per TRANSLATE step, distance × 1.1^steps and width × 1.1^steps
  (aspect ratio kept), each clamped to its own range. ORBIT, DISTANCE_FINE and
  SIZE_FINE are exact (degrees, ×1.1^(v/100)), subject to the same clamps.
- Clients MUST ignore unknown commands and any bytes after a known command's
  arguments (forward compatibility). A known command with missing argument bytes is
  malformed.
- Step commands come from keyboard shortcuts: one message per step; holding a
  shortcut on the Mac repeats it (after ~300 ms, then every ~80 ms).
- ORBIT, DISTANCE_FINE and SIZE_FINE come from continuous mouse/scroll gestures: the
  sender coalesces input and sends at most one of each per 1/60 s, so a client can
  apply them directly (or smooth them over a frame).
- SET_CURVATURE, SET_DIM and SET_HAND_GESTURES carry absolute values (idempotent); the
  sender is the source of truth for all three (the client does not persist them). It
  sends all three right after the hello on every connection (after the cached CONFIG if
  there is one, before the first FRAME), and again on every change; slider drags on the
  Mac are throttled to at most 30 messages per second, the final value always included.
  Clients apply them whenever they arrive, before or after CONFIG, and keep them across
  CONFIG changes.
- SET_DIM is a wish that applies while a picture is on screen: a client must not dim the
  surroundings while it shows no picture (searching, connecting, waiting for the first
  frame), since the user then needs to see the room. The reference client keeps the dim
  for 1.5 s after the picture goes away (brief reconnects do not flash) and fades over
  0.3 s in both directions.

### `0x04` AUDIO_CONFIG (server → client)

Describes the AUDIO messages that follow. **No AUDIO_CONFIG on a connection means no
audio** (e.g. the user switched audio off on the Mac, or the sender runs with
`--no-audio`); clients that do not play audio just skip both types.

| Payload offset | Size | Field      | Notes                                                        |
|---------------:|-----:|------------|--------------------------------------------------------------|
| 0              | 1    | format     | `u8`: `1` = PCM, signed 16-bit **little-endian** samples, channels interleaved (L R L R … for stereo) |
| 1              | 4    | sampleRate | `u32`, Hz (the Mac sender: 48000)                            |
| 5              | 1    | channels   | `u8` (the Mac sender: 2)                                     |

- When audio is on as a client connects: right after the hello, after the cached CONFIG
  and the settings CONTROLs, before the first FRAME and the first AUDIO. When audio is
  switched on later in a connection that has not had one: right before the first AUDIO.
  Sent again whenever the format changes; AUDIO always uses the format of the most recent
  AUDIO_CONFIG. Switching audio off sends nothing: AUDIO just stops.
- A client that does not support `format` (or the rate/channel count) ignores AUDIO until
  an AUDIO_CONFIG it supports arrives. Unknown `format` values are not a protocol error.

### `0x05` AUDIO (server → client)

One chunk of audio.

| Payload offset | Size | Field             | Notes                                                    |
|---------------:|-----:|-------------------|----------------------------------------------------------|
| 0              | 8    | captureTimeMicros | `u64` (big-endian like every header field), sender clock, time of the chunk's **first** sample |
| 8              | rest | PCM               | `frames × channels × 2` bytes, format from AUDIO_CONFIG   |

- **Endianness:** the header is big-endian, but the PCM samples are **little-endian**
  signed 16-bit (what Android `AudioTrack`/AAudio, WAV files and most hardware use), so
  the payload can be handed to the audio output unchanged.
- **Chunking:** the Mac sender sends 10 ms chunks: 480 frames at 48 kHz, i.e. 1920 PCM
  bytes (1928-byte payload, 1933 bytes on the wire) for stereo, about 100 messages and
  1.55 Mbit/s per second. Clients must accept any chunk length that is a whole number of
  frames (including 0).
- **Clock:** `captureTimeMicros` is on the same sender clock as FRAME `captureTimeMicros`
  (see "Clocks"), so audio and video are synchronized by comparing the two: a sample and
  a picture with the same capture time were captured at the same moment. For screen
  capture it is ScreenCaptureKit's presentation time of the audio buffer (captured
  system audio is about 25 ms old when the sender gets it); for the test pattern, the
  synthesized time.
- **Continuity:** consecutive chunks normally follow each other exactly (next
  `captureTimeMicros` = previous + frames × 10^6 / sampleRate, within ±1 µs of
  rounding; the sender follows the capture clock, so small adjustments of up to a few
  ms can occur). A larger step forward is a gap: chunks were dropped (see below),
  capture paused, or audio was switched off and on again. Audio can also simply stop
  (switched off on the Mac) without any message. Clients play silence in gaps (or let
  their buffer drain to silence) and re-align on the timestamps; they must not stall.
- **Not part of flow control:** AUDIO does not count as a FRAME, is never acknowledged
  and does not take room in the frame windows. It shares the socket with FRAMEs, in
  capture order.
- **Drop policy:** audio is only useful when it is fresh. The sender drops queued AUDIO
  chunks, oldest first, that are more than **150 ms** past their capture time before they
  could be handed to the socket (e.g. while a slow link drains a large keyframe). Bytes
  already in the kernel send buffer or in transit cannot be recalled, so clients must
  still cope with late chunks (drop or skip them by timestamp).
- Audio never delays video: capture, conversion and sending run independently of the
  encoder.

### `0x10` PING (client → server)

| Payload offset | Size | Field            | Notes                       |
|---------------:|-----:|------------------|-----------------------------|
| 0              | 8    | clientTimeMicros | `u64`, client's own clock   |

### `0x11` PONG (server → client)

| Payload offset | Size | Field            | Notes                                     |
|---------------:|-----:|------------------|-------------------------------------------|
| 0              | 8    | clientTimeMicros | `u64`, copied from the PING               |
| 8              | 8    | serverTimeMicros | `u64`, sender clock when the PING was read |

The server answers every PING with one PONG.

### `0x12` FRAME_ACK (client → server)

| Payload offset | Size | Field          | Notes                                         |
|---------------:|-----:|----------------|-----------------------------------------------|
| 0              | 4    | framesConsumed | `u32`, cumulative count, see below            |

`framesConsumed` is the number of FRAME messages received **on this connection** that
the client is done with: handed to its decoder (e.g. queued as a `MediaCodec` input
buffer) **or deliberately discarded** (e.g. dropped while waiting for a keyframe
after a CONFIG). It starts at 0 for every connection and wraps around at 2^32.

- Send a FRAME_ACK after every consumed FRAME. Acknowledging several frames with one
  FRAME_ACK (the latest count) is allowed.
- Every FRAME must eventually be counted. Once a client has acknowledged anything,
  the server sends at most a few unacknowledged FRAMEs, so a FRAME that is never
  counted stalls the stream for the rest of the connection.
- The count never goes backwards and never exceeds the number of FRAMEs received.
  The server ignores FRAME_ACKs that violate this.
- FRAME_ACK is optional. A client that never sends it keeps working; the server then
  only bounds its socket backlog (see "Flow control").

## Clocks and latency measurement

`captureTimeMicros` and `serverTimeMicros` are on the **sender clock**: the macOS host
time clock (`mach_absolute_time`, as used by CoreMedia), in microseconds. It is
monotonic and unrelated to wall time. For screen capture it is the presentation time
ScreenCaptureKit assigned to the captured image (FRAME) or audio buffer (AUDIO); for the
test pattern, the moment the pattern was rendered (FRAME) or the synthesized sample time
(AUDIO). FRAME and AUDIO timestamps are therefore directly comparable for A/V sync.

The client uses any monotonic clock of its own (Android: `CLOCK_MONOTONIC`) for
`clientTimeMicros`. To estimate the offset between the two clocks:

```
t0 = client clock when sending PING        (= clientTimeMicros, echoed back)
ts = serverTimeMicros from the PONG
t1 = client clock when the PONG arrived
rtt    = t1 - t0
offset = ts - (t0 + rtt / 2)               // sender clock - client clock
```

Keep the offset from the sample with the smallest RTT over a recent window (the Mac
viewer uses the last 16 samples, pinging 5 times at 100 ms intervals after connecting,
then once per second). Then, for a frame shown at client time `t`:

```
capture → display latency = (t + offset) - captureTimeMicros
```

## Video format

- 8-bit 4:2:0. HEVC Main profile, or H.264 (Constrained) High profile.
- **Colour: BT.709** primaries, transfer function and matrix, **limited (video)
  range** (Y 16-235, CbCr 16-240). The VUI signals exactly this
  (`video_full_range_flag = 0`, `colour_primaries = transfer_characteristics =
  matrix_coeffs = 1`). ScreenCaptureKit converts the display to sRGB/BT.709 primaries
  before encoding.
- Size: the display's native pixel size scaled down to fit the sender's
  `--max-width` (default 3840), aspect ratio kept, both dimensions even. A 5K display
  is sent as 3840x2160.

## Frame timing and keyframe policy

- Frames are sent only when the screen changes (ScreenCaptureKit delivers no images
  for an unchanged screen), up to the sender's target rate (default 60 fps). A static
  screen produces no FRAMEs at all: the client MUST keep showing the last decoded
  picture and must not treat a pause as an error. There is no keep-alive.
- On connect, the sender immediately re-encodes its most recently captured picture as
  a keyframe, so a client connecting to a static screen gets a picture at once.
- While frames flow, a keyframe is produced at least every ~5 s (sender
  `--keyframe-interval`) as a safety net.

## Flow control

The sender never drops an encoded frame (the decoder's reference chain must stay
intact). Instead, while a client's window is full it skips **encoding** new captures,
keeps only the newest one, and encodes that as soon as the window opens, so the final
state of a screen that just became static is always delivered. A slow link or a slow
decoder therefore lowers the frame rate instead of growing latency. Two windows exist:

1. **Frames in flight (clients that send FRAME_ACK).** After a client's first
   FRAME_ACK, the sender keeps `FRAMEs sent − framesConsumed` at or below a small limit
   (default **3**, sender option `--max-in-flight`). This bounds latency end to end,
   including data buffered anywhere between the two sockets (e.g. inside adb's USB
   tunnel) and frames waiting in the client. Before the first FRAME_ACK of a
   connection the window does not apply; the keyframe sent on connect always goes out.
2. **Socket backlog (all clients).** At most 2 encoded FRAMEs waiting to be written to
   the server socket, plus a fixed kernel send buffer: 128 KiB until the client's first
   FRAME_ACK, 512 KiB after it (the ack window then bounds latency, and 128 KiB would
   cap Wi-Fi TCP at about 120 Mbit/s). This is the only limit for clients that never
   acknowledge. It cannot see data that has already left the sender's socket: bytes
   buffered in a proxy (`adb reverse` over USB buffers about 3.5 MB before the sender's
   socket blocks, measured with adb 37.0.1, i.e. ~1 s at 30 Mbit/s and far more at
   desktop bitrates) or in the client's receive buffer (about 0.75 MB on a Quest 3 over
   Wi-Fi) add latency undetected, which is why FRAME_ACK exists.

AUDIO is outside both windows: it is sent as it is captured, and dropped instead of
delayed when it would be more than 150 ms late (see AUDIO).

Clients should read the socket promptly (e.g. on a dedicated thread) and acknowledge
frames as they hand them to the decoder.

## Example byte sequences

Hello: `56 52 53 43 00 01`

PING with `clientTimeMicros = 0x0A0B0C0D0E0F1011`:

```
10 00 00 00 08 0A 0B 0C 0D 0E 0F 10 11
```

FRAME_ACK with `framesConsumed = 42`:

```
12 00 00 00 04 00 00 00 2A
```

CONTROL TRANSLATE `dx = −1, dy = +1`, ORBIT yaw +5.00° pitch −2.50°, and RECENTER:

```
03 00 00 00 03 06 FF 01
03 00 00 00 05 07 01 F4 FF 06
03 00 00 00 01 01
```

CONTROL SET_CURVATURE 350 (35 %), SET_DIM 1000 (black surroundings) and
SET_HAND_GESTURES on:

```
03 00 00 00 03 0A 01 5E
03 00 00 00 03 0B 03 E8
03 00 00 00 02 0C 01
```

AUDIO_CONFIG s16le, 48000 Hz, stereo, then AUDIO with `captureTimeMicros =
0x0102030405060708` carrying one stereo frame, left = 1, right = −2 (a real chunk has 480
frames):

```
04 00 00 00 06 01 00 00 BB 80 02
05 00 00 00 0C 01 02 03 04 05 06 07 08 01 00 FE FF
```

FRAME, keyframe, `captureTimeMicros = 2`, access unit `00 00 00 01 26 01 ...`:

```
02 <len:4> 01 00 00 00 00 00 00 00 02 00 00 00 01 26 01 ...
```

## Typical session

```
server → client  hello  VRSC 0001
server → client  CONFIG codec=2 3840x2160 (VPS SPS PPS, ~100 B)
server → client  CONTROL SET_CURVATURE 350, SET_DIM 0, SET_HAND_GESTURES 0 (current settings)
server → client  AUDIO_CONFIG format=1 48000 Hz 2 ch (if audio is on)
server → client  FRAME  key=1 (VPS SPS PPS + IDR slices)
client → server  FRAME_ACK 1
client → server  PING
server → client  AUDIO  t=… (10 ms of PCM; ~100 per second, interleaved with FRAMEs)
server → client  PONG
server → client  FRAME  key=0 ...
client → server  FRAME_ACK 2
...              (pause while the screen is static)
server → client  CONFIG codec=2 2560x1440 (display resolution changed)
server → client  FRAME  key=1 ...
client → server  FRAME_ACK 3
```
