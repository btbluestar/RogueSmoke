# Movement & Shooting Feel — Design Spec

**Date:** 2026-06-11
**Status:** Approved design, pre-plan
**Owner workstream:** game feel (movement + shooting), D-0014/D-0015 follow-on

## 1. Goal

Make RogueSmoke's moment-to-moment movement and shooting feel *best-in-class* for its genre:

- **Movement north star:** Apex Legends / Deadlock blend, **leaning Deadlock** — snappy,
  high-responsiveness, momentum as a tool (slide-hop chains) rather than a simulation.
  Keep the existing sprint / slide / double-jump kit (D-0015).
- **Shooting north star:** **Destiny 2** — feedback-maximalist. Every shot pops: VFX,
  camera kick, damage numbers, audio. All four juice layers are in scope.

Diagnosed problems (user-confirmed, all four):
jump/air floatiness, slow start/stop response, animation disconnect (linked upper/lower
body, foot sliding, no slide pose, no landing weight), and camera lag/float.

Root causes found in code/content:

- Heroes (`BP_Vanguard`, `BP_Bombardier`) run on **`ABP_TP_Rifle` from `Variant_Shooter/`**
  (template leftover, basic full-body graph — no upper-body aim layer). This is the
  linked-body problem.
- Stock CMC numbers: `GravityScale` 1.0, `MaxAcceleration` 2048, `BrakingDecelerationWalking`
  2048 — template-walk tuning, reads floaty/indirect.
- `CameraBoom.bEnableCameraLag = true` — camera trails the pawn.
- Shooting has mechanics but almost no feedback layer: debug-line tracers, no muzzle/impact
  FX, no damage numbers, near-silent audio, raw recoil with no camera feel.

## 2. Architecture overview

Five workstreams. All gameplay-affecting changes stay server-authoritative; every feedback
layer is client-side cosmetic. No C++ expected (all knobs reachable from AngelScript);
documented fallback if the anim-instance spike fails (§3.0).

### 2.1 Movement physics + live tuning console (AngelScript)

New tunables on `URogueLocomotionComponent`, applied in `Initialize()` (same client+server
pattern as today; CMC prediction reconciles). Starting values — to be finalized by research
phase + in-PIE tuning:

| Knob | Now | Start point | Why |
|---|---|---|---|
| `GravityScale` | 1.0 | ~1.6 | #1 floaty culprit; faster fall = weight |
| `JumpZVelocity` | 600 | ~750 | similar height, faster arc under heavier gravity |
| `MaxAcceleration` | 2048 | ~3500 | near-instant speed-up; input feels direct |
| `BrakingDecelerationWalking` | 2048 | ~2800 | stop when input released |
| `BrakingFrictionFactor` + separate braking friction | stock | on, tuned | crisp stops without hurting turning |
| `AirControl` | 0.35 | ~0.55 | Deadlock-style air steering |

Mechanics (not just numbers):

- **Variable jump height:** `JumpMaxHoldTime` ≈ 0.12 s (tap = hop, hold = full). CMC-native,
  fully predicted.
- **Slide-hop momentum:** jumping out of a slide preserves horizontal velocity; the slide
  end-path must not bleed speed on the jump frame (audit `EndSlide()` friction restore
  ordering). Target chain: sprint → slide → jump → land → slide.

**MoveTune console** (`Script/Debug/`, dev-only, host):
- `MoveTune <Param> <Value>` — set any movement/camera-feel knob live in PIE.
- `MoveTune dump` — print all current values as a paste-ready defaults block.
- Feel convergence happens in the user's hands in PIE, not in offline guesses.

### 2.2 Camera feel (AngelScript, owning-client only)

New `URogueCameraFeelComponent` on `AHeroCharacter`; the existing focus-zoom blend moves in
so camera logic has one home. Spring-driven offsets composited per tick:

- **Camera lag off** (`bEnableCameraLag = false` or near-zero) — bolted-to-you feel.
- **Landing dip** — downward impulse scaled by landing fall speed, spring recovery.
  Weight lives in camera/anim/audio; movement input is never locked.
- **Sprint/slide FOV kick** — +4–6° sprint, +8° slide, fast blends.
- **Fire kick** — real pitch displacement stays (smaller), plus visual-only kick with
  spring recovery and a per-shot FOV/offset punch. Destiny trick: camera hits harder than
  the aim point moves.
- All magnitudes MoveTune-tunable.

### 2.3 Layered animation system (script computes, graph blends)

Kills the linked-body problem: legs answer to velocity, torso answers to the crosshair.

- **`URogueHeroAnimInstance`** (new, `Script/Player/`) — AS subclass of `UAnimInstance`
  (verified `Blueprintable`). `BlueprintUpdateAnimation` computes all graph inputs from the
  pawn: ground speed, strafe direction angle, aim pitch, `bIsFalling`, `bCrouched`,
  `bSliding`, `bSprinting`, time-since-landing, landing fall speed. Hot-reloadable script;
  zero logic in the ABP.
- **`ABP_Hero`** (new) — built in the editor GUI by the user from node-by-node
  instructions; reparented to the AS class. Structure:
  - **Base/lower body:** locomotion state machine — Idle → 8-direction strafe blendspace
    (`MF_Rifle_Walk_*` / `MF_Rifle_Jog_*`, axes = direction angle + speed) → Jump
    (start/apex/fall/land, `MM_Rifle_Jump_*`) → Crouch → Slide.
  - **Foot-slide fix:** blendspace speed axis calibrated to real speeds (600 walk /
    960 sprint), play-rate scaling, sync groups.
  - **Upper body:** `Layered blend per bone` at `spine_01`; `AO_Rifle` aim offset driven by
    aim pitch (yaw ≈ 0, aim-locked); **UpperBody montage slot** for `MM_Rifle_Fire` /
    `MM_Rifle_Reload`.
  - **Landing recovery additive** (`MM_Rifle_Jump_RecoveryAdditive`) scaled by fall speed.
