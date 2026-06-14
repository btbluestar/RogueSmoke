# Feel Research Notes — Movement + Shooting (2026-06-11)

> Output of the spec's research phase (three parallel agents). Read alongside
> `docs/superpowers/specs/2026-06-11-movement-shooting-feel-design.md`.
> Confidence flags: **[S]** = well-sourced, **[I]** = inferred/convention.

---

## Report A — Deadlock + Apex movement → UE5 CMC translation

**Diagnosis:** the floatiness is almost entirely GravityScale 1.0 — real-world gravity
(980 cm/s²) with JumpZ 600 gives a 0.61 s rise, ~184 cm apex, ~1.2 s airtime. Both reference
games run much heavier effective gravity. Secondary: the slide (900) is SLOWER than sprint
(960) — backwards; in both reference games the slide is the fastest ground state.

### Deadlock (Source 2; cvar dump: github.com/Mikooboy/deadlock-cvar-list)

| cvar | Default | Meaning |
|---|---|---|
| `sv_gravity` | 800 u/s² | world gravity (1 hu = 2.54 cm here → 2032 cm/s² → **GravityScale ≈ 2.07**) |
| `sv_friction` | 4 | ground friction (decel ≈ v·friction/s) |
| `sv_accelerate` / `sv_airaccelerate` | 10 / 10 | reach wishspeed in ~0.1 s |
| `citadel_slide_delay` | 0.15 s | slide entry delay |
| `citadel_player_slide_min_percent` | 0.8 | slide minimum-speed fraction |

