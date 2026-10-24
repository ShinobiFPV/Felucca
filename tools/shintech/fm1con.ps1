# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 William Kew, ShinTech Electronics
# Drive the FM-1's USB serial console (Felucca CDC) on COM4: send each command, wait, print what comes back.
#   fm1con.ps1 -Cmds "status","bench" -Wait 1500
# Fails (throws) if the port is missing or drops: callers must stop, never retry into a crashing device
# (two crashes in the first 30 s of a boot send the FM-1 to UBOOT: main.c bootguard).
param([string[]]$Cmds = @("status"), [int]$Wait = 1200, [string]$Port = "COM4")
$ErrorActionPreference = 'Stop'
$p = New-Object System.IO.Ports.SerialPort $Port, 115200, 'None', 8, 'One'
$p.DtrEnable = $true
$p.RtsEnable = $true
$p.ReadTimeout = 500
$p.Open()
try {
    Start-Sleep -Milliseconds 600
    $out = $p.ReadExisting()
    foreach ($c in $Cmds) {
        if ($c -match '^sleep (\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
        $p.Write("$c`r")
        Start-Sleep -Milliseconds $Wait
        $out += $p.ReadExisting()
    }
} finally {
    if ($p.IsOpen) { $p.Close() }
}
$out -replace "`r", ""
