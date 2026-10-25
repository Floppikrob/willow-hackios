# While a probe runs: what Windows makes of the phone on the cable, every 5 s, changes only.
param([int]$Seconds = 420, [string]$Out = (Join-Path (Split-Path -Parent $PSScriptRoot) "out\winwatch.txt"))
"" | Set-Content $Out -Encoding UTF8
$last = ""
$start = Get-Date
while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
	$now = @()
	$now += Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match "VID_1209" } |
		ForEach-Object { "pnp: $($_.Status) $($_.Class) '$($_.FriendlyName)' $($_.InstanceId)" }
	$now += Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -match "NCM|Remote NDIS|UsbNcm" -or $_.MacAddress -match "02-48-69" } |
		ForEach-Object { "net: '$($_.Name)' ifIndex $($_.ifIndex) $($_.Status) $($_.LinkSpeed) MAC $($_.MacAddress) '$($_.InterfaceDescription)'" }
	foreach ($a in (Get-NetAdapter -ErrorAction SilentlyContinue | Where-Object { $_.MacAddress -match "02-48-69" })) {
		$now += Get-NetIPAddress -InterfaceIndex $a.ifIndex -ErrorAction SilentlyContinue | ForEach-Object { "ip: $($_.IPAddress)/$($_.PrefixLength) $($_.AddressState)" }
		$now += Get-NetNeighbor -InterfaceIndex $a.ifIndex -ErrorAction SilentlyContinue | Where-Object { $_.State -ne "Permanent" -and $_.LinkLayerAddress -match "02-48-69" } |
			ForEach-Object { "neighbor: $($_.IPAddress) $($_.LinkLayerAddress) $($_.State)" }
		$s = Get-NetAdapterStatistics -Name $a.Name -ErrorAction SilentlyContinue
		if ($s) { $now += "stats: sent $($s.SentUnicastPackets)+$($s.SentBroadcastPackets)+$($s.SentMulticastPackets) received $($s.ReceivedUnicastPackets)+$($s.ReceivedBroadcastPackets)+$($s.ReceivedMulticastPackets)" }
	}
	$text = $now -join "`n"
	if ($text -ne $last) {
		$t = [int]((Get-Date) - $start).TotalSeconds
		"--- $t s" | Add-Content $Out -Encoding UTF8
		$text | Add-Content $Out -Encoding UTF8
		$last = $text
	}
	Start-Sleep 5
}
