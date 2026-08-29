param(
    [string]$Destination = "$PSScriptRoot\AudioMirror-src"
)

$ErrorActionPreference = 'Stop'

$repo = 'https://github.com/JannesP/AudioMirror.git'
$commit = 'a9618d1ab4114e3e35e8e50ae804a4205315d1f2'

if (Test-Path $Destination) {
    Remove-Item -Recurse -Force $Destination
}

git clone $repo $Destination
Push-Location $Destination
try {
    git checkout $commit

    $inf = Join-Path $Destination 'AudioMirror\AudioMirror.inf'
    $content = Get-Content $inf -Raw
    $content = $content.Replace('AudioMirror Virtual Device', 'scrcpy Virtual Audio Device')
    $content = $content.Replace('AudioMirror Wave Input', 'scrcpy Virtual Microphone')
    $content = $content.Replace('AudioMirror Topology Input', 'scrcpy Virtual Microphone Topology')
    $content = $content.Replace('AudioMirror Wave Output', 'scrcpy Virtual Microphone Feed')
    $content = $content.Replace('AudioMirror Topology Output', 'scrcpy Virtual Microphone Feed Topology')
    $content = $content.Replace('AudioMirror Drivers', 'scrcpy Virtual Audio Driver')
    Set-Content -Path $inf -Value $content -Encoding ascii

    $licenseSource = Join-Path $Destination 'LICENSE.md'
    $licenseDestination = Join-Path $PSScriptRoot 'AUDIOMIRROR-LICENSE.md'
    Copy-Item $licenseSource $licenseDestination -Force
}
finally {
    Pop-Location
}

Write-Host "Prepared pinned AudioMirror driver source at: $Destination"
Write-Host "Open AudioMirror.sln with Visual Studio + WDK and build x64."
Write-Host "The render endpoint will be named 'scrcpy Virtual Microphone Feed'."
Write-Host "The capture endpoint will be named 'scrcpy Virtual Microphone'."
