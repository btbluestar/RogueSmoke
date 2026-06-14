# Testing

> What to test and how, for a co-op deterministic roguelike. Two concerns are unusually
> important here and generic projects ignore them: **determinism** and **multiplayer correctness**.

## Priorities

1. **Determinism tests** (highest value). A fixed seed must produce a fixed world.
2. **Multiplayer/authority tests.** State the server owns must not diverge on clients.
3. **Pure logic tests.** Loot rolls, cooldowns, upgrade math, cluster-bonus calculations.
4. **The legibility playtest** (manual). The signature synergy must *read* in the moment.

Don't chase coverage on cosmetic or throwaway code. Test the things that are expensive to debug
later — desyncs and non-reproducible generation are the worst offenders in this genre.

## Script tests (automated logic)

- Use the plugin's script tests for anything pure: https://angelscript.hazelight.se/scripting/script-tests/
- Good candidates: loot weighting, cooldown/recharge math, upgrade application
  (e.g. Chain Detonation raises `ClusterBonusMultiplier` and `Radius`), cluster-bonus damage.

## Determinism tests (the ones that earn their keep)

- Assert: same seed → identical floor layout, room sequence, and loot rolls.
- Run generation twice with the same seed in one process; deep-compare the results.
- Run it again after iterating actors in a different order — output must not change (catches
  iteration-order dependence, a classic determinism bug).
- Add a debug command to print/replay the active seed; reproducing a reported bug starts here.
- Forbidden in generation paths: unseeded random, wall-clock reads, hash-order iteration.

## Multiplayer / authority testing

- In the editor, set **Number of Players = 2** and **Net Mode = Play As Listen Server** to test
  host + one client locally (PIE).
- Verify for each gameplay change:
  - Damage, loot grants, and actor spawns happen **only** on the server and replicate down.
  - A client pressing an ability sends a `Server_` RPC; the client never applies the outcome itself.
  - Replicated properties (health, currency, build) match across host and client.
  - Cosmetic-only RPCs are marked `Unreliable` (CODING_STANDARDS §4.1) — watch for reliable-channel
    saturation when many fire at once.
- Test a late-join / mid-run client if/when that's supported (still open — see `DECISIONS.md`).
- Test a party wipe and a successful extraction end-to-end with two players.

## The legibility check (manual, but a real pass/fail)

From MVP arch §8 — the synergy must read clearly:
- The **pull is visible** (enemies converge).
- The **`Clustered` state has a distinct cue** (the "setup ready" language).
- The **payoff bonus is large enough to feel**.

If any of these is weak in playtest, the synergy fails its pillar. Fix presentation before
adding more content — more content on top of an illegible synergy just hides the problem.

## Debug levels + headless smoke (Claude-runnable)

Isolated `DL_*` levels under `/Game/Levels/DebuggingLevels/` boot a real raid loop (GameMode,
hero embodiment, spawn seam) around ONE thing, so they work in PIE **and** headlessly
(`-game -nullrhi`, no window) — Claude Code uses them for unattended verification.

- `DL_Combat` — general combat sandbox (taunt→cluster→barrage combo).
- `DL_Enemy_<Archetype>` — one enemy archetype each (Crawler/Carapace/Spitter/Bloater/Lunger/
  BroodMother), spawned by `AEnemyTestStand` with auto-respawn.
- `DL_Upgrades` — the upgrade firing range: `AUpgradeTestRange` spawns passive dummy formations
  shaped per upgrade class — SOLO (damage/burn/poison), LINE (pierce), CLUSTER (chain arcs).

**Debug console execs** (type in `~`, or pass headlessly via `-ExecCmds="..."` — they poll for
up to 30s so they survive firing before the hero spawns):
- `ListUpgrades` / `GrantUpgrade <partial name>` (repeat to stack) / `GrantAllUpgrades`
- `UpgradeSmoke` — applies every pool upgrade, asserts each GE moves an attribute
  (`[UpgradeSmoke] RESULT n/n`); `WeaponSmoke` — fires one synthetic pierce+chain+DoT shot
  and re-reads the victim 2s later.
- `TelegraphSmoke` — rings a telegraph danger zone at the hero (cue-pass visual, no damage);
  headlessly, grep the log for `[Telegraph] zone`.
- `RaidGiveXP <n>` — grant shared team XP (100 = one level at the base curve → paused offer);
  `RaidKillOneElite` — kill the nearest objective elite (steps the XP/chest flow one kill at a
  time); `RaidGoToChest` — teleport onto the dropped mini-boss chest so it proximity-opens.
  Grep `[XP]`, `[Upgrades]`, `[Chest]`.
- `RaidDebugCam`, `RaidKillElites` (prints live count + total dealt), `RaidRestart`,
  `RaidWin` / `RaidLose`, `RaidResults`, `RaidPause`.

**Harnesses** (in `Tools/`):
- `SmokeTest.ps1` — boots every level above, asserts spawn breadcrumbs + no fatals (PASS/FAIL table).
- `BootLevel.ps1 -Map <path> [-Exec "<cmds>"] [-Grep <pattern>]` — one level, one exec battery,
  grepped log. AngelScript `Print()` lines land in the log as `LogBlueprintUserMessages`.

## Pre-commit smoke checklist

- [ ] Scripts compile (no broken `.as`).
- [ ] C++ module builds.
- [ ] PIE launches and the smoke-test path runs.
- [ ] A 2-player listen-server PIE session starts without immediate desync/errors.
