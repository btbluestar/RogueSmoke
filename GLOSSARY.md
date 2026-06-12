# Glossary

> Shared vocabulary for RogueSmoke. One name per concept — use these exact terms in code,
> docs, and chat with the AI so nothing drifts into three names for the same thing.

## Run structure

- **Run** — one playthrough from start to victory/wipe. Ends on death or successful finish;
  meta-progression is awarded between runs. (Whether a run is one raid or many is *still open* —
  see `DECISIONS.md`.)
- **Raid** — a self-contained mission with an objective and an extraction.
- **Floor** — one generated level within a raid; the unit the floor loop repeats over.
- **Room** — a space within a floor (combat / shop / event). Procedurally placed.
- **Encounter** — a single fight or challenge inside a room.
- **Extraction** — the act of leaving alive; the tension cap that turns "survived" into "won".

## Combat & enemies

- **Fodder / Swarm** — cheap, numerous enemies. Implemented as **Mass agents**, not Actors.
- **Elite** — a stronger enemy worth focusing/clustering. A full **Actor** (`EliteEnemyBase`).
- **Boss** — a raid-goal anchor enemy. Actor-based.
- **Clustered** — the synergy condition: enemies recently pulled/grouped. Set by setup
  abilities, read by payoff abilities for bonus effect. Time-limited.
- **Density** — how tightly enemies are grouped; a *mechanic*, since it enables synergies.
- **Telegraph** — a visible wind-up before an enemy attack lands; the player's counterplay window.
  A hard readability requirement (GDD §10). Shown as ground danger rings (an outline + a fill disc
  that reaches the edge exactly at impact), a body warning pulse, and per-archetype swell
  (`TelegraphSwell`); replicated to clients. Ground-target zones (boss artillery) ring via
  `UCombatSubsystem::ShowTelegraphZone`. Niagara polish comes later; debug draw stays as dev overlay.

## Enemy roster (bio-horde, D-0017)

- **AttackingElite** — `AAttackingElite`, the C++ base for the attacking elites: self-contained body +
  collision, shared targeting/approach/telegraph loop; per-archetype attack is an AngelScript override.
- **Crawler** — swarm fodder with contact melee (`AFodderEnemy`). Doesn't gate the objective.
- **Carapace** — tanky shield elite with a telegraphed radial slam; the taunt/cluster synergy anchor.
- **Spitter** — ranged elite that kites and lands a telegraphed shot.
- **Bloater** — suicide bomber: telegraph then detonate a radial blast (on contact or on death).
- **Lunger** — gap-closer: telegraph then lunge + melee (dodge it with the slide).
- **Brood-mother** — mini-boss / raid anchor: cycles ranged spit, summoned Crawler waves, and artillery AoE.

## Movement (Apex/Deadlock-style, D-0015 + D-0021)

- **Sprint** — hold-to-run; raises move speed in any direction (omnidirectional). Base 600 ×1.6 = 960.
- **Slide** — the fastest ground state: crouch while sprinting boosts to 1.3× sprint (cap 1.5×),
  carries on low friction, sustains downhill, ends when speed bleeds below 0.9× base. The MVP's
  skill-expression traversal move. (D-0021 numbers supersede the D-0015 list.) Costs one
  **stamina pip** (D-0023); out of pips, the crouch press degrades to a plain crouch. Animated
  by the GASP slide set as dynamic montages — see Anim instance.
- **Slide-hop** — jumping out of a slide: auto-stands, keeps the slide's speed through the air
  (`FallingLateralFriction 0`), and re-enters the slide on landing if crouch is held. The
  **boost-arming threshold** (boost only applies below 1.1× sprint speed) means chains *carry*
  speed but can't *build* it — Apex's anti-bhop rule.
- **Double-jump** — a second mid-air jump (`MaxJumpCount = 2`, second impulse slightly weaker);
  a future meta-progression unlock.
