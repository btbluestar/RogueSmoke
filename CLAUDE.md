# CLAUDE.md

Project memory for Claude Code. Kept deliberately short — detailed docs are imported/listed below.

## What this is
**RogueSmoke** — a co-op roguelike on **Unreal Engine 5.7** using the **UnrealEngine-Angelscript**
(Hazelight fork). Co-op uses a **listen server** (host is a player).

**Layering — "script the decisions, compile the simulation" (see Rogue_Smoke_MVP_Architecture.md):**
- **AngelScript** (`Script/`, `.as`): gameplay you iterate on — abilities, synergies, upgrades,
  objective/extraction flow, UI.
- **C++ + Mass** (`Source/RogueSmoke/`): the hot path — swarm enemy simulation, replicated health,
  spawning/pooling, and the authoritative combat seam `UCombatSubsystem`.
- **Blueprints / DataAssets** (`Content/`): asset assignment and designer tuning, **no logic**.

This is **not** an AngelScript-only project. Do not refactor away the C++/Mass layer (see D-0002).

## Critical context
- **Custom source-built engine** (AngelScript requires engine modifications). Don't assume stock
  UE behavior for engine-level changes. Build steps: see `SETUP.md`.
- **Networked from the ground up** — treat every gameplay change as server-authoritative.
- **Abilities never iterate enemies directly** — they call `UCombatSubsystem` (the seam), which
  handles both Mass agents (fodder) and Actor elites.

## Build & run
<!-- TODO: fill in real paths once the engine build is in place. Full guide in SETUP.md. -->
- Open `RogueSmoke.uproject` with the **custom** editor you built (not a launcher engine).
- AngelScript: hot-reloads on save (non-structural changes reload live in PIE).
  Editor **Tools -> Open Angelscript workspace** opens `Script/` in VS Code.
- C++: rebuild in Visual Studio, relaunch editor.
- Multiplayer test: PIE with **2 players, Net Mode = Play As Listen Server** (see `TESTING.md`).

## House rules (highest signal — full detail in imports/docs)
- **AngelScript RPCs default to RELIABLE.** Mark cosmetic RPCs `Unreliable` explicitly.
- **Server authority:** only the server spawns replicated actors, applies damage, grants loot,
  or advances the run. Clients send intent via `Server_` RPCs; validate server-side.
- **Go through the seam:** combat queries/AoE/pull/mark go through `UCombatSubsystem`, never a
  direct world iteration in an ability.
- **Procgen must be deterministic:** all randomness through the seeded RNG stream. No unseeded
  random, no iteration-order or wall-clock dependence.
- **No Unreal Interfaces** (UInterface/IInterface) in AngelScript — use base classes/components/tags.
- Don't commit with broken script or C++ compilation.

## Project documentation map
- `Rogue_Smoke_GDD.md` — design intent (the "why" of the game).
- `Rogue_Smoke_MVP_Architecture.md` — **authoritative technical reference**; read before
  implementing combat, abilities, or Mass.
- `ARCHITECTURE.md` — system overview and authority model.
- `CODING_STANDARDS.md` — AngelScript conventions and networking patterns.
- `DECISIONS.md` — what's been decided and what's still open. Check before re-litigating.
- `GLOSSARY.md` — shared vocabulary.
- `SETUP.md` — engine build + project bring-up.
- `TESTING.md` — determinism + multiplayer testing.

## Detailed docs (auto-loaded)
@ARCHITECTURE.md
@CODING_STANDARDS.md
@GLOSSARY.md