- **Montage triggers:** fire from existing `Multicast_FireFX`; reload via one new
  Unreliable multicast (reload state is server-only today).
- **Anim sources, in priority order:** (1) in-project
  (`Content/Characters/Mannequins/Anims/` — full rifle/pistol strafe sets, aim offsets,
  jumps, fire/reload, hit reacts), (2) **Game Animation Sample (GASP)** — migrate individual
  sequences (start/stop/pivot, better jump/land, slide if present) and retarget to
  Manny-Simple via the established headless retarget pipeline; we use GASP as an *anim
  library*, NOT its motion-matching system, (3) **Lyra sample**, (4) download (user
  executes) — last resort.
- **Slide pose:** prefer a GASP/Lyra slide sequence; fallback is a synthesized pose
  (crouch pose + lean). If neither reads well, user downloads a free slide anim and we
  retarget.
- **Deferred (post-MVP polish):** turn-in-place (standing mouse turns rotate the feet —
  Lyra-style TIP fixes it later), distance matching, motion matching.

### 2.4 Shooting feedback (Destiny layer; all cosmetic, server stays authoritative)

1. **VFX (Niagara):** muzzle flash; real tracer streak (muzzle → impact) replacing debug
   lines; impact split by surface (enemy = blood/ichor puff, world = sparks/dust).
   `Multicast_FireFX` payload gains a per-impact hit-enemy flag. Asset priority: migrate
   from Lyra (weapon VFX are shippable) / engine starter content before authoring new.
2. **Damage numbers:** new Unreliable owning-client RPC carrying (location, amount,
   crit/DoT flags) per hit — server already computes damage. HUD spawns pooled
   rising/fading numbers via the DPI-correct `WidgetLayout` path. Pooled + merged under
   horde load (chain/pierce volleys must not melt the frame).
3. **Camera/recoil:** §2.2 fire kick + FOV punch.
4. **Audio:** fire (layered punch), hit-confirm tick, kill confirm, reload, footsteps
   (receivers for the existing `BP_AnimNotify_FoleyEvent` notifies), jump/land.
   Source priority: in-project template content → Lyra/GASP migration → free packs
   (user downloads from a provided shortlist).

### 2.5 Execution structure

1. **Research phase** — three parallel read-only agents:
   (a) Deadlock/Apex movement values + techniques (community analysis);
   (b) Destiny 2 shooting-feedback breakdowns in implementable terms;
   (c) Lyra anim layering + UE5.7 layered-ABP best practice.
   Output: research notes doc that finalizes §2.1 starting numbers and the §2.3 ABP node
   spec.
2. **Plan** (writing-plans skill) with independent tasks → **subagent dispatch** per task
   (subagent-driven-development). File authoring parallelizes; **all compile/PIE/editor
   verification serializes through the main session**. `SmokeTest.ps1` before any commit.
3. **User hands-on tracks:** guided ABP build in the editor GUI; PIE feel sessions with
   MoveTune at each milestone (`dump` → numbers become defaults).
4. **Spike first:** Task 1 verifies the AS `UAnimInstance` subclass compiles
   (`run_code_test`) and an ABP can reparent to it, before anything depends on it.
   Fallback if it fails: a thin C++ `URogueHeroAnimInstance` with `BlueprintReadOnly`
   fields, AS keeps computing values via a component.
5. **No git worktrees** (CLAUDE.md rule): one editor, multi-GB engine per worktree —
   branch in-place; isolation comes from independent files + serialized verification.

## 3. Risks & open items

| Risk / open item | Mitigation |
|---|---|
| AS `UAnimInstance` subclass unproven in this fork | Spike task #1; C++ fallback documented (§2.5.4) |
| GASP/Lyra install paths | Provided: GASP = `F:\UnrealSamples\GameAnimationSample`, Lyra = `F:\UEAS\Samples\Games\Lyra` (both verified present) |
| GASP may lack a slide anim | Fallback chain in §2.3 |
| ABP GUI build is manual | Node-by-node instructions + MCP verification after each stage |
| Damage-number perf under horde + chain | Pooling + merge policy from day one |
| Recoil-feel vs aim-authority interaction | Visual kick is client-only; authoritative spread unchanged |
| Niagara authoring headless is painful | Migrate Lyra systems first; GUI-or-python decided per asset |

## 4. Success criteria

- User-judged in PIE: jump arc reads weighty, start/stop is immediate, slide-hop chain
  carries speed, camera feels attached.
- Upper body visibly tracks the crosshair pitch while legs strafe independently; fire and
  reload play on the upper body without stopping the legs.
- No visible foot sliding at walk or sprint speed.
- Every shot produces: muzzle flash, tracer, impact FX, audio; every hit additionally:
  hitmarker, damage number, hit-confirm tick.
- `SmokeTest.ps1` stays green; multiplayer PIE (2-player listen server) shows correct
  remote-player anims (strafe + aim pitch + fire montage on simulated proxies).
- All final feel numbers land as defaults in `URogueLocomotionComponent` /
  `URogueCameraFeelComponent`, dumped from a MoveTune session.
