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

## Movement (Apex/Deadlock-style, D-0015)

- **Sprint** — hold-to-run; raises move speed in any direction (omnidirectional).
- **Slide** — a momentum burst entered by crouching while sprinting; carries on low friction, decays,
  speeds up downhill. The MVP's skill-expression traversal move.
- **Double-jump** — a second mid-air jump (`MaxJumpCount = 2`); a future meta-progression unlock.
- **Locomotion component** — `URogueLocomotionComponent`, the AngelScript component on the hero that
  owns the sprint/crouch/slide/jump state machine and its tunables (the seam upgrades modify).

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
