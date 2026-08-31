$ErrorActionPreference = 'Stop'

$serviceName = 'ScrcpyBridgeService'
$exe = Join-Path $PSScriptRoot 'scrcpy-bridge-service.exe'

if (-not (Test-Path $exe)) {
    throw "Missing service executable: $exe"
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this installer from an Administrator PowerShell window.'
}

$existing = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($existing) {
    if ($existing.Status -ne 'Stopped') {
        Stop-Service -Name $serviceName -Force
    }
    & sc.exe delete $serviceName | Out-Host
    Start-Sleep -Milliseconds 800
}

& sc.exe create $serviceName binPath= "`"$exe`"" start= delayed-auto obj= LocalSystem DisplayName= "scrcpy Camera + VB-CABLE Bridge" | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "sc.exe create failed with exit code $LASTEXITCODE"
}

& sc.exe description $serviceName "Keeps the scrcpy virtual-camera registrar and scrcpy-to-VB-CABLE microphone bridge running in the active desktop session." | Out-Host
& sc.exe failure $serviceName reset= 86400 actions= restart/3000/restart/3000/restart/10000 | Out-Host

Start-Service -Name $serviceName
Write-Host ''
Write-Host 'Installed and started ScrcpyBridgeService.' -ForegroundColor Green
Write-Host 'Mic target: VB-CABLE render endpoint containing "CABLE Input".'
Write-Host 'Camera registrar: scrcpy-vcam-register.exe --headless.'
Write-Host 'The service retries the mic worker automatically until scrcpy creates Local\ScrcpyVirtualMicrophonePcm.'
