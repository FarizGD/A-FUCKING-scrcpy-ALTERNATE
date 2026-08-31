param(
    [string]$PackageDir = $PSScriptRoot
)

$ErrorActionPreference = 'Stop'

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this script from an elevated (Administrator) PowerShell window.'
    }
}

Assert-Administrator

$PackageDir = (Resolve-Path $PackageDir).Path
$inf = Join-Path $PackageDir 'scrcpy-vmic.inf'
$sys = Join-Path $PackageDir 'scrcpy-vmic.sys'
$cat = Join-Path $PackageDir 'scrcpy-vmic.cat'
$cer = Join-Path $PackageDir 'scrcpy-vmic-test.cer'

foreach ($file in @($inf, $sys, $cat, $cer)) {
    if (-not (Test-Path $file)) {
        throw "Missing package file: $file"
    }
}

try {
    $secureBoot = Confirm-SecureBootUEFI -ErrorAction Stop
    if ($secureBoot) {
        throw 'Secure Boot is enabled. This development driver uses a self-signed test certificate. Disable Secure Boot in UEFI before installing it.'
    }
}
catch [System.PlatformNotSupportedException] {
    Write-Warning 'Secure Boot status could not be queried on this system.'
}

$bcd = bcdedit /enum 2>&1 | Out-String
if ($bcd -notmatch '(?im)^\s*testsigning\s+Yes\s*$') {
    throw "Windows Test Signing mode is not enabled. Run 'bcdedit /set testsigning on' as Administrator, reboot, then run this installer again."
}

Write-Host 'Trusting the ephemeral scrcpy virtual microphone test certificate...'
Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $cer -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

Write-Host 'Adding the test-signed driver package to the driver store...'
& pnputil.exe /add-driver $inf /install
if ($LASTEXITCODE -ne 0) {
    throw "pnputil failed with exit code $LASTEXITCODE"
}

$devcon = Get-Command devcon.exe -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $devcon) {
    $kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Tools'
    if (Test-Path $kitsRoot) {
        $devcon = Get-ChildItem $kitsRoot -Recurse -Filter devcon.exe -File -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '(?i)\\x64\\devcon\.exe$' } |
            Sort-Object FullName -Descending |
            Select-Object -First 1
    }
}

if (-not $devcon) {
    Write-Warning 'The driver package was staged successfully, but devcon.exe was not found, so the Root\AudioMirror device could not be created automatically.'
    Write-Host 'Install the Windows Driver Kit (WDK), then run:'
    Write-Host "  devcon install `"$inf`" Root\AudioMirror"
    exit 0
}

$devconPath = if ($devcon.PSObject.Properties.Name -contains 'Source') { $devcon.Source } else { $devcon.FullName }
Write-Host "Creating/updating Root\AudioMirror with: $devconPath"
& $devconPath install $inf 'Root\AudioMirror'
if ($LASTEXITCODE -ne 0) {
    throw "devcon failed with exit code $LASTEXITCODE"
}

Write-Host ''
Write-Host 'scrcpy Virtual Microphone driver installed.'
Write-Host 'Expected endpoints:'
Write-Host '  Capture:     scrcpy Virtual Microphone'
Write-Host '  Render/feed: scrcpy Virtual Microphone Feed'