- **Locomotion component** — `URogueLocomotionComponent`, the AngelScript component on the hero that
  owns the sprint/crouch/slide/jump state machine and its tunables (the seam upgrades modify).
- **MoveTune** — host-only console exec: `MoveTune` dumps all ~39 movement/camera-feel knobs,
  `MoveTune <Param> <Value>` sets one live in PIE, `MoveTune dump` prints the block to bake into
  component defaults. Partner exec **MoveSmoke** asserts the slide rules (boost/cap/anti-bhop)
  and gates SmokeTest.
- **Anim instance** — `URogueHeroAnimInstance` (AngelScript): the data source both anim stacks
  read. For the live **Lyra stack** (D-0022) it is the re-parented base class of
  `ABP_Mannequin_Base` and supplies `GroundDistance` + the `GameplayTag_*` bools from
  replicated state; the graph computes its own locomotion data. The retired v1 fields
  (StrafeSpeed/PlayRate/Direction…) remain for the on-disk `ABP_Hero` until parity sign-off.
- **Stamina pip** — one discrete charge of the movement economy (D-0023): slide and slide-hop
  each cost 1, sprint is free; regen one pip per `StaminaRegenSeconds` after a post-spend
  pause. Lives on `URogueMovementSet` (GAS) so upgrades are plain GameplayEffects; HUD renders
  one square per pip under the health bar.

## Animation (Lyra stack, D-0022)

- **Lyra anim stack** — `ABP_Mannequin_Base` + linked anim layers, copied verbatim from Lyra
  5.7 and re-parented onto our AS anim instance via CoreRedirects. Distance matching,
  turn-in-place, cardinal strafes — production-grade locomotion we did not hand-build.
- **Linked anim layer** — a per-weapon anim overlay (`ABP_RifleAnimLayers`, pistol/shotgun
  variants imported) implementing `ALI_ItemAnimLayers`, linked at runtime with
  `Mesh.LinkAnimClassLayers`. Weapon swap = link a different layer; the base graph never changes.
- **RootYawOffset / turn-in-place** — Lyra's feet-planted idle: the actor yaw follows the
  camera but the mesh root counter-rotates (offset accumulates), and authored turn anims unwind
  it past the clamp. Replaces v1's actor-level idle free-look (`bActorLevelFreeLook`, now off).
- **CoreRedirects migration** — ini Class/StructRedirects that re-point Lyra C++ class/struct
  references in copied assets at our classes on load. Rule of thumb: every renamed UCLASS needs
  a ClassRedirect, every renamed USTRUCT that assets serialize needs a StructRedirect (entries
  deserialize EMPTY without it — no warning).
- **ContextEffects** — the ported Lyra system (C++, `Feedback/ContextEffects/`) that turns
  anim notifies into surface-aware footstep/land sounds + VFX: notify (tag) → component →
  subsystem → `CFX_*` library lookup by effect tag + surface context. Surface comes from a
  foot trace's physical material via the `RogueContextEffectsSettings` map.

## Synergy system (signature feature)

- **Synergy** — an effect that combines across players, greater than either part alone.
- **Setup** — the ability that *creates a condition* (e.g. taunt pulls + marks `Clustered`).
- **Payoff** — the ability that *exploits the condition* (e.g. barrage hits clustered enemies harder).
- **Kit** — a hero's distinct ability set. Distinct kits keep synergies legible.
- **Vanguard** — the MVP setup hero (taunt).
- **Bombardier** — the MVP payoff hero (barrage).

## Roguelike systems

- **Upgrade** — a run-altering pickup chosen between/within encounters.
  - **Personal upgrade** — modifies the picking player's kit/stats.
  - **Team upgrade** — affects the whole squad.
  - **Synergy upgrade** — unlocks/strengthens a cross-player interaction (e.g. Chain Detonation).
