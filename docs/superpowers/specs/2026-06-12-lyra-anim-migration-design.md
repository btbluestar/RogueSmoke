# Lyra Animation Stack Migration — Design

**Date:** 2026-06-12
**Status:** Approved (brainstorm with user, all scope questions answered)
**Supersedes:** the v1 hand-built anim stack (`ABP_Hero`, `BS_Rifle_Strafe`, GASP-retargeted clips) once parity is signed off.

## Goal

Replace the v1 hand-built animation stack with Lyra's production animation architecture —
linked anim layers per weapon, distance-matched starts/stops/pivots, turn-in-place,
professional aim offsets — plus Lyra's gun models, weapon audio/VFX, ContextEffects
footsteps, the heat-based recoil/spread model, and a new Deadlock-style stamina system.
Everything else from the movement/shooting-feel workstream (movement physics, slide-hop,
camera feel, MoveTune, FireFX seam, damage numbers) survives untouched.

## Sources on disk

| Source | Path | Use |
|---|---|---|
| Lyra content (5.7, Epic download) | `F:\UnrealSamples\LyraStarterGame` | All migrated assets |
| Lyra C++ source (reference only) | `F:\UEAS\Samples\Games\Lyra\Source` | API reference for `ULyraAnimInstance`, ContextEffects port |
| Game Animation Sample (GASP) | `F:\UnrealSamples\SlidingAnimations\GameAnimationSample` | Authored slide enter/loop/exit set |
| Stock UE 5.7 editor | `C:\Program Files\Epic Games\UE_5.7` | Fallback surgery environment only |

Verified present in Lyra content: `ABP_Mannequin_Base`, `ALI_ItemAnimLayers` +
`ABP_ItemAnimLayersBase`, per-weapon layer ABPs (`ABP_RifleAnimLayers`,
`ABP_PistolAnimLayers`, `ABP_ShotgunAnimLayers`, `ABP_UnarmedAnimLayers`, + Feminine
variants), full distance-matched locomotion sets per weapon, multi-sample aim offsets
(ADS + hipfire + crouch), turn-in-place, crouch sets, `SK_Rifle`/`SK_Pistol`/`SK_Shotgun`
meshes with physics assets and weapon ABPs, character + weapon fire/reload/equip montages,
rifle sounds. All under `Content/Characters/Heroes/Mannequin/**` and `Content/Weapons/**`.

## Decisions made during brainstorm

1. **Weapon scope:** architecture now, guns later. Migrate ALL four anim sets + gun
   meshes; gameplay keeps the single rifle. Adding a pistol later = data task
   (new weapon def + link a different anim layer). No weapon-switching gameplay
   in this workstream.
2. **Skeleton:** adopt Lyra's skeleton + mannequin mesh wholesale (full-detail Manny,
   not `_Simple`). Zero asset fixups; every Lyra asset works untouched. Our hero
   switches mesh/skeleton; v1 GASP-retargeted anims retire with the v1 stack.
3. **v1 fate:** keep wired and functional until the Lyra stack passes the user's
   side-by-side feel check, then a cleanup commit unhooks it (history keeps the assets).
4. **Gems scope: everything now** — anims, guns, their sounds/FX, ContextEffects
   footsteps, recoil/spread model. Phased so the game is always working between phases.
5. **ABP data source — Approach A:** copy Lyra graphs verbatim and re-parent
   `ABP_Mannequin_Base` onto `URogueHeroAnimInstance` (AngelScript), extended with the
   Lyra-named properties the graph binds to. C++ shim (`Approach B`) is the fallback.
6. **Stamina model:** Deadlock pips (discrete charges), sprint free.

## Architecture

### Migration mechanics (Phase 0)

- **Direct file copy preserving `/Game` paths** so internal references survive:
  - `Content/Characters/Heroes/Mannequin/**` → same path in our project (no collision
    with our existing `Characters/Mannequins`).
  - `Content/Weapons/{Generic,Rifle,Pistol,Shotgun}/**` → same path.
  - Dependency closure (audio buses/attenuations under `Content/Audio`, shared
    materials/textures, Niagara under `Content/Effects`): copy candidate roots, then
    walk dependencies with the asset registry **in our editor** (`assets_dependencies` /
    python), copy what is missing, repeat until the graph is closed. No guessing.
