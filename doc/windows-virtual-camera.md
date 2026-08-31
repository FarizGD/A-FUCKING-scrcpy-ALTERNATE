# Windows virtual camera output

This branch is for exposing scrcpy's decoded video as a native Windows webcam,
so applications such as VTube Studio can select it directly without routing
through OBS first.

## Current implementation status

Milestone 1 is now implemented on the scrcpy producer side:

- Windows video-decoder frames can be published to named shared memory.
- YUV420P is converted to NV12 without re-encoding.
- The mapping is `Local\ScrcpyVirtualCameraFrames`.
- The mapping header contains magic/version, dimensions, stride, FourCC, frame
  size, sequence number and timestamp.
- The producer is currently opt-in through `SCRCPY_WIN_VCAM=1` while the Media
  Foundation consumer is still under development.

Temporary test command on Windows CMD:

```bat
set SCRCPY_WIN_VCAM=1
scrcpy --video-source=camera --camera-facing=front --camera-size=1280x720 --camera-fps=30 --no-audio
```

PowerShell:

```powershell
$env:SCRCPY_WIN_VCAM = "1"
.\scrcpy.exe --video-source=camera --camera-facing=front --camera-size=1280x720 --camera-fps=30 --no-audio
```

Expected log once the decoder opens:

```text
Windows virtual camera producer ready: 1280x720 NV12 (Local\ScrcpyVirtualCameraFrames)
```

This milestone alone does **not** make the camera appear in VTube Studio yet.
The next milestone is the Media Foundation virtual-camera component that reads
this mapping and exposes `scrcpy Virtual Camera` to Windows camera applications.

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

The first producer milestone currently hooks at the decoder so it can be tested
without changing scrcpy's large main lifecycle yet. Once the Media Foundation
consumer is proven, this should be refactored into a proper sibling
`sc_frame_sink` and enabled through `--virtual-camera`.

Planned client-side pieces:

```text
app/src/win_vcam_producer.c
app/src/win_vcam_producer.h
windows/virtual-camera/...
```

## IPC transport

The Media Foundation camera source may be loaded by Windows Camera Frame Server
rather than by the scrcpy process. Therefore it must not depend on direct
in-process pointers to scrcpy frames.

The current producer uses a named shared-memory mapping with a versioned header.
Frames are published as NV12 so the eventual Media Foundation source can avoid
an additional pixel-format conversion.

Current mapping:

```text
Local\ScrcpyVirtualCameraFrames
```

The producer writes the frame payload, executes a memory barrier, then increments
a sequence number. Consumers should only treat a new sequence number as a
completed frame.

A later revision can move to explicit double buffering if testing shows the
single latest-frame slot can tear under load.

## Media Foundation component

The companion DLL will implement a custom Media Foundation media source and one
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

## CLI

Proposed final user-facing option:

```text
--virtual-camera
```

Optional friendly-name override can be added later if there is a real need.
Avoid adding unnecessary configuration in the first implementation.

Typical final command:

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
- one YUV420P -> NV12 conversion
- no extra SDL window when `--no-window` is eventually used
- no network transport between scrcpy and the virtual-camera component

Desired final path:

```text
phone camera -> Android encoder -> scrcpy decoder -> shared NV12 frame ->
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
  virtual-camera output is not requested.

## Reference implementation

Microsoft maintains an official Windows virtual-camera Media Foundation sample
in the `microsoft/Windows-Camera` repository under `Samples/VirtualCamera`.
Use that API model as a reference, but keep this implementation minimal: one
software camera, one media source, one video stream, and no tray application or
hardware-camera wrapping features.
