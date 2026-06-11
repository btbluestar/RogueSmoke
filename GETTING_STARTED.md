# Getting Started — build & play the MVP slice

> Hands-on guide: from a fresh clone to playing the **taunt→barrage** vertical slice
> (D-0008). For the one-time engine/toolchain install, see **`SETUP.md`** — this guide
> assumes that's done and focuses on *this* project's code.
>
> ⚠️ **Historical (D-0008 era) — the bring-up below is complete.** Several systems it
> references were replaced since: abilities run on **GAS** (D-0013), the camera is
> third-person shooter (D-0014), UI is **CommonUI** (D-0016), the enemy roster is the
> bio-horde (D-0017), and the upgrade flow is the level/chest loop (D-0018–20);
> `UStatsComponent` no longer exists (stats are GAS attributes) and `WBP_UpgradeSelect` needs no
> layout. **To play the game today:** open `/Game/Levels/RaidArena` and press Play — see
> `HANDOFF_gameplay_loop.md` for the walkthrough. AI agents: read **`startup.md`** instead.
> The fresh-clone/toolchain steps (§0–§2) are still accurate.

---

## 0. Prerequisites

- ✅ A **source build** of UE 5.7 + the Hazelight AngelScript fork (see `SETUP.md` §1–2).
- ✅ Visual Studio with "Game development with C++".
- ✅ VS Code + the **Unreal Angelscript** extension (`SETUP.md` §3).
- ✅ Git **and Git LFS** — this repo stores `.uasset`/`.umap` in LFS:
  ```
  git lfs install
  git clone <repo-url>
  cd RogueSmoke
  git lfs pull
  ```

---

## 1. Compile the C++ module

The combat/spawn seams are C++ and must compile before script can call them.

1. Right-click `RogueSmoke/RogueSmoke.uproject` → **Generate Visual Studio project files**.
2. Open `RogueSmoke.sln`, set configuration to **Development Editor / Win64**.
3. **Build** the `RogueSmoke` target. This compiles:
   - `Combat/CombatSubsystem`, `Combat/HealthComponent`
   - `Enemies/EliteEnemyBase`
   - `Spawning/SpawnDirector`
4. A clean build = the seams are sound. If it fails, fix C++ before touching script
   (script can't bind to a module that won't compile).

> The two subsystems (`UCombatSubsystem`, `USpawnDirector`) are **WorldSubsystems** —
> they auto-create per world. There's nothing to place for them to exist.

---

## 2. Smoke-test AngelScript (do NOT skip)

This proves the script pipeline + hot-reload before any gameplay (SETUP §5.1).

1. Launch the editor via your **custom** build, open `RogueSmoke.uproject`.
2. Open any level. In the Place Actors panel, search **`SmokeTestActor`**, drag one in.
3. **Play (PIE)** → you should see the on-screen print.
4. With PIE running, edit `Script/SmokeTestActor.as` → change `Message` → **save**.
   The print should update **live**, no restart.

❌ If hot-reload doesn't work, **stop and fix the toolchain** — nothing below will either.

---

## 3. Make the content (Blueprints over the script classes)

Script holds logic; Blueprints assign assets and let you place/tune (CODING_STANDARDS §6).
Create these under `Content/RogueSmoke/` (right-click → Blueprint Class → pick the script parent):

| Blueprint | Parent (script) | Set up |
|-----------|-----------------|--------|
| `BP_ClusterableElite` | `AClusterableElite` | Assign a **Static Mesh** to `Mesh` (a cube is fine). Set `Health → MaxHealth`. |
| `BP_Vanguard` | `AVanguard` | Assign a skeletal mesh + anim BP to the inherited `Mesh`. Tune `Taunt` (Radius/PullStrength/ClusterDuration) and `Stats` (MaxHealth/Armor/MoveSpeed/…). |
| `BP_Bombardier` | `ABombardier` | Same mesh setup. Tune `Barrage` (Radius/BaseDamage/ClusterBonusMultiplier) and `Stats`. |
| `BP_RaidObjective` | `ARaidObjective` | Set **`DefendWaveEliteClass = BP_ClusterableElite`**, `DefendWaveCount`, `DefendWaveRadius`, `ExtractionDefendSeconds`. |
| `WBP_UpgradeSelect` | `UUpgradeSelectWidget` | Add buttons; each button calls `ChooseUpgrade(index)`. Fill `OfferedUpgrades` (e.g. `UUpgrade_ChainDetonation`). |

> You *can* place raw script actors directly, but BPs are where you attach meshes/materials
> and tweak defaults without recompiling.

### Input wiring (Enhanced Input — on the PlayerController)

Input is wired in script on **`ARaidPlayerController`** (one place for every hero), not per
pawn. Requires the `AngelscriptEnhancedInput` plugin (already enabled in the `.uproject`).
No node graphs — just asset assignment:

1. Create `IA_PrimaryAbility` (Input Action, Digital/bool) and add it to `IMC_Default` with a
   key (e.g. Left Mouse Button). `IA_Move` is already in `IMC_Default` from the template.
2. Create **`IA_Look`** (Input Action, **Axis2D**) and add it to `IMC_Default` for mouse XY and the
   right stick. Third-person shooter camera (D-0014): this drives the control rotation the boom
   follows. Add a **Negate (Y)** modifier on the mouse mapping if you want standard (non-inverted)
   pitch — sign is handled in the IMC, not in script.
3. Make **`BP_RaidPlayerController`** (parent class = `RaidPlayerController`). In Class
   Defaults → Input, assign: **Input Mapping Context** = `IMC_Default`, **Move Action** =
   `IA_Move`, **Look Action** = `IA_Look`, and the **Input Config** (`DA_Raid_InputConfig`,
   which maps `IA_PrimaryAbility` → `InputTag.Ability.Primary`).
