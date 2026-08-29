# Windows virtual camera output

This branch is for exposing scrcpy's decoded video as a native Windows webcam,
so applications such as VTube Studio can select it directly without routing
through OBS first.

## Goal

```text
Android camera
    |
    | scrcpy camera capture (ADB / TCP/IP)
    v
scrcpy video decoder
    |
    v
Windows virtual-camera sink
    |
    v
scrcpy Virtual Camera
    |
    +--> VTube Studio
    +--> OBS
    +--> other Windows camera applications
```

The Android-side camera capture already exists. The new work belongs on the
Windows client side, after decoding.

## Why this is not V4L2 on Windows

On Linux, `app/src/v4l2_sink.c` can write decoded frames to a V4L2 loopback
output device. Windows has no equivalent writable `/dev/video*` device.

For Windows 11, the native API is Media Foundation's `MFCreateVirtualCamera()`.
A virtual camera registered through this API references a custom Media
Foundation media-source COM class. That media source is what Windows camera
clients actually open.

Minimum target for the native implementation:

- Windows 11, build 22000 or newer
- Windows SDK 10.0.22000.0 or newer
- Current-user registration by default (no administrator requirement)

Windows 10 support should be treated separately, most likely through a legacy
DirectShow virtual-camera filter. It should not complicate the initial Windows
11 implementation.

## scrcpy integration

scrcpy already has the abstraction needed for this. `sc_decoder` publishes
`AVFrame`s through `sc_frame_source`, and consumers implement `sc_frame_sink`.
The existing V4L2 implementation is a useful model for lifecycle and buffering.

The Windows implementation should add a sibling sink rather than modifying the
Android camera capture or decoder itself.

Planned client-side pieces:

```text
app/src/win_vcam_sink.c
app/src/win_vcam_sink.h
app/src/sys/win/vcam_ipc.c
app/src/sys/win/vcam_ipc.h
windows/virtual-camera/...
```

### `sc_win_vcam_sink`

Responsibilities:

1. Implement `sc_frame_sink`.
2. Accept decoded `AV_PIX_FMT_YUV420P` frames.
3. Keep only the newest frame when the consumer is late. Camera tracking values
   low latency more than preserving every frame.
4. Convert only when the Media Foundation stream asks for a format that cannot
   consume the decoded format directly.
5. Publish frames to the Media Foundation camera source through a low-overhead
   local IPC transport.
6. Stop cleanly without blocking scrcpy shutdown.

The sink should follow the same producer/consumer pattern as `sc_v4l2_sink`:
`sc_frame_buffer`, mutex/condition variable, and a dedicated output thread.

## IPC transport

The Media Foundation camera source may be loaded by Windows Camera Frame Server
rather than by the scrcpy process. Therefore it must not depend on direct
in-process pointers to scrcpy frames.

Use a per-instance named shared-memory mapping plus synchronization primitives.
The mapping header should be versioned so the producer and media source can
reject incompatible layouts safely.

Suggested layout:

```c
struct sc_vcam_shared_header {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t stride[3];
    uint32_t offset[3];
    uint64_t sequence;
    int64_t timestamp_us;
};
```

Use two frame slots (double buffering). The producer writes into the inactive
slot and publishes a monotonically increasing sequence number only after the
frame is complete. The consumer never waits for the producer; if it misses a
frame, it consumes the latest completed one.

Do not send raw frame payloads through a named pipe. A pipe may be used for
control/handshake messages, while shared memory carries video frames.

## Media Foundation component

The companion DLL implements a custom Media Foundation media source and one
video stream. Its COM CLSID is registered for the current user, then
`MFCreateVirtualCamera()` registers a software virtual camera with a friendly
name such as:

```text
scrcpy Virtual Camera
```

The media source reads the newest frame from the shared mapping and returns
Media Foundation samples when the client requests them.

Initial formats:

- 1280x720 @ 30 fps
- 1920x1080 @ 30 fps
- NV12 preferred for Windows camera applications

The producer can continue receiving YUV420P from FFmpeg. Conversion to NV12 is
cheap because the Y plane is copied unchanged and U/V are interleaved.

## CLI

Proposed user-facing option:

```text
--virtual-camera
```

Optional friendly-name override can be added later if there is a real need.
Avoid adding unnecessary configuration in the first implementation.

Typical camera command:

```bash
scrcpy --video-source=camera \
       --camera-facing=front \
       --camera-size=1280x720 \
       --camera-fps=30 \
       --no-audio \
       --virtual-camera \
       --no-window
```

The resulting device should appear in VTube Studio as `scrcpy Virtual Camera`.

## Performance requirements

This feature exists partly to avoid a wasteful camera chain, so keep the hot
path deliberately small:

- no extra video encoder
- no H.264 re-encode on the PC
- no OBS dependency
- latest-frame semantics instead of an ever-growing queue
- one YUV420P -> NV12 conversion only when needed
- no extra SDL window when `--no-window` is used
- no network transport between scrcpy and the virtual-camera component

Desired final path:

```text
phone camera -> Android encoder -> scrcpy decoder -> shared frame ->
Media Foundation virtual camera -> VTube Studio -> OBS
```

## Failure behavior

- If the virtual-camera component is not installed/registered, print a clear
  error and exit when `--virtual-camera` was explicitly requested.
- If a consumer is not connected, scrcpy should continue publishing frames
  without blocking.
- If scrcpy exits while the webcam is open, the Media Foundation source should
  emit a black frame or end the stream cleanly instead of retaining stale image
  data.
- The normal scrcpy display and recording paths must remain unchanged when
  `--virtual-camera` is not requested.

## Reference implementation

Microsoft maintains an official Windows virtual-camera Media Foundation sample
in the `microsoft/Windows-Camera` repository under `Samples/VirtualCamera`.
Use that API model as a reference, but keep this implementation minimal: one
software camera, one media source, one video stream, and no tray application or
hardware-camera wrapping features.
