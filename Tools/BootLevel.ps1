<#
.SYNOPSIS
  Boot one level headlessly (-game -nullrhi) with optional console execs, capture the log.

.DESCRIPTION
  The single-level building block behind SmokeTest.ps1 — use it to debug ONE level or run a
  debug-exec battery (UpgradeSmoke, WeaponSmoke, GrantUpgrade ...) without the full suite.
  The game free-runs (no auto-quit), so it gets killed after -WindowSec. AngelScript Print()
  breadcrumbs land in the log as LogBlueprintUserMessages lines; -Grep filters them out for you.

.EXAMPLE
  .\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "UpgradeSmoke" -Grep "Upgrade"
.EXAMPLE
  .\BootLevel.ps1 -Map /Game/Levels/RaidArena -Exec "GrantAllUpgrades, WeaponSmoke" -WindowSec 45
#>

param(
    [Parameter(Mandatory = $true)] [string]$Map,
    [string]$Exec = "",
    [int]$WindowSec = 40,
    [string]$Log = (Join-Path $env:TEMP "rs_boot.log"),
    [string]$Grep = ""
)

$ErrorActionPreference = "Stop"

$EditorCmd = "F:\UEAS\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Project   = "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\RogueSmoke.uproject"

if (Test-Path $Log) { Remove-Item $Log }

$a = @($Project, $Map, "-game", "-unattended", "-nullrhi", "-nosound", "-nosplash", "-abslog=$Log")
if ($Exec -ne "") { $a += "-ExecCmds=`"$Exec`"" }

$p = Start-Process -FilePath $EditorCmd -ArgumentList $a -PassThru -WindowStyle Hidden
Start-Sleep -Seconds $WindowSec
if (-not $p.HasExited) { $p.Kill() }
Start-Sleep -Milliseconds 600  # let the log flush

if (-not (Test-Path $Log)) {
    Write-Host "BOOT FAILED: no log at $Log" -ForegroundColor Red
    exit 1
}

if ($Grep -ne "") {
    Select-String -Path $Log -Pattern $Grep | ForEach-Object Line
} else {
    Write-Host "Log: $Log"
}
exit 0
