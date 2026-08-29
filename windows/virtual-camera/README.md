# scrcpy Virtual Camera (Windows 11)

This component exposes scrcpy's decoded camera frames as a Windows Media Foundation virtual camera.

## Data path

```text
Android camera
  -> scrcpy decoder
  -> YUV420P -> NV12
  -> Local\ScrcpyVirtualCameraFrames
  -> Media Foundation custom media source
  -> IMFVirtualCamera / MFCreateVirtualCamera
  -> Windows camera applications
```

The scrcpy producer is enabled with `SCRCPY_WIN_VCAM=1`.

The camera source is based on a pinned MIT-licensed VCamSample revision and patched at build time to read the scrcpy shared-memory transport. The upstream source is not vendored into this repository.

## Requirements

- Windows 11 build 22000 or newer
- x64
- Visual Studio 2022 with Desktop development with C++
- Windows 11 SDK
- NuGet

## Build locally

From the repository root in PowerShell:

```powershell
.\windows\virtual-camera\bootstrap-camera.ps1
nuget restore .\windows\virtual-camera\VCamSample-src\VCamSample.sln
msbuild .\windows\virtual-camera\VCamSample-src\VCamSample.sln /m /p:Configuration=Release /p:Platform=x64
```

GitHub Actions also builds the component as `scrcpy-virtual-camera-device-win64`.

## Register the media source

The custom media source is an in-process COM server. Register the built DLL from an **Administrator** terminal:

```powershell
regsvr32 .\scrcpy-vcam-source.dll
```

The registration helper itself creates the virtual camera with current-user access. Keep it running while testing:

```powershell
.\scrcpy-vcam-register.exe
```

## Start scrcpy camera transport

From the scrcpy Win64 build:

```powershell
$env:SCRCPY_WIN_VCAM='1'
.\scrcpy.exe `
  --video-source=camera `
  --camera-facing=front `
  --camera-size=1280x720 `
  --camera-fps=30 `
  --no-audio
```

The current Media Foundation adapter advertises 1280x720 at 30 fps and expects the scrcpy transport to use the same size. If scrcpy is not running, the camera source emits a black NV12 frame rather than stale memory.

## Current development limitations

- Windows 11 only (`MFCreateVirtualCamera`).
- 1280x720 @ 30 fps is fixed for this first milestone.
- The registration helper currently follows VCamSample's session-lifetime model; a later installer/service milestone will make installation and persistence friendlier.
- The COM source DLL must be registered before the virtual camera can be started.

The shared-memory sequence uses odd/even publication: odd means scrcpy is writing and even means a complete frame is available. The Media Foundation reader accepts a frame only when the sequence is unchanged and even before/after its copy.
