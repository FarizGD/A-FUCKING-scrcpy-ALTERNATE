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
# service context. Provision a file-backed transport under ProgramData so
# scrcpy and Frame Server can exchange frames across Windows sessions.
$userSid = $identity.User.Value
$tempScript = Join-Path $env:TEMP "scrcpy-vcam-elevated-$PID.ps1"
$setupLog = Join-Path $env:TEMP "scrcpy-vcam-setup-$PID.log"
$escapedSource = $source.Replace("'", "''")
$escapedTransport = $transport.Replace("'", "''")
$escapedLog = $setupLog.Replace("'", "''")

$elevated = @"
`$ErrorActionPreference = 'Stop'
`$source = '$escapedSource'
`$transport = '$escapedTransport'
`$userSid = '$userSid'
`$log = '$escapedLog'

function Invoke-IcaclsGrant([string]`$ace) {
    ("Granting ACL: " + `$ace) | Add-Content -Path `$log
    & icacls.exe `$transport /grant:r `$ace 2>&1 | Tee-Object -FilePath `$log -Append | Out-Null
    if (`$LASTEXITCODE -ne 0) {
        throw "icacls grant failed for `$ace with exit code `$LASTEXITCODE"
    }
}

try {
    "Starting elevated scrcpy vcam setup" | Set-Content -Path `$log -Encoding UTF8
    New-Item -ItemType Directory -Force `$transport | Out-Null

    & icacls.exe `$transport /inheritance:r 2>&1 | Tee-Object -FilePath `$log -Append | Out-Null
    if (`$LASTEXITCODE -ne 0) { throw "icacls inheritance failed with exit code `$LASTEXITCODE" }

    # Apply one ACE per icacls invocation. This avoids PowerShell/native argv
    # parsing ambiguities when several SID expressions contain parentheses.
    `$userAce = '*' + `$userSid + ':(OI)(CI)M'
    Invoke-IcaclsGrant `$userAce
    Invoke-IcaclsGrant '*S-1-5-18:(OI)(CI)F'
    Invoke-IcaclsGrant '*S-1-5-19:(OI)(CI)RX'
    Invoke-IcaclsGrant '*S-1-5-20:(OI)(CI)RX'
    Invoke-IcaclsGrant '*S-1-5-32-544:(OI)(CI)F'

    & `$env:SystemRoot\System32\regsvr32.exe /s `$source
    if (`$LASTEXITCODE -ne 0) { throw "regsvr32 failed with exit code `$LASTEXITCODE" }

    "Elevated setup completed successfully" | Add-Content -Path `$log
}
catch {
    ("ERROR: " + `$_.Exception.Message) | Add-Content -Path `$log
    exit 1
}
"@

Set-Content -Path $tempScript -Value $elevated -Encoding UTF8
try {
    Write-Host 'Provisioning camera transport and registering Media Foundation source (UAC prompt expected)...'
    $admin = Start-Process powershell.exe -Verb RunAs `
        -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $tempScript + '"')) `
        -Wait -PassThru
    if ($admin.ExitCode -ne 0) {
        if (Test-Path $setupLog) {
            Write-Host ''
            Write-Host 'Elevated setup log:' -ForegroundColor Yellow
            Get-Content $setupLog | ForEach-Object { Write-Host "  $_" }
            Write-Host ''
        }
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

Remove-Item $setupLog -Force -ErrorAction SilentlyContinue
