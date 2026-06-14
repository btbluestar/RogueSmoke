# Meta-Upgrade Variables

> A catalogue of the **exposed, tunable variables** meta-progression (and in-run upgrades) can
> modify. Meta-progression scope is still deferred (GDD §6.3, see [DesignThreads](DesignThreads.md)
> DT-4) — this doc is the *menu of knobs* so when we wire upgrades we already know what's available
> and where it lives.
>
> Two kinds of knob:
> - **Component UPROPERTY** — a tunable on an AngelScript component (e.g. `URogueLocomotionComponent`).
>   Editable in the BP defaults and live via the **`MoveTune`** console exec. An upgrade changes it by
>   writing the property (movement upgrades call `ApplyMovementConfig()` after).
> - **GAS attribute** — lives on a C++ AttributeSet on the PlayerState ASC; an upgrade is a
>   `GameplayEffect` that modifies it (the project's intended upgrade path, D-0013/D-0018).
>
> Defaults below are current as of 2026-06-13; verify against source before relying on a number.
> Exact GAS attribute names should be confirmed in the C++ AttributeSets before authoring effects.

---

## Movement — `URogueLocomotionComponent` (`Script/Player/LocomotionComponent.as`)

| Variable | Default | What it does | Meta-upgrade idea |
|----------|---------|--------------|-------------------|
| `MaxAcceleration` | 6144 | Ground input responsiveness | — (feel) |
| `GroundFriction` | 10 | Ground grip | — |
| `GravityScale` | 1.8 | Fall heaviness | — |
| `JumpZVelocity` | 750 | Jump impulse | **Higher jump** |
| `DoubleJumpZVelocity` | 700 | 2nd-jump impulse | — |
| `MaxJumpCount` | 2 | Air jumps allowed | **Unlock double/triple jump** (DT-4) |
| `AirControl` | 0.9 | Mid-air steering | **Better air control** |
| `SprintSpeedMultiplier` | 1.6 | Sprint speed (× base) | **Faster sprint** |
| `CrouchSpeed` | 300 | Crouch-walk speed | — |
| **`SlideBoostMultiplier`** | 1.3 | Slide entry boost (× sprint) | **Stronger slide launch** |
| **`SlideSpeedCapMultiplier`** | 1.5 | Slide hard cap (× sprint) | **Higher slide cap** |
| `SlideBoostArmMultiplier` | 1.1 | Anti-bhop arming threshold | — |
| `SlideEntryMinFraction` | 0.85 | Min speed to enter a slide | — |
| **`SlideExitSpeedFraction`** | 0.5 | Slide ends below base×this | **Longer slides** (lower it) |
| **`SlideGroundFriction`** | 0.5 | Slide decay rate (also routed to BrakingFriction) | **Slicker slides** (lower it) |
| `SlideBraking` | 100 | Constant slide decel | — |
| **`SlideSteerAccel`** | 1000 | Input thrust allowed *during* a slide — steering vs. pure momentum | **Slide Steering** (user-requested; 0 = none, higher = more turn, but trades away the flat-ground degrade) |
| `SlideDownhillAccel` | 2048 | Downhill slide acceleration | — |
| **`SlideMaxDuration`** | 1.5 | Max slide length (s) on flat | **Longer slides** |
| `SlideSustainMinSlope` | 0.1 | Slope that refreshes slide duration | — |
| `bRequireSprintToSlide` | true | Must be sprinting to slide | — |

> All of the above are live-tunable in PIE with **`MoveTune <Param> <Value>`**, and `MoveTune dump`
> prints the block to bake into defaults. `MoveSmoke` asserts the slide invariants (boost/cap/anti-bhop).

## Stamina / pips (D-0023) — currently **dormant** (see [DesignThreads](DesignThreads.md) DT-1)

| Variable | Lives on | Notes |
|----------|----------|-------|
| `StaminaRegenSeconds` | `AHeroCharacter` (`HeroCharacter.as`) | Seconds per pip regen |
| `StaminaRegenDelay` | `AHeroCharacter` | Pause after a spend |
| `Stamina` / `MaxStamina` | `URogueMovementSet` (GAS) | Pip pool. Nothing consumes pips right now; reserved for dash + air-jump (DT-1/DT-2/DT-4) |
| `MoveSpeed` | `URogueMovementSet` (GAS) | Base walk speed; the "Swift" upgrade modifies this |

## Combat / weapon — `URogueCombatSet` (GAS attributes)

> The weapon-upgrade surface (see GLOSSARY "Weapon upgrade"). Confirm exact attribute names in the
> C++ AttributeSet before authoring GameplayEffects.

| Attribute (approx.) | What it does | Track |
|---------------------|--------------|-------|
| `WeaponDamageBonus` | Bonus gun damage | Damage |
| `FireRateBonus` | Faster fire rate | Fire rate |
| `Pierce` | Bullets pass through N extra enemies | Piercing Rounds → milestone |
| `Chain` | Hits arc to nearest enemies (rewards Density) | Chain Lightning → Chain Detonation |
| Burn magnitude/chance | On-hit fast DoT | Burn |
| Poison magnitude/chance | On-hit slow DoT | Poison |
| `AbilityPower` | Scales ability (taunt/barrage) effect | Ability track |

## Health / defense — `UHealthComponent` / health AttributeSet

| Attribute (approx.) | What it does |
|---------------------|--------------|
| `MaxHealth` | Health pool |
| `Health` | Current HP (not an upgrade target directly) |
| `Shield` | Regenerating overshield, if present |

---

## How an upgrade applies a change

- **Movement knob:** set the `URogueLocomotionComponent` property, then call `ApplyMovementConfig()`
  so it pushes into the CMC. (This is what a movement meta-upgrade would do; none are wired yet — D-0015.)
- **GAS attribute:** author a `GameplayEffect` that modifies the attribute (INSTANT for permanent, or
  Infinite for a held buff). This is the standard upgrade path (D-0018 team-XP picks, synergy cards).

See also: [DesignThreads.md](DesignThreads.md) (open design questions), `DECISIONS.md` (D-0013 GAS,
D-0015 movement, D-0018 upgrades, D-0023 stamina), `GLOSSARY.md` (upgrade vocabulary).
