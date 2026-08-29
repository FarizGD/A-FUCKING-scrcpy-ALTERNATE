# scrcpy Virtual Microphone (Windows)

This directory contains the Windows-side virtual microphone work for the scrcpy
camera/microphone fork.

## Current data path

The Win64 client can publish decoded microphone samples when
`SCRCPY_WIN_VMIC=1` is set:

```text
Android microphone
  -> scrcpy audio stream
  -> FFmpeg decoder
  -> float PCM ring buffer
  -> Local\ScrcpyVirtualMicrophonePcm
```

The shared-memory producer lives in:

```text
app/src/win_vmic_sink.c
app/src/win_vmic_sink.h
```

The mapping contains a versioned header followed by an interleaved float32 PCM
ring buffer. The ring is intentionally lossy: a slow consumer follows the newest
samples instead of blocking scrcpy.

## Driver architecture

A real Windows microphone endpoint requires a Windows audio driver. The driver
will be deliberately small and will not contain ADB, networking, FFmpeg, or
Android-specific code.

```text
scrcpy.exe
  -> shared PCM ring
  -> user-mode bridge/service
  -> WaveRT virtual capture miniport
  -> Windows Audio Engine
  -> "scrcpy Virtual Microphone"
```

The WaveRT endpoint is implemented as a root-enumerated virtual audio device.
The user-mode bridge owns the shared-memory reader and feeds the driver through
a small private IOCTL/control channel. Keeping the parser/transport outside the
kernel minimizes the kernel-mode attack surface and makes reconnects easier.

## Target format

Initial endpoint format:

- 48 kHz when provided by the Android stream (otherwise use the stream rate)
- 32-bit float internally
- mono or stereo input
- Windows-facing capture format may be converted to 16-bit PCM or float32 by
  the bridge/driver contract as required by the WaveRT miniport

## Shared-memory contract

Mapping name:

```text
Local\ScrcpyVirtualMicrophonePcm
```

Header magic: `SMIC`

Version: `1`

Fields include sample rate, channel count, ring capacity, current write cursor,
sequence number, and total frames written.

## Development requirements

The kernel driver will require:

- Visual Studio 2022
- Windows Driver Kit (WDK)
- x64 target
- test signing while developing

Do not require administrator privileges for ordinary scrcpy operation after the
driver is installed. Installation/removal of the driver itself will require
elevation.

## Reference model

The implementation follows the Windows PortCls/WaveRT virtual-audio model. The
Microsoft Windows driver samples contain a Simple Audio Sample that exposes
virtual speaker/microphone endpoints and is useful for understanding PortCls and
WaveRT lifecycle. Do not copy its source into this project; this implementation
keeps its own code and licensing.

## Milestones

1. [x] Decoded PCM producer in scrcpy.
2. [ ] Shared protocol header usable by Win32 bridge and driver.
3. [ ] User-mode bridge/service.
4. [ ] Root-enumerated WaveRT capture driver.
5. [ ] INF + development test signing/install scripts.
6. [ ] Automatic reconnect/silence behavior when scrcpy exits.
7. [ ] Package `scrcpy Virtual Microphone` with the Windows build.
