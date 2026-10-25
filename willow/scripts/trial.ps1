# One RAM-only probe: fastboot boot out/willow-ios.img, wait for TWRP again, read the log.
param([string]$Name = "willow-trial", [int]$WaitSeconds = 600)
$proj = Split-Path -Parent $PSScriptRoot
Set-Location $proj
adb reboot bootloader
$t = 0
while (-not ((fastboot devices) -match "fastboot")) { Start-Sleep 2; $t += 2; if ($t -gt 90) { Write-Host "no fastboot"; exit 1 } }
fastboot boot out/willow-ios.img
$start = Get-Date
Start-Sleep 60
while ($true) {
	$d = adb devices
	if ($d -match "recovery") { break }
	if (((Get-Date) - $start).TotalSeconds -gt $WaitSeconds) { Write-Host "TWRP did not come back in $WaitSeconds s"; exit 1 }
	Start-Sleep 5
}
Write-Host ("back in TWRP after {0:N0} s" -f ((Get-Date) - $start).TotalSeconds)
Start-Sleep 8
powershell -NoProfile -ExecutionPolicy Bypass -File read-diag.ps1 -Blocks 16384 -Name $Name
