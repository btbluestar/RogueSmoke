# Weapon Upgrades Plan — pierce / chain / fire rate / elemental (2026-06-10)

> ✅ **Executed.** The weapon track shipped: pierce/chain/burn/poison live as `URogueCombatSet`
> attributes proc'd in `UCombatSubsystem` (see GLOSSARY "Weapon upgrade"); D-0019/20 then built
> stacks, milestones, and behavior evolutions on top. Historical plan — kept for the genre
> research; don't implement from it.

> User ask: "more weapon focused upgrades, like piercing, chain shot, fire rate etc. maybe even
> effects like fire/poison." Research how other games do gun upgrades, plan, then execute
> (including authoring the GEs).

## 1. Research — how the genre does gun upgrades

| Game | System | Takeaways for us |
|---|---|---|
| **Gunfire Reborn** | *Inscriptions* (random weapon modifiers) + a 3-element system: Fire→Burning (DoT, % of hit damage per second), Lightning→Shock (+damage taken), Corrosion (anti-armor). Elemental *chance* per hit; fusion effects reward combining. | Status = **% chance per hit, magnitude derived from the hit's damage** — scales with every other upgrade automatically, no separate DoT balance track. |
| **Risk of Rain 2** | Stackable on-hit items: Ukulele (**chain lightning to ~3 nearby for fraction damage**), Tri-tip (bleed DoT), Gasoline/Will-o'-wisp (on-kill AoE), Soldier's Syringe (+15% attack speed/stack). Uniform "proc" model on the hit event. | Chain = *find N nearest enemies within radius of the victim, deal fraction damage*. Everything stacks linearly per pick — repeat offers ARE the stacking mechanic. |
| **Deadzone: Rogue** (co-op FPS roguelite, closest genre match) | Perks in trees: Combat (fire rate, damage, reload), Elemental (Chain Lightning, burn, shock; *elemental affix converts weapon, perks make every hit apply the debuff*). | Category split: **ballistic stat boosts** (common) vs **behavior changers** (pierce/chain, rarer) vs **elemental procs** (rarest, build-defining). Maps cleanly to our rarity tiers. |
| **DRG: Rogue Core / Roboquest / Gunforged** | Flat rolled modifiers: +damage, +weakpoint, +reload speed, **Piercing**, **Ricochet**, +% damage as element. | Pierce expressed as "+N enemies penetrated". Reload/mag size are cheap, satisfying commons. |

**Genre consensus:** three tiers — (1) plain ballistic stats (damage/fire rate/mag/reload),
(2) bullet-behavior modifiers (pierce, chain/ricochet), (3) elemental on-hit procs (burn/poison)
whose damage derives from the hit so they scale with tier-1 picks. All stack on re-pick.

## 2. Fit into RogueSmoke's architecture

The decided upgrade pipeline (D-0013) is: **upgrade = GE modifying attributes on `URogueCombatSet`;
abilities read attributes; the seam executes combat.** Weapon upgrades follow it exactly — no new
pattern:

- **8 new attributes** on `URogueCombatSet` (C++, copy-one-attribute pattern the header invites):
  `WeaponDamageBonus`, `FireRateBonus`, `PierceCount`, `ChainCount`, `BurnChance`, `PoisonChance`,
  `MagazineBonus`, `ReloadSpeedBonus`. All default 0; bonuses are additive fractions
  (effective = base × (1 + bonus)), counts/chances are raw. Instant ADD_BASE GEs stack naturally
  when the same card is offered again (the existing Tier_1 GEs already follow this shape).
