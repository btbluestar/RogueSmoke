# Core-Loop Hardening Pass

> **For agentic workers:** AngelScript + PowerShell only. Verify with `as-helper run_code_test`
> (compile) and `Tools/SmokeTest.ps1` (interactive editor CLOSED). No feel-test, no design fork,
> independent of the shelved `lyra-anim-migration` work. Branch: `core-loop-hardening`.

**Goal:** Prove and tighten the in-run roguelike loop — the thing GDD §6.3 says to get right
*before* building meta-progression ("don't build a meta layer on top of an unproven core").

**Context (verified 2026-06-13):** The in-run core loop is already **functionally complete** —
combat seam, all six enemy archetypes (Crawler/Carapace/Spitter/Bloater/Lunger/Brood-mother),
movement, upgrades v3, `RaidObjective` clear→extract→defend, `DownComponent` down/revive, and
**both** win (`Extracted`→Victory) and loss (`NotifyPartyWiped`→`Failed`→Defeat) conditions are
wired. The docs lagged badly and cost real investigation time:
- D-0017 "Open / polish" lists "projectile + line-of-sight for Spitter" and "smooth dash for
  Lunger" as TODO — **both are implemented** (`Spitter.as` `ASpitterProjectile` + `HasLineOfSight
  ToActor`; `Lunger.as` `StartDash` multi-frame dash + `DashContactRange`).
- `RaidObjective.as:12-13` says "the defend-wave spawner isn't built" and "party-wipe depends on
  down/revive (not built)" — **both are built** (`OnExtractionPhaseStarted` calls
  `Director.SpawnEliteWave`; `DownComponent.CheckPartyWipe`→`NotifyPartyWiped`).

So the gap is **proof and one real default**, not new features.

---

## Task 1: Doc / comment freshness  *(done in this pass — no editor needed)*

**Files:** `RogueSmoke/Script/Objective/RaidObjective.as`, `DECISIONS.md`

- [x] `RaidObjective.as:12-13` — rewrite the two stale "not built" bullets to reflect that the
      defend-wave spawner and party-wipe loss are wired (note the one real caveat: defend wave is
      empty unless `DefendWaveEliteClass` is set — see Task 3).
- [x] `DECISIONS.md` D-0017 "Open / polish" — strike the Spitter projectile/LoS and Lunger dash
      items (done); leave genuinely-open items (boss healthbar, telegraph/death GameplayCues,
      per-archetype art) which need the user's eyes.

## Task 2: `RaidLoopSmoke` — automated win/loss proof  *(the headline deliverable)*

**Files:** `RogueSmoke/Script/Player/RaidPlayerController.as` (new exec),
`Tools/SmokeTest.ps1` (new case(s)).

No automated test currently drives the raid to Victory or Defeat. Add a parameterized exec that
proves each bridge end-to-end. Each outcome ends the run, so they run in **separate boots**
(mirrors how `EvoSmoke` boots separately from `UpgradeSmoke`).

Pattern to follow: `MoveSmoke` (`RaidPlayerController.as:715`) — retry-poll via
`System::SetTimer(this, n"RaidLoopSmoke", 1.0, false)` until the world is ready, synchronous
asserts, and a final `[RaidLoopSmoke] RESULT n/m` line that `SmokeTest.ps1` greps.

- [ ] **`UFUNCTION(Exec) void RaidLoopSmoke(FString Mode = "victory")`**

  **Victory path** (`Mode == "victory"`):
  1. Find the `ARaidObjective` (`GetAllActorsOfClass`); retry-poll if absent/`InProgress` before
     elites have spawned.
  2. Set `Objective.DefendWaveEliteClass = ACarapace` (test the spawner mechanism without
     imposing a shipping default — that's Task 3) and a tiny `ExtractionDefendSeconds` (e.g. 0.2).
  3. Clear the gating elites (reuse the `RaidKillElites` body — extract it to a private
     `KillAllElites()` helper both execs call). Poll until `Phase == ExtractionReady`.
  4. Record `UCombatSubsystem::Get().GetEliteCount()`, `CallExtraction()`, assert
     `Phase == Extracting` **and** elite count rose by ~`DefendWaveCount` (defend wave spawned).
  5. Poll until `Phase == Extracted` and `GetGameState().Phase == ERunPhase::Victory`
     (the objective→RunManager bridge). `RESULT k/4`.

  **Defeat path** (`Mode == "defeat"`):
  1. Find the `ARaidObjective`.
  2. Incapacitate every `AHeroCharacter` (set Health attribute to 0 via the ASC, the same path
     `DownComponent` watches — or call `NotifyPartyWiped()` directly as the minimum bridge proof).
  3. Poll until `Phase == ERaidPhase::Failed` and `GetGameState().Phase == ERunPhase::Defeat`.
     `RESULT k/2`.

  Notes: gate authority (`HasAuthority()`); the exec mutates/ends run state, which is fine in a
  disposable smoke boot. Keep all reads synchronous after each driving call where possible.

- [ ] **`SmokeTest.ps1`** — add `RaidLoopVictory` and `RaidLoopDefeat` cases booting the real raid
      level (DL_Combat boots the full loop headlessly — see debug-levels memory) with
      `-ExecCmds="RaidLoopSmoke victory"` / `"RaidLoopSmoke defeat"`, asserting the `RESULT 4/4`
      and `RESULT 2/2` lines. Bumps the suite from 9 to 11 cases.

- [ ] **Verify:** `as-helper run_code_test` compiles; `SmokeTest.ps1` all green (editor closed).

## Task 3: Default the defend wave  *(gameplay tuning — needs the user's nod)*

**File:** `RogueSmoke/Script/Objective/RaidObjective.as`

`DefendWaveEliteClass` is the only roster on the objective **not** defaulted in `BeginPlay`
(`EliteRoster`/`BossClass`/`InjectRoster` all are), so the extraction *finale* (D-0010: "survive a
defend timer — a final wave") spawns nothing out of the box; the defend phase is just continued
fodder. Defaulting it (e.g. `if (DefendWaveEliteClass.Get() == nullptr) DefendWaveEliteClass =
ACarapace;`) realizes the design and matches the "works without editor wiring" pattern used
everywhere else in the file.

**Why this is gated:** it changes gameplay defaults (which archetype, how many) — a tuning call.
Recommend defaulting to a tanky/telegraphed anchor (Carapace) at the existing `DefendWaveCount`,
but leave for the user to confirm/redirect. Until then, Task 2 sets the class locally so the
spawner is still proven.

---

## Commit plan
- Task 1 → one docs/comment commit (no gameplay change; safe without SmokeTest).
- Task 2 → one commit after SmokeTest is green.
- Task 3 → separate commit only after the user confirms the default.

**Standing exclusions** (never stage): `GE_Upgrade_ChainDetonation.uasset`, `ABP_Hero.uasset`,
`Test_1.umap`, `SUPERPOWERS_HANDOFF.md`. `F:\UEAS` read-only.
