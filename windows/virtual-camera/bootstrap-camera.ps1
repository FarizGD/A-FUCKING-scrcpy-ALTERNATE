param(
    [string]$Destination = "$PSScriptRoot\VCamSample-src"
)

$ErrorActionPreference = 'Stop'

$repo = 'https://github.com/smourier/VCamSample.git'
$commit = '58b6d4b0e0c2c97fdffd66d23e7c8da6b89f570d'

if (Test-Path $Destination) {
    Remove-Item -Recurse -Force $Destination
}

git clone $repo $Destination
Push-Location $Destination
try {
    git checkout $commit

    Copy-Item "$PSScriptRoot\ScrcpySharedFrame.h" `
              "$Destination\VCamSampleSource\ScrcpySharedFrame.h" -Force

    $streamPath = "$Destination\VCamSampleSource\MediaStream.cpp"
    $stream = Get-Content $streamPath -Raw
    $stream = $stream.Replace('#define NUM_IMAGE_ROWS 960 //480', '#define NUM_IMAGE_ROWS 720 // scrcpy camera transport')
    $stream = $stream.Replace('RETURN_IF_FAILED(handler->SetCurrentMediaType(types[0]));', 'RETURN_IF_FAILED(handler->SetCurrentMediaType(types[1]));')

    $oldD3d = @'
HRESULT MediaStream::SetD3DManager(IUnknown* manager)
{
	RETURN_HR_IF_NULL(E_POINTER, manager);

	// comment these 2 lines to force CPU usage
	RETURN_IF_FAILED(_allocator->SetDirectXManager(manager));
	RETURN_IF_FAILED(_generator.SetD3DManager(manager, NUM_IMAGE_COLS, NUM_IMAGE_ROWS));
	return S_OK;
}
'@
    $newD3d = @'
HRESULT MediaStream::SetD3DManager(IUnknown* manager)
{
	// scrcpy publishes a CPU NV12 shared-memory frame. Keep the sample
	// allocator on CPU memory so FrameGenerator can copy it without a GPU hop.
	RETURN_HR_IF_NULL(E_POINTER, manager);
	return S_OK;
}
'@
    if (-not $stream.Contains($oldD3d)) {
        throw 'Could not patch MediaStream::SetD3DManager; pinned VCamSample source changed.'
    }
    $stream = $stream.Replace($oldD3d, $newD3d)
    Set-Content $streamPath $stream -Encoding utf8

    $generatorPath = "$Destination\VCamSampleSource\FrameGenerator.cpp"
    $generator = Get-Content $generatorPath -Raw
    $generator = $generator.Replace('#include "FrameGenerator.h"', "#include `"FrameGenerator.h`"`r`n#include `"ScrcpySharedFrame.h`"")

    $needle = @'
	*outSample = nullptr;

	// render something on image common to CPU & GPU
'@
    $replacement = @'
	*outSample = nullptr;

	// For the scrcpy transport, NV12 is already produced by scrcpy. Copy the
	// latest complete shared frame directly into the Media Foundation sample.
	if (format == MFVideoFormat_NV12 && !HasD3DManager())
	{
		RETURN_IF_FAILED(ScrcpyCopySharedNv12(sample, _width, _height));
		sample->AddRef();
		*outSample = sample;
		_frame++;
		return S_OK;
	}

	// render something on image common to CPU & GPU
'@
    if (-not $generator.Contains($needle)) {
        throw 'Could not patch FrameGenerator::Generate; pinned VCamSample source changed.'
    }
    $generator = $generator.Replace($needle, $replacement)
    Set-Content $generatorPath $generator -Encoding utf8

    # The app title passed to MFCreateVirtualCamera becomes the camera's
    # friendly name. Patch only the resource string, not C++ identifiers.
    $rcPath = "$Destination\VCamSample\VCamSample.rc"
    $rc = Get-Content $rcPath -Raw
    $rc = $rc.Replace('VCamSample', 'scrcpy Virtual Camera')
    # Revert resource identifiers accidentally touched by the broad textual
    # replacement; only user-visible strings should change.
    $rc = $rc.Replace('IDC_scrcpy Virtual Camera', 'IDC_VCAMSAMPLE')
    $rc = $rc.Replace('IDI_scrcpy Virtual Camera', 'IDI_VCAMSAMPLE')
    Set-Content $rcPath $rc -Encoding Unicode

    Copy-Item "$Destination\LICENSE" "$PSScriptRoot\VCAMSAMPLE-LICENSE" -Force
}
finally {
    Pop-Location
}

Write-Host "Prepared scrcpy Media Foundation virtual camera source at: $Destination"
Write-Host "Build VCamSample.sln for x64, register VCamSampleSource.dll, then run VCamSample.exe."
