# MVP Progress — taunt→barrage slice (D-0008)

Tracks the bring-up order from `SETUP.md` §5 / `Rogue_Smoke_MVP_Architecture.md` §9.
Status legend: ✅ done · 🟡 scaffolded (unverified in-editor) · ⛔ blocked · ⬜ not started.

| # | Step (SETUP §5) | Status | Notes |
|---|-----------------|--------|-------|
| 0 | Build UE 5.7 + AngelScript fork from source | ⬜ | Confirm stable 5.7 branch (D-0001). Prereq for *all* verification below. |
| 1 | Hot-reload smoke test | 🟡 | `Script/SmokeTestActor.as` written. **Run this first**; don't proceed until hot-reload works. |
| 2 | Verify API signatures vs running compiler | ⬜ | `Math::`, `::Get` accessors, `NewObject`/`TSubclassOf` forms used in script are *patterns*, not verified. |
| 3 | Stand up `UCombatSubsystem` (C++) stub | 🟡 | `Source/RogueSmoke/Combat/` + `Enemies/EliteEnemyBase`. Actor-registry backend. Needs a C++ compile. |
| 4 | First combo on Actors only | 🟡 | Abilities + `Vanguard`/`Bombardier` + `ClusterableElite` written. Needs BP subclasses + input wiring in-editor. |
| 5 | Spike Mass fodder | ⬜ | Deferred — version-sensitive. **Spawn seam is ready:** `Spawning/SpawnDirector` has a working pooled-Actor backend for elites; Mass fodder plugs into `SpawnFodderWave()` (currently a logged placeholder) without changing callers. |
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
