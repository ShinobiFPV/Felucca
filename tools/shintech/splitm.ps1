# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
# bench split: the chip on two cores. For each load: core 1 alone at duty 8 (reference), then split at duty 8, 16, 24,
# 32, stopping when the whole ISR passes 85 % (the benchmark trips at 90 %). Stops on any reboot.
param([int]$Settle = 4000)
$ErrorActionPreference = 'Stop'
$s = Join-Path $PSScriptRoot 'fm1con.ps1'
function Get-Kv([string]$text, [string]$key) {
    $l = ($text -split "`n" | Where-Object { $_ -match "^$key " } | Select-Object -Last 1)
    if ($l) { [int]($l -split ' ')[1] } else { $null }
}
$up0 = Get-Kv ((& $s -Cmds "status" -Wait 500) -join "`n") 'uptime_ms'
if ($null -eq $up0) { throw "no status from the FM-1" }
foreach ($load in 0, 1) {
    $o = (& $s -Cmds "status", "bench vol 0", "bench probe 0", "bench split 0", "bench c1 0", "bench load $load", "bench duty 8",
          "bench ymfm", "sleep $Settle", "bench" -Wait 500) -join "`n"
    $up = Get-Kv $o 'uptime_ms'; if ($null -eq $up -or $up -lt $up0) { throw "rebooted: stopping" }; $up0 = $up
    "load{0} core1 alone, duty 8:  chip {1} permille of a core, ISR {2} %" -f $load, (Get-Kv $o 'est_permille'), (Get-Kv $o 'cpu_pct')
    foreach ($duty in 8, 16, 24, 32) {
        $o = (& $s -Cmds "status", "bench split 1", "bench duty $duty", "sleep $Settle", "bench" -Wait 500) -join "`n"
        $up = Get-Kv $o 'uptime_ms'; if ($null -eq $up -or $up -lt $up0) { throw "rebooted: stopping" }; $up0 = $up
        $cpu = Get-Kv $o 'cpu_pct'
        "load{0} split, duty {1,2}:      core1 part {2}, core2 part {3} permille; ISR {4} %; tripped {5} c1_failed {6}" -f $load, $duty,
            (Get-Kv $o 'est_permille'), (Get-Kv $o 'est1_permille'), $cpu, (Get-Kv $o 'tripped'), (Get-Kv $o 'c1_failed')
        if ($cpu -gt 85 -or (Get-Kv $o 'tripped') -ne 0) { break }
    }
    & $s -Cmds "bench off", "bench split 0" -Wait 400 | Out-Null
}
$st = (& $s -Cmds "status" -Wait 500) -join "`n"
"audio_late {0}  cpu2_gave_up {1}  cpu2_bad {2}  cpu2_wait_max_us {3}" -f (Get-Kv $st 'audio_late'), (Get-Kv $st 'cpu2_gave_up'),
    (Get-Kv $st 'cpu2_bad'), (Get-Kv $st 'cpu2_wait_max_us')