Gameplay (deadlock.wiki Movement/Jumping/Dashing/Stamina; dlmovement.wiki):
base run 8 m/s, sprint 10 m/s; slide entry needs ≥ ~8.9 m/s or downhill; ground jump peak
7.5 m/s (rise ≈ 0.37 s); **dash-jump SETS speed to 15.5 m/s (~1.55× sprint)** — a burst
entry that disregards prior momentum (that's what prevents compounding). Momentum loop:
dash-jump → hold crouch in air → land sliding → hop out; capped by stamina + slide decay.

### Apex Legends (Source 1 / Titanfall lineage; 1 hu = 1.905 cm)

- `sv_gravity` **750** u/s² — verified in Northstar's decompiled Titanfall 2 scripts
  (`_utility_shared.nut`) → 1429 cm/s² → **GravityScale ≈ 1.46**.
- Sprint 299 hu/s ≈ 5.7 m/s (apexmovement.tech). Slide entry grants **+150 hu/s instantly,
  hard-capped at 400 hu/s** (≈ sprint × 1.34).
- **Slide-jump boost only applies if velocity < 350 hu/s when you jump** — Apex's single
  anti-infinite-bhop governor. Above it: keep speed, no new boost.
- Air: airaccel 500 with wishspeed cap 60 (Quake strafe steering, no direct gain).
- The 2019 bhop-healing nerf raised friction contextually (while healing) — nerf via
  contextual friction, not by removing momentum.

### Proposed UE5 CMC starting values (lean-Deadlock)

| CMC property | Current | Proposed | Confidence |
|---|---|---|---|
| GravityScale | 1.0 | **1.8** | [S-derived] Deadlock 2.07 / Apex 1.46, lean Deadlock |
| JumpZVelocity | 600 | **750** | [S] Deadlock 295 u/s = 749 cm/s; rise 0.43 s at GS 1.8 |
| Double-jump Z (if separable) | 600 | **700** | [I] weaker second jump reads better |
| JumpMaxHoldTime | 0 | **0.0** | [S] both games fixed-impulse; variable height adds float (keep as MoveTune knob) |
| AirControl | 0.35 | **0.9** | [I] approximates Deadlock's direct air steering on stock CMC |
| MaxAcceleration | 2048 | **6144** | [S-derived] sv_accelerate 10 ≈ full speed in ~0.1 s, tempered |
| BrakingDecelerationWalking | 2048 | **3072** | [S-derived] sv_friction 4 at sprint speeds; stop ~0.3 s |
| GroundFriction | 8 | **10** | [I] snappier held-input direction changes |
| bUseSeparateBrakingFriction | off | **true**, BrakingFriction **6.0**, BrakingFrictionFactor **1.0** | [I] decouple stop-feel from turn-feel (PBCharacterMovement precedent) |
| FallingLateralFriction | 0 | **0.0** | [S] preserve air momentum fully |
| Slide boost speed | 900 | **~1250 (sprint × 1.3)** | [S] Apex ×1.34 cap; slide must beat sprint |
| Slide speed cap | — | **1450 (sprint × 1.5)** | [S-derived] Apex cap; Deadlock dash-jump 1.55× |
| Slide GroundFriction | 0.4 | **0.6** | [I] decay from frame 1 so re-hopping matters |
| Slide BrakingDeceleration | 200 | **256** | [I] flat slide lives ~1.2–1.5 s |
| Slide downhill accel | 2048 | keep | [I] |
| Slide exit threshold | 350 | **~0.9 × MaxWalkSpeed (≈540)** | [I] mirrors slide_min_percent 0.8 |

### Slide-hop rule set (stock CMC, script-side)

1. Slide entry requires sprint (or downhill) and speed ≥ ~85% sprint.
2. **Boost arming (anti-bhop governor, from Apex):** apply the slide boost ONLY if current
   2D speed < BoostThreshold (≈ sprint × 1.1). Above it: enter slide, keep velocity, no boost.
3. **Slide-jump preserves 100% of horizontal velocity**; FallingLateralFriction 0 keeps it in air.
4. **Landing with crouch held and speed ≥ slide threshold → re-enter slide immediately**
   (skip entry cooldown). Ground-contact friction frames are the natural per-hop decay tax.
5. **Hard cap:** clamp 2D speed to SlideCap while sliding or in post-slide-hop air.
6. Boost cooldown 1.5–2 s as belt-and-suspenders [I] (replaces stamina economy).
7. Future nerf lever: contextual friction (raise friction in special states), never core numbers.

**Tuning order:** GravityScale+JumpZ first → AirControl+MaxAcceleration → slide boost
inversion → hop rules.

Sources: github.com/Mikooboy/deadlock-cvar-list · deadlock.wiki (Movement/Jumping/Dashing/Stamina) ·
dlmovement.wiki · github.com/R2Northstar/NorthstarMods (`_utility_shared.nut`) ·
apexmovement.tech (Fundamental Slide/Air Tech, Velocities, Bunnyhop vs Slidehop) ·
apexlegends.fandom.com/wiki/Movement · projectborealis.com/movement ·
github.com/ProjectBorealis/PBCharacterMovement · developer.valvesoftware.com/wiki/Sv_gravity

---

## Report B — Destiny-grade shooting feedback

### Core sourced principles

1. **Recoil splits into two channels** [S]: cosmetic camera/gun motion (big, self-recovering)
   vs actual aim displacement (small, player-controlled). Bungie tunes them separately
   (PC recoil heavily reduced while keeping the animation; "camera movement" patched as its
   own dial). For server-auth aiming this is perfect: server only sees real control rotation.
2. **Priority-evict damage numbers, don't merge** [S — Warframe model]: small numbers still
   spawn but get evicted early when higher-priority (crit/large) numbers need room; crits
   linger longer. Color is the primary crit channel (white normal / yellow crit), size secondary.
3. **The tail makes guns sound powerful** [S — Chuck Russom]: first ~200 ms of a gunshot has
   little character; neglect the tail and all guns sound weak and identical. For full-auto:
   short per-shot sample, full tail only on trigger release.
4. **Shake for explosions, kick for shots** [S — Vlambeer]: per-shot = small recovering
   camera kick opposite fire direction; oscillating screen shake reserved for big events.
   Every camera impulse must decay to zero quickly (motion-sickness guard).
5. **Combat corridor** [S — Destiny GDC]: feedback must frame the reticle center, never
   occlude it (damage numbers/flash placement).
6. **Permanence sells impact** [S — Vlambeer]: decals, shells, corpses linger.

### Feedback spec checklist (starting values)

| Element | Starting parameters |
|---|---|
| Cosmetic camera kick (per shot) | 0.4° pitch, ±0.15° yaw random, spring-recover ~0.1 s; clamp stacked offset at 2.5° |
| Real aim recoil | keep existing path, magnitude ≤ cosmetic kick |
| Screen shake | explosions/kills only; ~1° amp, 0.25 s, damped oscillator |
| FOV punch | 2°, 0.1 s in / 0.25 s out; heavy events only, never per-shot |
| Landing dip | ~1.5° pitch-down or ~15 cm Z, 0.2 s spring, scaled by fall speed |
| Muzzle flash | 0.05–0.08 s; billboard core + 2 directional flames + sparks; 4–8 variants, random rot/scale |
| Muzzle light | point light 0.05 s, ~300 cm radius, no shadows |
| Tracer | fake streak along hitscan ray ~15,000 cm/s, motion-stretched; every shot (power fantasy) or every 2nd for autos |
| Impact VFX | 0.2–0.4 s sparks/puff + decal (world); pooled + count-limited in horde; smaller variant for chain hits |
| Damage numbers | world-anchored pooled widgets; white normal / yellow crit (~1.4× size) / type-colored dimmer DoT ticks; 0.7 s life (crit 1.2 s), rise + fade, XY jitter; soft cap ~30 with priority eviction; compact "12.5k" format |
| Hitmarker | existing flash + crit variant (larger/yellow) + kill variant (distinct shape/expand) |
| Fire sound | pre-baked transient+body+sub per-shot WAV; 4–6 round-robins; pitch 0.95–1.05; vol ±1.5 dB; concurrency max ~6 stop-oldest |
| Fire tail | separate cue on trigger release (~0.5–1 s); never stack tails per shot |
| Hit-confirm tick | <100 ms, high-passed ~2 kHz, quiet; crit +3–5 semitones |
| Kill confirm | third distinct sound, longer/lower, loudest of the three; pair with body reaction/permanence |
| Reload / footsteps / landing | 2–4 round-robins each, pitch ±3%; landing volume scaled by fall speed |
| Hitstop (later) | client-side 1–2 frame victim anim freeze on kill; never pause world (MP) |

Sources: Vlambeer "Art of Screenshake" (youtube SkgkIXZ_13Y) · PCGamesN Destiny-2-PC-recoil ·
Charlie INTEL recoil/camera tuning notes · GDC Vault 1022298 (Art of FP Animation for Destiny) ·
wiki.warframe.com Damage_Numbers · designingsound.org Chuck Russom gun design ·
Pro Sound Effects Mark Kilborn interview · splice.com weapon sound design ·
realtimevfx.com muzzle-flash + hitscan-tracer threads · outscal.com UE object pooling.

---

## Report C — GASP/Lyra inventory + layered ABP node recipe

### C.1 Game Animation Sample (F:\UnrealSamples\GameAnimationSample)

Skeleton: **UEFN Mannequin** (`SK_UEFN_Mannequin`, mesh `SKM_UEFN_Mannequin`,
IK rig `IK_UEFN_Mannequin` in `Content\Characters\UEFN_Mannequin\Rigs\`). The project also
contains the standard UE5 mannequins (`SKM_Manny_Simple`, `SK_Mannequin`) and
`Content\Blueprints\RetargetedCharacters\` — both ends of an IK-Retargeter bake exist in one
project. `M_Neutral_*` = weapon-ready style (our fit); `_Lfoot/_Rfoot` = foot-phase variants.
**Our project already contains one retargeted GASP clip: `Content/Characters/Mannequins/Anims/Slide/Slide_KneesOut_Loop`** — the pipeline is proven.

Has: **slide enter/loop/exit, sprint start/stop/turn, jump/fall/land (light+heavy), full
crouch 10-dir strafe set, stand+crouch aim-offset poses, idle turn-in-place.**
Paths relative to `Content\Characters\UEFN_Mannequin\Animations\`:

| Need | Asset |
|---|---|
| Slide enter | `Slide\M_Neutral_Slide_FootOut_Into_Lfoot/_Rfoot` (also `KneesOut`) |
| Slide loop | `Slide\M_Neutral_Slide_FootOut_Loop`, `Slide\M_Neutral_Slide_KneesOut_Loop` |
| Slide exit | `Slide\M_Neutral_Slide_FootOut_Out_Moving_Run`, `_Out_Idle_Stand`, `_Out_Idle_Crouch`, `_Out_Moving_Crouch/Walk` |
| Sprint loop/lean | `Sprint\M_Neutral_Sprint_Loop_F` (+`_FL/_FR/_F_L_20/_F_R_20`), `Sprint_Pose_Lean_*` |
| Sprint start/stop/turn | `Sprint\M_Neutral_Sprint_Start_F_*`, `Sprint_Stop_F_*`, `Sprint_Turn_L/R_090/_180_*` |
| Jump fall loop | `Jump\M_Neutral_Jump_Loop_Fall` |
| Jump starts | `Jump\M_Neutral_Jump_F_Start_Stand/Run/Sprint_Lfoot/_Rfoot` |
| Landing | `Jump\M_Neutral_Jump_F_Land_Stand/Run/Sprint_Light/_Heavy_*`, `Land_Roll`, `Land_Stumble` |
| Run start/stop/pivot (10-dir) | `Run\M_Neutral_Run_Stop_<dir>_*`, `Run_Start_<dir>_*`, `Run_Pivot_*` |
| Crouch set | `Crouch\M_Neutral_Crouch_Loop_<dir>` ×10, `Crouch_Pivot_*`; `Idle\M_Neutral_Crouch_Idle_Loop` |
| Aim offsets | `AimOffset\M_Neutral_AO_Crouch_*` + `BS_Neutral_AO_Stand` |
| Turn-in-place (deferred) | `Idle\M_Neutral_Stand_Turn_045/090/135/180_L/R` |

Caveat: clips are authored for motion matching — starts/stops/pivots carry heavy root motion
and uneven lengths. Loops and lands drop cleanly into a classic ABP; starts/stops need
trimming discipline. Retarget per memory `retarget-bake-via-python`
(`IKRetargetBatchOperation.duplicate_and_retarget`, target OUR mesh → our skeleton).

### C.2 Lyra (F:\UEAS\Samples\Games\Lyra)

**This checkout is SOURCE-ONLY — zero Content (verified recursively).** Niagara/sound/widget
migration requires downloading "Lyra Starter Game" from the Epic Launcher/Fab first (user
action). Locally valuable now: `Source\LyraGame\Animation\LyraAnimInstance.h/.cpp` (reference
for our anim-instance variable computation) and `Plugins\GameFeatures\ShooterCore\Source\`
(weapon feedback flow). Expected content paths once downloaded (verify then):
`Content/Characters/Heroes/Mannequin/Animations/ABP_Mannequin_Base`, `LinkedLayers/ALI_ItemAnimLayers`,
ShooterCore `Content/Effects/` (`NS_WeaponFire_*`), `Content/Audio/` (`sfx_Weapons_*`),
camera shakes + crosshair/hitmarker widgets in ShooterCore UserInterface.

### C.3 Layered ABP node recipe (Manny skeleton, classic — no motion matching)

**Anim instance variables** (computed in the AS `UAnimInstance` subclass):

| Variable | Computation |
|---|---|
| `Velocity` | `Pawn.GetVelocity()` |
| `GroundSpeed` | `Velocity.Size2D()` |
| `bShouldMove` | `GroundSpeed > 3 && CurrentAcceleration.SizeSquared() > 0` (prevents idle-pop while decelerating) |
| `bIsFalling` | `CharacterMovement.IsFalling()` |
| `bIsCrouching` / `bIsSliding` / `bIsSprinting` | from `URogueLocomotionComponent` |
| `Direction` (-180..180) | signed angle of velocity vs actor facing (manual atan2 in actor space — don't rely on a CalculateDirection binding) |
| `AimPitch` (-90..90) | `(Pawn.GetBaseAimRotation() - Pawn.GetActorRotation()).Normalized.Pitch` clamped — **BaseAimRotation, not ControlRotation: it replicates to sim proxies** |
| `AimYaw` | same delta yaw clamped ±90 (≈0 for aim-locked; keep for lag/leans) |
| `PlayRate` | `GroundSpeed / 450` when above 450, else 1.0; **clamp [0.8, 1.35]** |
| `LandRecoveryAlpha` | set ~1 on landing scaled by `abs(VelZ)/JumpZVelocity`, decay → 0 over ~0.4 s |

**BlendSpace `BS_Rifle_Strafe`** (2D): Horizontal `Direction` -180..180, grid 8, **Wrap
Input ON** (fixes the ±180 pop) — still place `Bwd` samples at BOTH +180 and -180.
Vertical `Speed` 0..450 **in authored units** (walk row ~200, jog row ~450; measure root
speed once per set). Samples: Fwd→0, Fwd_Right→45, Right→90, Bwd_Right→135, Bwd→±180,
Bwd_Left→-135, Left→-90, Fwd_Left→-45. Axis smoothing ~0.1–0.15 (Averaged). Above 450 runtime
speed: feed `min(GroundSpeed, 450)` to the axis and use PlayRate for the rest.
Sync group `Locomotion` on all BS players + idle (jog BS = Can Be Leader, idle = Always Follower).

**Locomotion state machine:** Idle ⇄ Cycle (`bShouldMove`, blends 0.15/0.25);
(Idle|Cycle)→JumpStart `bIsFalling && Vel.Z > 100` (`MM_Rifle_Jump_Start`, auto-rule →
FallLoop or `Vel.Z < 0`); (Idle|Cycle)→FallLoop `bIsFalling && Vel.Z <= 100` (walked off);
FallLoop→Land `!bIsFalling` (`MM_Rifle_Jump_Fall_Land`; →Cycle on `bShouldMove` blend 0.2,
→Idle auto-rule 0.3); Cycle→Slide `bIsSliding` 0.15 (GASP slide loop; →Cycle/Idle on
`!bIsSliding`; →FallLoop on `bIsFalling`); Crouch ⇄ on `bIsCrouching`. Use a Conduit
`ToAirborne` from grounded states to avoid N×M wires.

**Post-state-machine spine (exact node order):**
1. `Locomotion` SM → **Save Cached Pose** `LocomotionCache`.
2. Use Cached Pose → **Layered blend per bone**, Base Pose pin.
3. Second Use Cached Pose → **Slot 'DefaultGroup.UpperBody'** → Blend Poses 0 pin.
4. Layered blend config: Branch Filter **Bone `spine_01`, Blend Depth 4**; **Mesh Space
   Rotation Blend ON**. (Alternative stiffer split: `spine_03` depth 2 — one field to change.)
5. → **Aim Offset Player `AO_Rifle`** (Pitch=`AimPitch`, Yaw=`AimYaw`, Alpha bound to an
   `AimOffsetAlpha` var). AO source poses must be Additive **Mesh Space**. AO sits AFTER the
   layered blend (Lyra-style: aim bends the spine on top of upper-body action).
6. → **Apply Additive** (Additive = `MM_Rifle_Jump_RecoveryAdditive`, Alpha = `LandRecoveryAlpha`).
7. → **Slot 'DefaultGroup.DefaultSlot'** (full-body montages: deaths, hit reacts, equips).
8. → Output Pose.

Setup once: Anim Slot Manager → add slot **UpperBody**. Fire/reload montages → UpperBody
slot; deaths/equips → DefaultSlot. Per-weapon (Rifle/Pistol): v1 **Blend Poses by Enum**
inside Idle/Cycle; the Lyra ALI linked-layer pattern is the upgrade when a 3rd weapon exists.

Sources: dev.epicgames.com Lyra animation docs · using-layered-animations docs ·
aim-offset docs · UE forum CalculateDirection thread.
