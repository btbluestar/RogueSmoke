# Upgrade Loop v3 — Behavior Evolutions, Hero Ability Tracks, Wave Director

**Date:** 2026-06-11 · **Status:** Approved by user (design conversation, this date)
**Builds on:** D-0018 (team XP / pick loop), D-0019 (per-player hands, stacks, prereqs, chest synergy picks) and the v2 spec `2026-06-11-upgrade-loop-v2-design.md`.

## Goal

Three additions, in implementation order:

1. **Behavior evolutions** — chest-tier cards that upgrade the four existing synergy cards from
   stat combos into new combat *behaviors* (chain spreads burn, poison death-burst, etc.).
2. **Hero ability tracks** — Vanguard (taunt) and Bombardier (barrage) card tracks:
   stat cards → milestone → behavior evolution, hero-gated so cards only appear for the right hero.
3. **Wave director** — `RaidObjective` fodder-wave pressure scales with team level and squad
   size: wave size, tempo, deterministic elite injections, player-count scaling.

## Architecture decision (the fork)

**Hybrid execution.** Rejected: a generic on-hit event broadcast to AngelScript (fires per
bullet × pierce × chain = scripting the hot path, against `Rogue_Smoke_MVP_Architecture.md`).

- **Hit-path behaviors compile.** New branches inside the existing C++
  `UCombatSubsystem::ProcOnHitEffects` / chain resolve, switched by new `URogueCombatSet`
  attributes carried in `FWeaponShotParams` (exactly how pierce/chain/burn already work).
- **Death-path behaviors script.** Low-frequency; run in AngelScript inside
  `ARaidGameMode.HandleEnemyKilled` (already bound to `USpawnDirector::OnEnemyKilled`,
  which fires **before** pool recycle, so victim state is readable).
- **Ability behaviors script.** Vortex/salvo/carpet live in `GA_Taunt` / `GA_Barrage`
  (AngelScript, hot-reloadable), gated by flag attributes.

One C++ pass early (attributes + three small seam additions), then everything else is
script + content.

## New C++ surface (one pass, Task 1 of the plan)

### `URogueCombatSet` — 11 new attributes (same pattern as existing, infinite ADD_BASE GEs)

| Attribute | Used by | Meaning |
|---|---|---|
| `ChainIgniteFraction` | Searing Arcs | >0: chain arc targets get Burn, DPS = ArcDamage × fraction / BurnDuration |
| `ClusterChainBonusArcs` | Overwhelm evo | Extra chain arcs when the shot's victim `IsClustered()` |
| `PoisonBurstDps` | Toxic Burst | >0: poisoned victims burst on death — poison DoT at this DPS in a sphere |
| `ClusterKillShieldAmount` | Iron Bulwark | Flat Shield granted to the whole squad per Clustered kill |
| `TauntRadiusBonus` | Magnetic Pull | Added to `GA_Taunt.Radius` (currently hardcoded 800) |
| `TauntClusterDurationBonus` | Iron Grip | Added to `GA_Taunt.ClusterDuration` (currently hardcoded 3.0) |
| `TauntDamage` | Concussive Taunt | >0: taunt also deals `TauntDamage × (1 + AbilityPower)` radial damage |
| `TauntVortex` | Event Horizon | Flag (≥1): taunt becomes a re-pulling vortex |
| `BarrageDamageBonus` | High Explosives | Multiplicative: `BaseDamage × (1 + bonus)` (BaseDamage currently hardcoded 40) |
| `BarrageSalvoCount` | Twin Salvo | Extra barrage strikes (default 0) |
| `BarrageCarpet` | Carpet Bombing | Flag (≥1): salvo becomes a marching strip of blasts |

### `FWeaponShotParams` — 2 new fields

`ChainIgniteFraction`, `ClusterChainBonusArcs` — populated by `GA_WeaponFire` from the
shooter's attributes (per-player ownership falls out naturally: your bullets, your evolutions).

### `UCombatSubsystem` / `UHealthComponent` — three additions

