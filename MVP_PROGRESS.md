# MVP Progress — taunt→barrage slice (D-0008)

Tracks the bring-up order from `SETUP.md` §5 / `Rogue_Smoke_MVP_Architecture.md` §9.
Status legend: ✅ done · 🟡 scaffolded (unverified in-editor) · ⛔ blocked · ⬜ not started.

| # | Step (SETUP §5) | Status | Notes |
|---|-----------------|--------|-------|
| 0 | Build UE 5.7 + AngelScript fork from source | ⬜ | Confirm stable 5.7 branch (D-0001). Prereq for *all* verification below. |
| 1 | Hot-reload smoke test | 🟡 | `Script/SmokeTestActor.as` written. **Run this first**; don't proceed until hot-reload works. |
| 2 | Verify API signatures vs running compiler | ⬜ | `Math::`, `::Get` accessors, `NewObject`/`TSubclassOf` forms used in script are *patterns*, not verified. |
| 3 | Stand up `UCombatSubsystem` (C++) stub | ✅ | `Source/RogueSmoke/Combat/` + `Enemies/EliteEnemyBase`. Builds clean; verified in PIE — `GetEliteCount`/`CountEnemiesInSphere` see registered elites. |
| 4 | First combo on Actors only | ✅ | **Verified in PIE (2026-06-09):** `MarkClustered`→`IsClustered` gives the cluster bonus (unclustered 40 → clustered 120 = 3×); damage lands on replicated health (100→60). Pull now *visibly* converges enemies after the `bSweep` fix (was a no-op — actors at Z=0 froze on a swept move). Ability-activation-via-input path still to exercise with real input. |
| 5 | Fodder swarm | ✅ (interim) | **Cheap-Actor fodder shipped** (`Enemies/FodderEnemy`, D-0003 fast path): subclasses `AEliteEnemyBase` so it reuses the seam + pool unchanged; steers at the nearest player; excluded from `GetEliteCount` but counted by `CountEnemiesInSphere`. Verified in PIE: 12 spawn + approach + die to barrage (20 HP) + recycle. **Mass backend** is the eventual replacement behind the same `SpawnFodderWave()` seam (version-sensitive, deferred). |
| 6 | Upgrade + select widget | 🟡 | `Upgrades/` + `UI/UpgradeSelectWidget.as` written. Still needs the BP widget asset + an offer flow. |
| 7 | Raid objective + extraction, 2-player test | 🟡 | `Objective/RaidObjective.as` written (single raid D-0009 + defend-timer extraction D-0010). Defend-wave now spawns via `SpawnDirector` (set `DefendWaveEliteClass`). Party-wipe still a hook pending down/revive. 2-player test needs the editor. |

## To verify the slice in-editor (do in order)

1. **Smoke test** — place `ASmokeTestActor` in a level, Play, confirm the print; edit `Message`, save, confirm live hot-reload.
2. **Compile C++** — build the editor target so `UCombatSubsystem`/`UHealthComponent`/`AEliteEnemyBase` exist; confirm `UCombatSubsystem::Get(this)` resolves from script.
3. **Make content** — `BP_ClusterableElite` (assign a mesh), `BP_Vanguard`/`BP_Bombardier`, an `IA_PrimaryAbility` Enhanced Input action bound to `OnPrimaryAbilityPressed`.
4. **Run the combo** — place several elites, Vanguard taunts (they converge + a cue), Bombardier barrages (clustered enemies take the bonus). Tune feel.
5. **Upgrade** — offer `UUpgrade_ChainDetonation` via a BP `UpgradeSelectWidget`; confirm the barrage window widens.

## What unblocks the rest
- **Step 5 (Mass)** and the BP assets need the **built engine** — there's no substitute for the editor here.
- **Step 7 (objective/extraction)** needs the **open design decisions resolved** first (run = single raid vs chained? extraction trigger/risk/failure cost?).