- **Seam, not ability logic** (house rule): pierce/chain/status execute inside `UCombatSubsystem`.
  New `FWeaponShotParams` struct + `FireWeaponShot(Start, End, Params, Instigator)`:
  - **Pierce:** iterative line trace; passes through up to `PierceCount` extra registered enemies
    (full damage each), stops at world geometry. Tracer endpoint = final stop.
  - **Chain:** on each enemy hit, arc to up to `ChainCount` nearest other registered enemies within
    `ChainRadius` (600uu) for `ChainDamageFraction` (50%) of the hit. Chain hits are plain damage
    (no recursive procs — bounded). Fodder is in the registry, so chains love swarms (the RoR2
    ukulele moment, and it feeds our density pillar).
  - **Status procs:** per enemy hit, roll `BurnChance` / `PoisonChance` (server-side `FRand`, same
    nondeterminism class as the existing spread `VRandCone` — combat is server-authoritative, not
    replayed, so the procgen determinism rule doesn't apply). Magnitudes derive from the hit
    (Gunfire Reborn model): **Burn = 50% of hit damage over 3s** (fast, anti-flesh), **Poison =
    100% of hit damage over 6s** (slow, bigger total).
  - `FireHitscan` keeps its signature (other callers untouched); it becomes a thin wrapper.
- **DoT lives on `UHealthComponent`** (C++ hot path — enemies are not GAS): `ApplyDot(Type, Dps,
  Duration, Instigator)`, one slot per type (reapply = refresh duration, keep strongest DPS — no
  unbounded stacking), component tick enabled only while a dot is active, damage funnels through
  `ApplyDamage` so **kill/damage stat credit and pool-safety come for free**; `ResetHealth()`
  clears dots (pooled-actor recycle).
- **Fire rate / reload / magazine** are weapon-state, not per-shot: `URogueWeaponComponent` gets
  the three bonus fractions pushed from the hero (which owns the ASC), applied at the existing
  choke points: `NotifyFired` (refire interval ÷ (1+FireRateBonus)), `StartReload` (time ÷
  (1+ReloadSpeedBonus)), reload-complete/equip (mag × (1+MagazineBonus)).
- **`GA_WeaponFire`** builds `FWeaponShotParams` from `GetCombatAttribute(...)` (helper already on
  `GA_RogueAbility`) — damage = `Def.Damage × (1 + WeaponDamageBonus)`. `AbilityPower` stays
  ability-only; weapon damage is its own track (distinct build axes, per Deadzone's category split).

## 3. The 8 new upgrade cards

| Card | Rarity | GE (instant, ADD_BASE) | Card value line |
|---|---|---|---|
| **Heavy Caliber** | 1 | WeaponDamageBonus +0.20 | +20% Weapon Damage |
| **Overclocked Receiver** | 1 | FireRateBonus +0.25 | +25% Fire Rate |
| **Extended Drum** | 1 | MagazineBonus +0.50 | +50% Magazine Size |
| **Greased Bolt** | 1 | ReloadSpeedBonus +0.30 | +30% Reload Speed |
| **Piercing Rounds** | 2 | PierceCount +1 | Shots pierce +1 enemy |
| **Arc Conductor** | 2 | ChainCount +2 | Hits arc to 2 nearby enemies (50% dmg) |
| **Incendiary Rounds** | 3 | BurnChance +0.25 | 25% chance to ignite (50% dmg over 3s) |
| **Neurotoxin Coating** | 3 | PoisonChance +0.25 | 25% chance to poison (100% dmg over 6s) |

Pool grows 8 → 16 (half kit/stat, half weapon — good offer variety). Repeat offers stack:
2× Piercing = pierce 2, 2× Incendiary = 50% burn chance, etc. GEs + DAs authored headlessly via
python commandlet (BlueprintFactory + the `ge-modifier-editing-via-python` import_text pattern),
appended to `BP_RaidGamemode.UpgradePool` (CDO edit → compile → save).

## 4. Execution order

1. **C++** (#59, #60): attributes → `FWeaponShotParams`/`FireWeaponShot` → HealthComponent DoT → build.
2. **AS** (#61): GA_WeaponFire params; WeaponComponent bonus plumbing; `WeaponSmoke` debug exec
   (applies the GEs to the host hero, fires a seam shot at the nearest elite, prints attribute
   values + pierce/chain/dot results — the headless proof).
3. **Content** (#62): python commandlet creates 8 GEs + 8 DAs, wires the pool.
4. **Verify + ship** (#63): `run_code_test`, headless `-game` boot in the combat debug level
   grepping WeaponSmoke breadcrumbs, smoke suite, commit, handoff.

## 5. Deliberately out of scope (noted follow-ups)

- **VFX/readability for procs** (burning/poisoned enemy tint, chain-arc beam, pierce tracer punch):
  lands with the GameplayCue pass (#39). MVP shows chain arcs as server debug lines.
- **Status replication to clients** (for future cue/UI): dots are server-side state today; the
  damage itself replicates via Health.
- **Crit, ricochet-off-walls, on-kill explosions, weapon-swap cards:** good future pool entries;
  same attribute+seam pattern, no new architecture.
- **Rarity-weighted offers:** RollOptions is uniform today; weighting is a balance-pass item (#42).