1. `ProcOnHitEffects`: chain resolve applies Burn to arc targets when `ChainIgniteFraction > 0`
   (arc damage × fraction, standard burn duration); arc count becomes
   `ChainCount + (Victim->IsClustered() ? ClusterChainBonusArcs : 0)`. Chain targets still never
   re-chain or re-proc (bounded, unchanged).
2. `UHealthComponent::HasActiveDot(ERogueDotType) const` — UFUNCTION query so script can ask
   "was this enemy poisoned?" at death time.
3. `UCombatSubsystem::ApplyDotInSphere(Center, Radius, DotType, Dps, Duration)` and
   `UCombatSubsystem::GrantShieldToSquad(Amount)` — server-only seam methods. Shield grant
   clamps to `MaxShield` in C++ (authority + clamping stay compiled).

## A. Behavior evolutions (4 cards — synergy class, chest-only)

All: `MaxStacks = 1`, `bApplyToSquad = true`, `PrereqA =` the base synergy card (squad scope,
same duo/solo rules as D-0019). Offered only in chest (synergy-only) picks.

| Card | Requires | Behavior | GE |
|---|---|---|---|
| **Searing Arcs** | Wildfire | Chain arcs ignite their targets | `ChainIgniteFraction +0.5` |
| **Toxic Burst** | Venom Cascade | Enemies dying while Poisoned release a poison cloud (radius 350, DoT only — no instant damage, so cascades are time-gated by dot ticks; no recursion guard needed) | `PoisonBurstDps +6` |
| **Overwhelm evo** | Overwhelm | Hits on Clustered enemies arc to extra targets | `ClusterChainBonusArcs +2` |
| **Iron Bulwark** | Iron Vanguard | Kills on Clustered enemies shield the squad | `ClusterKillShieldAmount +3` |

Death-path handling (Toxic Burst, Iron Bulwark) extends `ARaidGameMode.HandleEnemyKilled`:
read `HasActiveDot(Poison)` / `IsClustered()` on the victim, read the squad-wide attribute from
the first valid hero's ASC (every hero has the GE — `bApplyToSquad`), call the seam.
Late-join GE reconciliation is **out of scope** (no late join in MVP).

## B. Hero ability tracks (7 cards — personal, hero-gated)

New field `URogueUpgradeDef.RequiredHeroClass` (`TSubclassOf<APawn>`, null = any hero).
`ARaidGameMode.IsEligible` additionally requires the player's pawn to be of that class.
Hands short on eligible cards pad from `UtilityPool` exactly as today.

**Vanguard / Taunt** (track: stats → milestone → evolution, all `bPrereqSelf` personal prereqs):

| Card | Type | Effect | Stacks / prereq |
|---|---|---|---|
| **Magnetic Pull** | stat | `TauntRadiusBonus +150` | ×3 |
| **Iron Grip** | stat | `TauntClusterDurationBonus +1.0` | ×3 |
| **Concussive Taunt** | milestone | `TauntDamage +30` (taunt now hits) | ×2, requires Magnetic Pull ×3 |
| **Event Horizon** | evolution | `TauntVortex` flag — taunt becomes a 3 s vortex: every 0.5 s re-`PullEnemiesToward` (60 % strength) + refresh `MarkClustered` | ×1, requires Concussive Taunt ×1 **and** Iron Grip ×2 |

**Bombardier / Barrage:**

| Card | Type | Effect | Stacks / prereq |
|---|---|---|---|
| **High Explosives** | stat | `BarrageDamageBonus +0.25` | ×3 |
| **Twin Salvo** | milestone | `BarrageSalvoCount +1` — second strike 0.4 s later at 60 % damage, same center | ×1, requires High Explosives ×3 |
| **Carpet Bombing** | evolution | `BarrageCarpet` flag — strikes become a strip: 5 blasts marching along the caster's facing, 300 uu apart, 0.25 s cadence, 60 % damage, 60 % radius, each preceded by a 0.4 s `ShowTelegraphZone` ring | ×1, requires Twin Salvo ×1 **and** Wide Barrage ×2 |

