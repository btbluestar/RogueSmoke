<#
.SYNOPSIS
  Headless regression smoke test for RogueSmoke enemies + the raid loop.

.DESCRIPTION
  Boots each level as a standalone game (-game -nullrhi, no window) for a few seconds and asserts,
  from the log, that (a) the run starts, (b) the expected enemies spawn, and (c) nothing fatals
  (no "Fatal error", "Assertion failed", or AngelScript "Script call stack"). Prints a PASS/FAIL
  table and exits 1 if any level fails.

  This is the cheap "did my change break an enemy / the loop?" gate while the interactive PIE MCP is
  wedged. It relies on the [Raid]/[EnemyTest] spawn breadcrumbs (RaidObjective.as / EnemyTestStand.as).
  Run it after enemy/loop/seam changes (a clean editor build should already be done).

.NOTES
  Windows + the AngelScript engine fork. Adjust $EditorCmd / $Project below if paths differ.
  Each level free-runs (no auto-quit), so we kill it after $SpawnWindowSec. Increase that on a slow box.
#>

param(
    [int]$SpawnWindowSec = 14
)

$ErrorActionPreference = "Stop"

$EditorCmd = "F:\UEAS\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$Project   = "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\RogueSmoke.uproject"
$LogDir    = Join-Path $env:TEMP "rs_smoke"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

# Each: map path, and the spawn breadcrumb(s) that prove its content spawned (Expect may be an
# array — all must appear). Optional: Exec (passed as -ExecCmds, for debug-exec batteries) and
# Window (per-case spawn window in seconds, when the exec needs longer than the default).
$Cases = @(
    @{ Name = "RaidArena";          Map = "/Game/Levels/RaidArena";                       Expect = "[Raid] spawned 4 ring elites + boss" }
    @{ Name = "Enemy_Crawler";      Map = "/Game/Levels/DebuggingLevels/DL_Enemy_Crawler";     Expect = "CRAWLER (fodder): spawned 8" }
    @{ Name = "Enemy_Carapace";     Map = "/Game/Levels/DebuggingLevels/DL_Enemy_Carapace";    Expect = "CARAPACE: spawned 2" }
    @{ Name = "Enemy_Spitter";      Map = "/Game/Levels/DebuggingLevels/DL_Enemy_Spitter";     Expect = "SPITTER: spawned 2" }
    @{ Name = "Enemy_Bloater";      Map = "/Game/Levels/DebuggingLevels/DL_Enemy_Bloater";     Expect = "BLOATER: spawned 2" }
    @{ Name = "Enemy_Lunger";       Map = "/Game/Levels/DebuggingLevels/DL_Enemy_Lunger";      Expect = "LUNGER: spawned 2" }
    @{ Name = "Enemy_BroodMother";  Map = "/Game/Levels/DebuggingLevels/DL_Enemy_BroodMother"; Expect = "BROOD-MOTHER (boss): spawned 1" }
    # Upgrade firing range + the GE->attribute battery: every pool upgrade must move an attribute.
    @{ Name = "Upgrades";           Map = "/Game/Levels/DebuggingLevels/DL_Upgrades";
       Expect = @("[UpgradeTest] range ready: solo=1 line=4 cluster=5", "[UpgradeSmoke] RESULT 24/24", "[FlowSmoke] RESULT 6/6")
       Exec = "UpgradeSmoke, UpgradeFlowSmoke"; Window = 60 }
)

$FatalPatterns = @("Fatal error", "Assertion failed", "Script call stack", "LogScript: Error")
$results = @()

foreach ($c in $Cases) {
    $log = Join-Path $LogDir ("$($c.Name).log")
    if (Test-Path $log) { Remove-Item $log }
    $a = @($Project, $c.Map, "-game", "-unattended", "-nullrhi", "-nosound", "-nosplash", "-abslog=$log")
    if ($c.Exec) { $a += "-ExecCmds=`"$($c.Exec)`"" }
    $win = if ($c.Window) { $c.Window } else { $SpawnWindowSec }
    $p = Start-Process -FilePath $EditorCmd -ArgumentList $a -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds $win
    if (-not $p.HasExited) { $p.Kill() }
    Start-Sleep -Milliseconds 400  # let the log flush

    $text = if (Test-Path $log) { Get-Content $log -Raw } else { "" }
    $started = $text -match "\[RunManager\] Run started"
    $spawned = $true
    foreach ($e in @($c.Expect)) { if (-not $text.Contains($e)) { $spawned = $false } }
    $fatal   = $false
    foreach ($fp in $FatalPatterns) { if ($text -match [regex]::Escape($fp)) { $fatal = $true } }

    $pass = $started -and $spawned -and (-not $fatal)
    $results += [pscustomobject]@{
        Level = $c.Name; Started = $started; Spawned = $spawned; NoFatal = (-not $fatal); Result = if ($pass) { "PASS" } else { "FAIL" }
    }
}

""
$results | Format-Table -AutoSize
$failed = @($results | Where-Object { $_.Result -eq "FAIL" })
if ($failed.Count -gt 0) {
    Write-Host "SMOKE TEST FAILED: $($failed.Count) level(s). Logs in $LogDir" -ForegroundColor Red
    exit 1
}
Write-Host "SMOKE TEST PASSED: all $($results.Count) levels." -ForegroundColor Green
exit 0
