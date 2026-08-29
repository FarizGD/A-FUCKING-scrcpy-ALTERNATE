$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'scrcpy-vcam-source.dll'
$helper = Join-Path $PSScriptRoot 'scrcpy-vcam-register.exe'
$transport = 'C:\ProgramData\scrcpy-vcam'

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

if ($isAdmin) {
    Write-Warning 'Do not run this installer from an Administrator PowerShell.'
    Write-Warning 'The virtual camera uses CurrentUser access and its registration helper must run unelevated.'
    Write-Host ''
    Write-Host 'Open a normal PowerShell window and run:'
    Write-Host '  Set-ExecutionPolicy -Scope Process Bypass'
    Write-Host '  .\install-camera.ps1'
    exit 1
}

# The Media Foundation camera source is loaded by Windows Frame Server in a
# service context. A Local\\ named mapping created by scrcpy is session-scoped,
# so the service cannot see it. Provision a file-backed transport under
# ProgramData instead: the current user can write it and Frame Server service
# identities can read it across sessions.
$userSid = $identity.User.Value
$tempScript = Join-Path $env:TEMP "scrcpy-vcam-elevated-$PID.ps1"
$escapedSource = $source.Replace("'", "''")
$escapedTransport = $transport.Replace("'", "''")

$elevated = @"
`$ErrorActionPreference = 'Stop'
`$source = '$escapedSource'
`$transport = '$escapedTransport'
`$userSid = '$userSid'

New-Item -ItemType Directory -Force `$transport | Out-Null

# Remove inherited ACLs, then grant only the identities required for the
# transport. Numeric SIDs avoid localized account-name problems.
& icacls.exe `$transport /inheritance:r | Out-Null
& icacls.exe `$transport /grant:r `
    "*`$userSid`:(OI)(CI)M" `
    '*S-1-5-18:(OI)(CI)F' `
    '*S-1-5-19:(OI)(CI)RX' `
    '*S-1-5-20:(OI)(CI)RX' `
    '*S-1-5-32-544:(OI)(CI)F' | Out-Null
if (`$LASTEXITCODE -ne 0) { throw "icacls failed with exit code `$LASTEXITCODE" }

& `$env:SystemRoot\System32\regsvr32.exe /s `$source
if (`$LASTEXITCODE -ne 0) { throw "regsvr32 failed with exit code `$LASTEXITCODE" }
"@

Set-Content -Path $tempScript -Value $elevated -Encoding UTF8
try {
    Write-Host 'Provisioning camera transport and registering Media Foundation source (UAC prompt expected)...'
    $admin = Start-Process powershell.exe -Verb RunAs `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $tempScript + '"')) `
        -Wait -PassThru
    if ($admin.ExitCode -ne 0) {
        throw "Elevated camera setup failed with exit code $($admin.ExitCode)"
    }
}
finally {
    Remove-Item $tempScript -Force -ErrorAction SilentlyContinue
}

Write-Host 'Starting scrcpy Virtual Camera registration helper as the current user...'
Start-Process $helper -WorkingDirectory $PSScriptRoot

Write-Host ''
Write-Host 'scrcpy Virtual Camera source registered.'
Write-Host "Cross-session frame transport: $transport\frames.bin"
Write-Host 'Keep scrcpy-vcam-register.exe running during this development milestone.'
Write-Host 'Then start the matching scrcpy build with SCRCPY_WIN_VCAM=1 and 1280x720 camera capture.'
