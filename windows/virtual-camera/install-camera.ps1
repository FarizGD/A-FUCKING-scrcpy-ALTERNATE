$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'scrcpy-vcam-source.dll'
$helper = Join-Path $PSScriptRoot 'scrcpy-vcam-register.exe'

if ([Environment]::OSVersion.Version.Build -lt 22000) {
    throw 'scrcpy Virtual Camera requires Windows 11 build 22000 or newer.'
}

if (-not (Test-Path $source)) {
    throw "Missing $source"
}
if (-not (Test-Path $helper)) {
    throw "Missing $helper"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    Write-Host 'Administrator permission is required once to register the Media Foundation source.'
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"' + $PSCommandPath + '"')
    )
    Start-Process powershell.exe -Verb RunAs -ArgumentList $args -Wait
    exit $LASTEXITCODE
}

Write-Host 'Registering scrcpy Media Foundation camera source...'
$reg = Start-Process "$env:SystemRoot\System32\regsvr32.exe" `
    -ArgumentList @('/s', ('"' + $source + '"')) `
    -Wait -PassThru
if ($reg.ExitCode -ne 0) {
    throw "regsvr32 failed with exit code $($reg.ExitCode)"
}

Write-Host 'Starting scrcpy Virtual Camera registration helper...'
Start-Process $helper -WorkingDirectory $PSScriptRoot

Write-Host ''
Write-Host 'scrcpy Virtual Camera source registered.'
Write-Host 'Keep scrcpy-vcam-register.exe running during this development milestone.'
Write-Host 'Then start scrcpy with SCRCPY_WIN_VCAM=1 and 1280x720 camera capture.'