4. In **`BP_RaidGameMode`** → Class Defaults, set **Player Controller Class** =
   `BP_RaidPlayerController`.

That's it — the controller adds the mapping context, binds the actions, forwards `DoMove` / look to
the possessed hero, and routes ability inputs by gameplay tag. No input wiring in the hero BPs.

---

## 4. Build a test arena

1. New basic level with a floor.
2. Place a **GameMode** (script or BP) — or set it in World Settings — with
   `DefaultPawnClass = BP_Vanguard`. (For two kits, see §6.)
3. Drag in **several `BP_ClusterableElite`** in a loose cluster.
4. Drag in **one `BP_RaidObjective`**.
5. Add a PlayerStart.

---

## 5. Play the slice (solo first)

With `DefaultPawnClass = BP_Vanguard`:

1. **Play.** Press the ability key → **Taunt**: nearby elites get pulled toward you and
   marked Clustered; you'll see the `TAUNT:` print.
2. Switch `DefaultPawnClass` to `BP_Bombardier`, Play again → **Barrage**: the `BARRAGE hit N`
   print shows damage; clustered elites take the bonus multiple.
3. **Clear all elites** → `OBJECTIVE COMPLETE - call extraction` print (the objective polls
   `USpawnDirector`/`UCombatSubsystem` for the live elite count).
4. **Trigger extraction.** Quick test: from the Level BP on a key press, call your objective's
   `CallExtraction()` (host/authority). The defend wave spawns via `SpawnDirector`; survive
   `ExtractionDefendSeconds` → `EXTRACTED - raid won!`.

> Seeing the **taunt→barrage** combo feel good on Actor elites is the success criterion
> for the slice (D-0008). Tune the numbers in the BPs until it does, *then* expand.

### Try the upgrade
Show `WBP_UpgradeSelect` (Create Widget + Add to Viewport from the Level/hero BP), click the
Chain Detonation button → it routes through `Hero.Server_ApplyUpgrade` → the barrage's radius
and cluster bonus grow. Re-run the combo to feel the difference.

---

## 6. Two-player test (host-authoritative)

1. **Play** dropdown → set **Number of Players = 2**, Net Mode = **Play As Listen Server**.
2. To give each player a different kit (Vanguard vs Bombardier), assign pawns per controller
   in your GameMode (e.g. override pawn selection by player index) rather than a single
   `DefaultPawnClass`.
3. Confirm the host-authority model (CODING_STANDARDS §4): one client taunts, the other
   barrages; damage/extraction resolve on the server, cosmetics play on both.

> Note: abilities and stats now run on **GAS** (DECISIONS.md D-0013), not the old
> `UAbilityComponent`/`UStatsComponent`. The AbilitySystemComponent lives on `ARoguePlayerState`
> and replicates; teammate HUDs read replicated attributes (`URogueHealthSet` Health/Shield).
> Heroes are granted a `URogueAbilitySet` on possession; input activates abilities by gameplay tag.

---

## 7. Troubleshooting

| Symptom | Likely cause |
|---------|--------------|
| Script types missing in editor | C++ module didn't compile (step 1) — rebuild. |
| `UCombatSubsystem::Get` returns null | Called with a bad WorldContext, or before the world exists. |
| Taunt/Barrage do nothing | No elites registered — elites register on `BeginPlay` (placed) or via `SpawnDirector`. Confirm they're `AEliteEnemyBase` subclasses. |
| Ability fires on client but no effect | Effects are server-only by design; check you're testing on the listen server / authority. |
| Compiler rejects a script signature | Expected — `Math::`, `::Get`, `NewObject`, `TSubclassOf`, `HasAuthority()` are fork-version-sensitive. Use **Add Import To** / fix to the editor's reported form; paste the error and we'll correct the source. |
| Objective never completes | The "cleared" check needs elites to have existed first (`StartGraceSeconds` + a non-zero count), then drop to 0. |

---

## Fork API conventions (confirmed against the engine)

Things that differ from stock C++/Blueprint assumptions, learned from real compiler passes:

- **Subsystems:** access world subsystems via the auto-generated **`UMySubsystem::Get()`**
  (no args). Don't hand-write a `Get(WorldContext)` — it collides with the generated one.
  In C++, use `GetWorld()->GetSubsystem<>()`.
  ([docs](https://angelscript.hazelight.se/scripting/subsystems/))
- **Components:** access via **`UMyComponent::Get(actor)`** (takes the owning actor).
- **Ticking:** there is **no settable `default PrimaryActorTick.bCanEverTick`**. Overriding
  the `Tick` event (`UFUNCTION(BlueprintOverride) void Tick(...)`) is what makes a class tick,
  like Blueprint's Event Tick.
- **RPCs default to RELIABLE** (opposite of C++); mark cosmetic ones `Unreliable`
  (CODING_STANDARDS §4.1).
- **Authority:** on a listen server the **host has authority**, so server-only logic and
  `Server_` RPCs from the host execute locally — solo/host testing works before replication
  is fully set up.

## What's NOT wired yet (by design)

- **Hero damage intake** — heroes have a full `UStatsComponent`, but nothing damages them
  yet (enemies have no attack). Route enemy attacks to `Stats.ApplyDamage(...)` when added.
- **Mass fodder** — `SpawnDirector::SpawnFodderWave` is a logged placeholder until the Mass
  spike (SETUP §5.5, needs a concrete enemy-count target — DECISIONS "Still open").
- **Down/revive** → party-wipe is a hook (`RaidObjective::NotifyPartyWiped`).
- **Spawn staggering** — waves spawn in one call; fine at low counts, revisit at scale.

See `MVP_PROGRESS.md` for the live status of every step.
