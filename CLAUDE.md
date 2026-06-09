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
- **AngelScript engine fork:** `F:\UEAS` (editor: `F:\UEAS\Engine\Binaries\Win64\UnrealEditor.exe`).
- Open `RogueSmoke.uproject` (in `RogueSmoke/RogueSmoke/`) with that **custom** editor — not a launcher engine.
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

## Subagents & parallelism (superpowers)
**There is ONE editor.** The build, `run_code_test`, and all PIE/MCP tools drive a single
stateful Unreal instance — so anything that touches it MUST be serialized on the main session.
- **Parallelize (safe, no shared state):** read-only codebase/doc research (`Explore`), and
  *authoring* independent `.as`/`.cpp` files whose only verification is a later central compile.
- **Never parallelize (shared editor = agents fight):** `ue-cpp build` (bounces the editor),
  `as-helper run_code_test` (spawns its own editor-cmd; concurrent runs contend), and any
  `unreal-test-mcp` PIE/actor/python call. One driver at a time.
- **No git worktrees for editor work:** each worktree would need its own multi-GB engine build +
  editor; the isolation isn't worth it. Branch in-place instead.
- Pattern: fan out research/authoring to agents → funnel **all** build/test/PIE verification back
  through this session, sequentially.

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