Ability script changes (`GA_Taunt` / `GA_Barrage`, pure AngelScript): read the new attributes
via the existing `GetCombatAttribute()`; vortex/salvo/carpet are server-side timers inside the
ability instance (`ServerOnly`, `InstancedPerActor` — ability ends after the last tick).
Concussive Taunt reuses `ApplyRadialDamage` with cluster multiplier 1.0 (no double-dip with
Barrage's cluster bonus).

Pool grows ~24 → ~35 cards.

## C. Wave director (pure AngelScript, `RaidObjective.as`)

Extract wave math into a **pure function** `ComputeWavePlan(TeamLevel, WaveIndex, NumPlayers)`
returning `FWavePlan { FodderCount, Interval, EliteClassIndex /* -1 = none */ }` — testable
headlessly without spawning anything. The tick consumes the plan. All knobs
`UPROPERTY(EditDefaultsOnly)` on `ARaidObjective`:

| Knob | Default | Behavior |
|---|---|---|
| `FodderPerTeamLevel` | 0.8 | Wave size += `floor(TeamLevel × 0.8)` (on top of existing per-wave-index escalation) |
| `WaveIntervalReductionPerLevel` | 0.35 s | Interval = `FodderWaveInterval − Level × 0.35` |
| `MinWaveInterval` | 3.5 s | Tempo floor |
| `EliteInjectStartLevel` | 4 | Below this, never inject |
| Injection cadence | every 3rd wave (level < 8), every 2nd (level ≥ 8) | Deterministic: keyed to `WaveIndex` only — rotation `[Spitter, Lunger]` indexed by `WaveIndex`, **no RNG** |
| `PlayerCountWaveScale` | 0.5 | Wave size × `(1 + 0.5 × (NumPlayers − 1))` |
| `FodderMaxPerWave` | 20 → **32** | Final clamp after all scaling |
| `MaxConcurrentPerExtraPlayer` | 5 | Soft concurrent cap += per extra player |

`TeamLevel` read from `ARaidGameState` (replicated; the objective ticks server-side).
Determinism: the plan is a pure function of (level, wave index, player count) — no seeded
stream even needed; spawn *positions* keep using the existing master-seed ring placement.

## Verification

1. `mcp__ue-cpp__build` green after the C++ pass; `run_code_test` 0 errors after each script task.
2. **New `EvoSmoke` exec** (DL_Upgrades, retry-poll pattern like `UpgradeSmoke`), ≥7 checks:
   ignite-on-arc, bonus arcs vs Clustered, poison death-burst, squad shield on Clustered kill,
   vortex re-pull/refresh beyond base duration, twin-salvo double hit, carpet multi-blast.
   Prints `[EvoSmoke] RESULT n/7`.
3. **New `DirectorReport` exec**: table of `ComputeWavePlan` at levels 1–12 × 1–2 players;
   asserts monotonic non-decreasing size, interval ≥ floor, injection cadence, clamp at 32,
   and same-inputs-same-plan determinism. Prints `[DirectorSmoke] RESULT n/n`.
4. `UpgradeFlowSmoke` extended: hero-gating check (Taunt card ineligible for a Bombardier pawn).
5. `Tools\SmokeTest.ps1` expectations extended; full 8-level gate must stay green.
6. 2-player feel pass: manual PIE checklist (appended to the existing one).

## Out of scope

Niagara/polish FX (debug draw + telegraph rings only), Mass migration (fodder remain pooled
actors), late-join GE reconciliation, balance beyond first-pass numbers, taunt slow/CC effects.

## Risks / verify-in-plan

- **Order of `OnEnemyKilled` vs DoT reset**: research says the delegate fires before pool
  recycle (`ResetHealth` clears dots). The plan must confirm with a read of
  `SpawnDirector.cpp::HandleEliteDeath`; if reset happens first, snapshot dot state into the
  broadcast or reorder.
- **Dual self-prereq** (`PrereqA` + `PrereqB`, both `bPrereqSelf`): confirm `IsEligible`
  evaluates both in self scope (D-0019 code reads both; verify before relying on it for
  Event Horizon / Carpet Bombing).
- **11 new attributes** = one editor-down C++ rebuild; serialize on the main session.
- **`GetPlayerState().GetPawn()` class check timing**: eligibility is evaluated while the raid
  is paused mid-run, pawn is alive — but guard null pawn (dead/respawning player) by treating
  hero-gated cards as ineligible.
