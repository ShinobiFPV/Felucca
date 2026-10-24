# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
# The cost split on core 1, typical load (released operators go idle: no SSG-EG): whole chip vs clocked only, keys held vs all released (idle).
param([int]$Duty = 8, [int]$Settle = 4500)
$ErrorActionPreference = 'Stop'
$s = Join-Path $PSScriptRoot 'fm1con.ps1'
function Get-Kv([string]$text, [string]$key) {
    $l = ($text -split "`n" | Where-Object { $_ -match "^$key " } | Select-Object -Last 1)
    if ($l) { [int]($l -split ' ')[1] } else { $null }
}
$up0 = Get-Kv ((& $s -Cmds "status" -Wait 500) -join "`n") 'uptime_ms'
if ($null -eq $up0) { throw "no status from the FM-1" }
$out = (& $s -Cmds "bench vol 0", "bench c1 0", "bench load 1", "bench duty $Duty", "bench probe 0", "bench ymfm", "sleep $Settle",
        "bench", "bench probe 1", "sleep $Settle", "bench", "bench probe 0", "bench keys 0", "sleep $Settle", "bench",
        "bench probe 1", "sleep $Settle", "bench", "bench off", "status" -Wait 500) -join "`n"
$up = Get-Kv $out 'uptime_ms'
if ($null -eq $up -or $up -lt $up0) { throw "the FM-1 rebooted: stopping" }
$est = ($out -split "`n" | Where-Object { $_ -match '^est_permille' } | ForEach-Object { [int]($_ -split ' ')[1] })
# the figure after each settle: the 'bench' lines follow the sleeps (8th, 11th, 14th, 17th commands)
$b = ($out -split "> bench`n" | Select-Object -Skip 1)
"keys on,  whole chip:   {0}" -f (Get-Kv $b[0] 'est_permille')
"keys on,  clocked only: {0}" -f (Get-Kv $b[1] 'est_permille')
"keys off, whole chip:   {0}" -f (Get-Kv $b[2] 'est_permille')
"keys off, clocked only: {0}" -f (Get-Kv $b[3] 'est_permille')
