# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
# One benchmark round over the FM-1 console: ymfm, worst and typical loads, on core 1 then core 2. Prints a table.
# Stops at the first sign of trouble: the port gone, or the uptime gone back (the device rebooted). It never starts the
# benchmark again on a device that just rebooted.
param([int]$Duty = 8, [int]$Settle = 4500)
$ErrorActionPreference = 'Stop'
$s = Join-Path $PSScriptRoot 'fm1con.ps1'
function Get-Kv([string]$text, [string]$key) {
    $l = ($text -split "`n" | Where-Object { $_ -match "^$key " } | Select-Object -Last 1)
    if ($l) { [int]($l -split ' ')[1] } else { $null }
}
$up0 = Get-Kv ((& $s -Cmds "status" -Wait 500) -join "`n") 'uptime_ms'
if ($null -eq $up0) { throw "no status from the FM-1" }
$rows = @()
foreach ($c1 in 0, 1) {
    foreach ($load in 0, 1) {
        $out = (& $s -Cmds "status", "bench vol 0", "bench probe 0", "bench keys 1", "bench c1 $c1", "bench load $load", "bench duty $Duty", "bench ymfm",
                "sleep $Settle", "bench", "bench off" -Wait 500) -join "`n"
        $up = Get-Kv $out 'uptime_ms'
        if ($null -eq $up -or $up -lt $up0) { throw "the FM-1 rebooted (uptime $up ms < $up0 ms): stopping" }
        $up0 = $up
        $rows += "core{0} load{1}: est_permille {2,-5} cpu_pct {3,-3} tripped {4} c1_failed {5}" -f ($c1 + 1), $load,
            (Get-Kv $out 'est_permille'), (Get-Kv $out 'cpu_pct'), (Get-Kv $out 'tripped'), (Get-Kv $out 'c1_failed')
    }
}
$st = (& $s -Cmds "status" -Wait 500) -join "`n"
"audio_late {0}  cpu2_gave_up {1}  cpu2_bad {2}  cpu2_wait_max_us {3}  uptime_ms {4}" -f (Get-Kv $st 'audio_late'),
    (Get-Kv $st 'cpu2_gave_up'), (Get-Kv $st 'cpu2_bad'), (Get-Kv $st 'cpu2_wait_max_us'), (Get-Kv $st 'uptime_ms')
$rows
