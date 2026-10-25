# Reads the text log at the start of the phone's logdump partition: the HackiOS UEFI diagnostics
# report (XnuLoader.c, "XNULOADER LOG v1") or the XNU loader's log with the kernel's in it.
# Run from TWRP (adb is root there). Only reads the phone.
#   read-diag.ps1 [-Blocks 64] [-Name willow-diag]    (4 KiB blocks; the kernel run needs ~1024)
param([int]$Blocks = 64, [string]$Name = "willow-diag")
$out = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Force $out | Out-Null
$raw = Join-Path $out "$Name-logdump.bin"
adb shell "dd if=/dev/block/by-name/logdump of=/tmp/logdump-head.bin bs=4096 count=$Blocks"
adb pull /tmp/logdump-head.bin $raw
$bytes = [System.IO.File]::ReadAllBytes($raw)
$end = [Array]::IndexOf($bytes, [byte]0)
if ($end -lt 0) { $end = $bytes.Length }
if ($end -eq 0) {
	Write-Host "logdump is empty at the start: nothing was written"
	exit 1
}
$text = [System.Text.Encoding]::ASCII.GetString($bytes, 0, $end)
$log = Join-Path $out "$Name.txt"
Set-Content -Path $log -Value $text -Encoding UTF8
Write-Host "$end bytes of text; saved: $log"
