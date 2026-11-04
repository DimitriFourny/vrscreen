# Quest Mirror wire protocol, version 1

This is the contract between the Mac sender (`quest-mirror-sender`) and any client:
the Mac test viewer (`quest-mirror-viewer`) and the Quest app (C++/NDK,
`AMediaCodec`). A client can be written from this document alone.

## Transport

- Plain TCP. The **sender is the server**: it listens on `0.0.0.0`, port **7777** by
  default (`--port`), with `TCP_NODELAY` set.
- Over USB: run `adb reverse tcp:7777 tcp:7777` on the Mac; the Quest app connects to
  `127.0.0.1:7777`. Over Wi-Fi: the Quest app connects to `<Mac IP>:7777`. The
  byte stream is identical in both cases.
- **One active client.** When a new client connects, the server closes the
  previous connection. Clients should reconnect automatically (the Mac viewer
  retries every 1 s), which also covers sender restarts.
- There is no authentication or encryption. Use it on trusted links only.

## Encoding rules

- All integers are unsigned and **big-endian** (network byte order).
- `u8`/`u16`/`u32`/`u64` are 1/2/4/8-byte unsigned integers.

## Connection start: hello

Right after the TCP connection is established, the server sends 6 bytes:

| Offset | Size | Field   | Value                        |
|-------:|-----:|---------|------------------------------|
| 0      | 4    | magic   | ASCII `QMIR` = `51 4D 49 52` |
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
  fields defined below for PING and PONG.
- Receivers may treat `length > 64 MiB` (67 108 864) as a protocol error and close the
  connection. Real messages are far smaller (a 4K keyframe is typically 150-500 KB).

| Type   | Name   | Direction       |
|--------|--------|-----------------|
| `0x01` | CONFIG | server → client |
| `0x02` | FRAME  | server → client |
| `0x10` | PING   | client → server |
| `0x11` | PONG   | server → client |

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

## Clocks and latency measurement

`captureTimeMicros` and `serverTimeMicros` are on the **sender clock**: the macOS host
time clock (`mach_absolute_time`, as used by CoreMedia), in microseconds. It is
monotonic and unrelated to wall time. For screen capture it is the presentation time
ScreenCaptureKit assigned to the captured image; for the test pattern, the moment the
pattern was rendered.

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

## Flow control (informative)

The sender keeps a small backlog per client: at most 2 encoded FRAMEs waiting to be
written to its socket, plus a fixed 128 KiB kernel send buffer. While the backlog is
full it skips **encoding** new captures (never dropping encoded frames, so the
reference chain stays intact), keeps only the newest capture, and encodes that one
as soon as the backlog drains. A slow link therefore lowers the frame rate instead of
growing latency. Data waiting in the *client's* receive buffer is latency the sender
cannot see: clients should read the socket promptly (e.g. on a dedicated thread) and
hand frames to the decoder without queueing.

## Example byte sequences

Hello: `51 4D 49 52 00 01`

PING with `clientTimeMicros = 0x0A0B0C0D0E0F1011`:

```
10 00 00 00 08 0A 0B 0C 0D 0E 0F 10 11
```

FRAME, keyframe, `captureTimeMicros = 2`, access unit `00 00 00 01 26 01 ...`:

```
02 <len:4> 01 00 00 00 00 00 00 00 02 00 00 00 01 26 01 ...
```

## Typical session

```
server → client  hello  QMIR 0001
server → client  CONFIG codec=2 3840x2160 (VPS SPS PPS, ~100 B)
server → client  FRAME  key=1 (VPS SPS PPS + IDR slices)
client → server  PING
server → client  PONG
server → client  FRAME  key=0 ...
...              (pause while the screen is static)
server → client  CONFIG codec=2 2560x1440 (display resolution changed)
server → client  FRAME  key=1 ...
```
