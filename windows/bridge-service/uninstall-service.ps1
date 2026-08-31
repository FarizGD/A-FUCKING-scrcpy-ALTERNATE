$ErrorActionPreference = 'Stop'

$serviceName = 'ScrcpyBridgeService'
$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if (-not $service) {
    Write-Host 'ScrcpyBridgeService is not installed.'
    exit 0
}

if ($service.Status -ne 'Stopped') {
    Stop-Service -Name $serviceName -Force
}

& sc.exe delete $serviceName | Out-Host
if ($LASTEXITCODE -ne 0) {
    throw "sc.exe delete failed with exit code $LASTEXITCODE"
}

Write-Host 'ScrcpyBridgeService removed.' -ForegroundColor Green