- **Class filter:** migrate only assets whose classes are engine types (AnimSequence,
  AnimBlueprint, SkeletalMesh, MetaSound, NiagaraSystem, Montage, DataAsset of engine
  classes...). Assets parented to LyraGame C++ classes (B_Weapon, experiences, pawn
  data) stay behind — except those deliberately adopted via CoreRedirects (below).
- **CoreRedirects strategy:** ini `[CoreRedirects]` entries map Lyra C++ class paths to
  our classes so copied assets load without surgery:
  - `/Script/LyraGame.LyraAnimInstance` → our AS anim instance class
    (AS classes register under `/Script/Angelscript.<Name>`).
  - Phase 4 extends this: Lyra's ContextEffects notify + library classes → our C++ ports,
    which keeps the footstep notifies already authored on every copied anim alive, and
    lets Lyra's effect-library DataAssets load as-is.
- **Phase 1 opens with a time-boxed spike** proving the CoreRedirect re-parent on a copy
  of `ABP_Mannequin_Base` before committing. Fallbacks, in order: pre-surgery in the
  stock 5.7 editor inside the Lyra project (re-parent where everything compiles, then
  export); Approach B C++ shim parent ported into `Source/RogueSmoke`.

### Base anim stack (Phase 1)

- Hero switches to Lyra's mannequin mesh + skeleton.
- `ABP_Mannequin_Base` becomes the hero anim class, re-parented onto
  `URogueHeroAnimInstance`, which gains the Lyra-named properties the graph property-
  access bindings resolve by name: `GroundDistance`, velocity/acceleration breakdowns,
  and the `GameplayTag_*` booleans (IsFiring, IsADS, ...) filled from our already-
  replicated state — same pattern the v1 instance uses today, so simulated proxies work.
- Linked-layer system comes verbatim: `ALI_ItemAnimLayers` → `ABP_ItemAnimLayersBase` →
  `ABP_RifleAnimLayers` linked by default.
- **Two deliberate gameplay-side changes:**
  1. **Actor-level idle free-look (`TickFacing`) is disabled** while the Lyra stack is
     active. Lyra solves feet-planted/torso-tracking properly at the animation level
     (RootYawOffset + authored turn-in-place). Running both would double-rotate.
     The v1 mechanism was a workaround for having no turn anims.
  2. **Slide stays ours** — Lyra has none. One custom state grafted into the copied
     base ABP's state machine (the only graph modification we make), driven by
     `bIsSliding`, using **GASP's authored slide set**: enter (L/R-foot variants),
     loop, and exits to crouch/idle/run/sprint selected from locomotion state.
     If GASP's skeleton differs from Lyra's, reuse the v1 Task-6 retarget bake pipeline.
- Sprint rides Lyra's blendspaces with our existing PlayRate plumbing for
  above-authored speeds.

### Gun models (Phase 2)

- `SK_Rifle` attached to the mannequin weapon socket, running `ABP_Weap_Rifle`
  (animated magazine/bolt).
- Task-9 montage wiring swaps to Lyra montages: `AM_MM_Rifle_Fire/Reload` on the
  character, `AM_Weap_Rifle_*` on the gun mesh.
- Pistol/shotgun meshes + anim sets imported and catalogued, not equipped — they wait
  for a future weapon-switching workstream.

### Weapon audio & VFX (Phase 3)

- Harvest Lyra rifle MetaSounds, muzzle flash + tracer + impact Niagara, and their
  dependency closure (attenuations, control buses, shared Niagara modules).
- They fill the **Task-11 FireFX slots that were built null-safe for exactly this**;
  the existing seam-driven FireFX multicast stays the trigger path. We do NOT adopt
  Lyra's GameplayCue tags.
- Not ported: Lyra's dynamic audio mixing C++ (`LyraAudioMixEffectsSubsystem`).
  Buses work statically; dynamic mixing is a catalogued later gem.

### ContextEffects footsteps (Phase 4)

- The one true C++ port: Lyra's ContextEffects system (~4 small classes — anim notify,
  effects library, actor component, settings — from `LyraGame/Feedback/ContextEffects`)
  into `Source/RogueSmoke`. Consistent with project layering: cosmetic hot-path in C++,
  fired by anim notifies.
- CoreRedirects map Lyra's notify + library class names to the ports → authored
  footstep notifies on all copied anims fire, library DataAssets load as-is.