- **Rarity** — upgrade tier affecting power/availability.
- **Weapon upgrade** — a personal upgrade on the *gun* track (`WeaponDamageBonus`, `FireRateBonus`,
  pierce/chain/proc attributes on `URogueCombatSet`); distinct from ability upgrades (AbilityPower).
  - **Pierce** — a bullet passes through extra enemies; world geometry always stops it.
  - **Chain** — a hit arcs bonus damage to the nearest other enemies in range (rewards Density).
  - **Burn / Poison** — on-hit DoT procs; magnitude derives from the hit's damage. Burn = fast and
    short, Poison = slow and bigger total. One slot per type on `UHealthComponent` (refresh, no stacking).
- **Team XP / Team level** — the shared pool every kill feeds (per-archetype `XPValue`); a level-up
  pauses the raid for an upgrade pick, rarity-weighted by the level reached (D-0018). One pool for
  the whole squad — leveling is a team event, not per-player.
- **Upgrade chest** — `AUpgradeChest`, dropped where the mini-boss fell; any living player standing
  next to it opens it for a squad-wide **synergy upgrade** pick (the only source of synergy cards).
- **Stack** — one owned copy of an upgrade card; repeat picks deepen it ("Lv 2 → 3") up to the
  card's `MaxStacks` cap (≤0 = unlimited, used by utility filler). (D-0019)
- **Milestone upgrade** — a modifier card unlocked by stacking a track to its prerequisite count
  (e.g. 3× Piercing Rounds); once eligible it's guaranteed a slot in that player's next hand.
- **Eligibility** — whether a card may be offered: under its stack cap AND prerequisites met.
  Synergy cards use squad-scope (duo) eligibility: prereq A and B held by two different players
  (one player may hold both when playing solo).
- **Utility card** — consolation filler (squad heal, small stat dribble) padding a hand that
  eligibility filtering left short; a pick screen never shows a dead/short hand.
- **Squad reroll** — one shared charge per raid; any player may spend it to re-roll their own hand.
- **Behavior evolution** — a chest/milestone card that changes a *mechanic*, not a number
  (chain arcs ignite, poisoned enemies burst on death, taunt becomes a vortex). Hit-path
  evolutions compile into the seam; death-path evolutions script on the kill event. (D-0020)
- **Ability track** — a hero-gated card line: stat cards → milestone → behavior evolution
  (Taunt: Magnetic Pull → Concussive Taunt → Event Horizon; Barrage: High Explosives →
  Twin Salvo → Carpet Bombing). `RequiredHeroClass` keeps it out of the wrong hero's hand.
- **Wave director** — the pure function (`RaidDirector::ComputeWavePlan`) scaling fodder-wave
  pressure with team level and squad size: size, tempo, deterministic elite injections.
  Injected elites never gate the arena clear.
- **Meta-progression** — persistence *between* runs (scope still open).

## Technical / architecture

- **The Seam** — `UCombatSubsystem` (C++): the single combat API AngelScript calls. Hides
  whether a target is a Mass agent or an Actor; authoritative (mutations server-only).
- **Mass agent** — a data-oriented entity (Unreal Mass) used for fodder; not an Actor.
- **Fragment** — a Mass data struct on an agent (e.g. `FSwarmHealthFragment`, `FClusterTagFragment`).
- **Processor** — Mass system that runs logic over agents (movement, pull, damage application).
- **Trait** — composes which fragments make up a Mass archetype.
- **Authority** — the server's exclusive right to mutate game state. "Has authority" = is the server.
- **Listen server** — host machine that both simulates the game and plays it.
- **Seed** — the integer that drives deterministic generation; one master seed per run.
- **RunManager** — server-side owner of the seed and run state machine.

## Asset conventions (see CODING_STANDARDS §2)

- **`BP_`** — Blueprint subclass of a script/C++ class (asset assignment only, no logic).
- **`DA_`** — DataAsset (designer-authored definitions: heroes, items, enemies, rooms).
- **`DT_`** — DataTable (tabular balance data: loot weights, stat curves).
- **GameplayTag** — composable label for categorization (damage types, synergies, statuses).
