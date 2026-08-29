$ErrorActionPreference = 'Stop'

$source = Join-Path $PSScriptRoot 'scrcpy-vcam-source.dll'

Get-Process 'scrcpy-vcam-register' -ErrorAction SilentlyContinue | Stop-Process -Force

if (-not (Test-Path $source)) {
    throw "Missing $source"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

if (-not $isAdmin) {
    $args = @(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"' + $PSCommandPath + '"')
    )
    Start-Process powershell.exe -Verb RunAs -ArgumentList $args -Wait
    exit $LASTEXITCODE
}

Write-Host 'Unregistering scrcpy Media Foundation camera source...'
$reg = Start-Process "$env:SystemRoot\System32\regsvr32.exe" `
    -ArgumentList @('/u', '/s', ('"' + $source + '"')) `
    -Wait -PassThru
if ($reg.ExitCode -ne 0) {
    throw "regsvr32 /u failed with exit code $($reg.ExitCode)"
}

Write-Host 'scrcpy Virtual Camera source unregistered.'