- Surface-aware variation activates when levels get physical materials; until then the
  default surface mapping plays. Component attached to the hero from AS.

### Recoil & spread (Phase 5)

- Port Lyra's heat model **as data, not code**: spread-vs-heat curve, heat-per-shot,
  cooldown rate, implemented in our AngelScript weapon (owner of `GetSpreadDegrees`),
  seeded with Lyra's rifle curve values.
- Camera recoil maps onto the existing `CameraFeelComponent` kick, tuned toward Lyra's
  per-shot values. All knobs join MoveTune's dump.

### Stamina — Deadlock pips (Phase 6)

- **Model:** 3 pips at start. Slide costs 1, slide-hop's jump costs 1, future
  double-jump costs 1. **Sprint is free** (gating sprint would fight the movement feel).
  Pips regen one at a time on a timer; regen pauses briefly after a spend.
- **Attributes:** `Stamina` / `MaxStamina` on a C++ GAS AttributeSet (per D-0013).
  Meta-progression upgrades later are plain GameplayEffects (+1 pip, faster regen),
  identical in shape to every other upgrade card.
- **Authority:** server-authoritative spend/regen on slide/jump events; owner client
  predicts the gate so movement never feels laggy.
- **UI:** pip row on the HUD near the health bar, driven by attribute-change delegates
  exactly like health.
- **Tuning:** pip count, regen seconds, costs become MoveTune knobs; MoveSmoke gains
  stamina assertions.

## Verification & user-test gates

- Every phase lands with **SmokeTest green** (all 9 levels) before commit.
- **Machine verification per phase:** bone-level pose probes (the tooling that caught
  the v1 −35° AO bug) for the re-parent and slide graft; MoveSmoke for movement rules;
  stamina assertions in Phase 6; 2-player listen-server checks after Phase 2 (anim
  state derives only from replicated data) and Phase 6 (stamina replication).
- **User feel-check gates (AFK-aware):** the user tests after every phase when
  available; work does NOT block on this — machine verification gates the next phase,
  and the user's queued checkpoints are: (a) after Phases 1–2 (anim stack + guns),
  (b) final pass over audio/VFX/footsteps/recoil/stamina. **v1 is retired only after
  the user's parity sign-off**, in a dedicated cleanup commit.

## Risks & fallbacks

| Risk | Mitigation |
|---|---|
| CoreRedirect to an AS parent class fails in the fork | Spike first; fallback: pre-surgery in stock 5.7 Lyra project, then C++ shim (Approach B) |
| GASP slide anims on a different skeleton | Proven v1 Task-6 retarget bake pipeline |
| Fork-vs-stock asset serialization quirks | Same engine version (5.7) both sides; lowest risk |
| Lyra graph reads something only `LyraCharacterMovementComponent` has | The known case is `GroundDistance` — computed by our AS instance instead; spike surfaces any others |
| Turn-in-place fights residual actor-yaw logic | TickFacing disabled with the Lyra stack; MoveTune knobs revisited |
| Full-detail Manny heavier than `_Simple` | 4-player co-op budget; acceptable, measure in perf pass |

## Out of scope (this workstream)

- Weapon-switching gameplay (equip flow, weapon defs) — future workstream; assets ready.
- Feminine anim-set variants — imported with the folders, unused.
- Lyra dynamic audio mixing C++; accolades; full GameplayCue adoption.
- Physical materials for levels (Phase 4 works with default surface until then).
- Meta-progression stamina upgrades (system is upgrade-ready; cards come with the
  meta-progression workstream).

## Gems catalog (later workstreams)

- **ContextEffects beyond footsteps:** jump/land effects per surface.
- **Dynamic audio mixing** (`LyraAudioMixEffectsSubsystem`): ducking, slomo filters.
- **Accolade system** (kill streak/“double kill” toasts) — pairs with our kill-confirm.
- **Lyra spectator/killcam PocketWorlds tech.**
- **Quinn mesh + Feminine anim sets** as a second hero body.

## Docs & decisions on completion

- New DECISIONS entries: Lyra anim stack adoption (architecture + skeleton),
  stamina pips system.
- GLOSSARY: linked anim layer, anim layer interface, RootYawOffset/turn-in-place,
  ContextEffects, stamina pip; revise Slide/Anim-instance entries.
- Retire `docs/guides/ABP_HERO_BUILD_GUIDE.md` (historical note pointing here).
- startup.md freshness pass.
