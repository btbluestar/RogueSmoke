# Architecture

> Working title: **[ProjectName]** — a co-op roguelike built in Unreal Engine 5 with
> [UnrealEngine-Angelscript](https://angelscript.hazelight.se/) (Hazelight fork).
> This document describes *what* the systems are and *why* they are shaped this way.
> For *how* to write the code, see `CODING_STANDARDS.md`.

## 1. Pillars & constraints

These drive most architectural decisions. Revisit them before adding a system.

- **Co-op first.** Every gameplay system must be designed network-aware from day one.
  Retrofitting replication onto single-player code is the most expensive mistake in this genre.
- **Run-based.** The game is structured around *runs*: short, self-contained sessions that
  end in victory or death, with persistent meta-progression between them.
- **Procedural, but deterministic.** Levels, loot, and events are generated from a seed.
  Given the same seed and inputs, generation must produce the same world on every machine.
- **Designer-iterable.** Tunable content (items, enemies, rooms) lives in data assets, not
  hardcoded in script, so balance changes don't require code changes.

## 2. Networking model

**Topology: listen server (host is also a player).** One player hosts; others join.
This is the standard for the genre (Risk of Rain 2, Vampire Survivors co-op, etc.) and is the
default assumption throughout this doc. A dedicated-server build is a later option and mostly
changes deployment, not the authority rules below.

> **If you instead want dedicated servers, flag it now** — it affects whether host-side
> player logic can assume a local pawn.

**Authority rules (non-negotiable):**

- **The server is authoritative for all game state.** Spawning replicated actors, applying
  damage, granting loot, advancing the run, and generating the world all happen on the server.
- **Clients predict/cosmetic only.** Clients may run cosmetic effects locally and send intent
  to the server via `Server` RPCs, but never decide outcomes.
- **Only the server spawns replicated actors.** A client-spawned actor exists only on that client.

**What lives where:**

| Object            | Exists on        | Authoritative for                                  |
|-------------------|------------------|----------------------------------------------------|
| `GameInstance`    | each machine     | Process-lifetime data, session/connection handling |
| `GameMode`        | **server only**  | Run rules, spawning, win/lose, level transitions   |
| `GameState`       | server + clients | Replicated run state (floor #, seed, shared score) |
| `PlayerController`| owning machine   | Input, player-specific UI, client RPCs             |
| `PlayerState`     | server + clients | Per-player replicated data (health, build, currency)|
| `Character`/`Pawn`| server + clients | Movement (with client prediction), abilities       |

See `CODING_STANDARDS.md` for the concrete AngelScript replication syntax and patterns.

## 3. Run lifecycle

```
Main Menu
   └─> Lobby (host + joiners, character/loadout select)
        └─> Run Start  (server rolls master seed, broadcasts via GameState)
             └─> Floor Loop  ── repeat per floor ──┐
                  ├─ Generate floor (server, seeded)
                  ├─ Room loop (combat / shop / event rooms)
                  ├─ Floor boss
                  └─ Descend / portal ─────────────┘
                       └─> Victory  OR  Party Wipe
                            └─> Results + meta-progression payout
                                 └─> back to Main Menu / Lobby
```

Key owner: a **RunManager** (a server-only subsystem or a `GameMode`-owned object) drives
this state machine and is the single source of truth for "where are we in the run."

## 4. Core systems

### 4.1 Run / floor generation
- A server-side **RunManager** holds the master seed and current run state.
- A **LevelGenerator** consumes the seed to lay out floors and rooms. It must be deterministic:
  no `Math::Rand()` without a seeded stream, no dependence on actor iteration order, no wall-clock.
- The seed is replicated through `GameState` so late-joiners and clients can reproduce
  cosmetic/deterministic decisions without the server streaming every detail.

### 4.2 Player character & abilities
- A base **Character** (movement, health hooks) subclassed per playable hero.
- Abilities are **GAS** GameplayAbilities (cooldown/cost/effect/replication via the system).
  Activation flows client intent → `Server` RPC → server activates the granted ability → results
  replicate back.
- The project uses **Unreal's Gameplay Ability System**, copying Lyra's patterns, via the engine
  fork's **AngelscriptGAS** plugin (so abilities/granting stay in AngelScript). The ASC lives on the
  PlayerState; player stats are GAS AttributeSets; upgrades are GameplayEffects. See **D-0013**.

### 4.3 Items, loot & inventory
- Items defined as **DataAssets** (stats, rarity, tags, visuals). Never hardcode item stats.
- Loot tables (DataTable or DataAsset) rolled **server-side** from the seeded stream.
- Inventory/build state lives on `PlayerState` (replicated) so other players' builds are visible.
- Stacking/synergy effects compose via a tag-driven modifier system rather than bespoke per-item code.

### 4.4 Enemies & AI
- Enemy archetypes as DataAssets + a base enemy Character/Pawn.
- AI (behavior trees or a simple state machine) runs **server-side only**; clients see replicated
  transforms/animation state. Do not run AI logic on clients.
- Spawning/wave logic owned by the floor/room manager, seeded for reproducibility.

### 4.5 Meta-progression & saving
- Persistent unlocks (heroes, items added to the pool, currency) saved **between runs**, not mid-run.
- Save is local to the host for co-op meta, *or* per-player local for individual unlocks — decide
  this explicitly (it changes whether unlocks are shared in a session).
- Use Unreal `SaveGame` objects; keep the schema versioned so saves survive content updates.

## 5. Data architecture

- **DataAsset** for typed, designer-authored definitions (heroes, items, enemies, rooms).
- **DataTable** for tabular/balance data (loot weights, stat curves).
- **GameplayTags** for composable categorization (damage types, item synergies, status effects).
- Script reads data; designers edit data. The boundary is deliberate.

## 6. Module / folder map

Scripts live under `Project/Script/` (auto-loaded by the plugin). Suggested top-level layout:

```
Script/
  Core/         RunManager, GameMode, GameState, GameInstance subsystems
  Player/       Character, controllers, input
  AbilitySystem/ GAS abilities, AbilitySet/InputConfig (Attributes/PlayerState/HeroBase are C++)
  Abilities/    Ability definitions and effects
  Items/        Item data, inventory, modifier system
  Enemies/      Enemy pawns, AI, spawning
  Generation/   Seeded RNG, level/room generation
  UI/           HUD logic, menu/lobby controllers (UMG widgets stay as assets)
  Data/         Shared structs, enums, gameplay tag helpers
  Tests/        Script tests
```

Blueprints subclass the relevant script classes for asset assignment (mesh, particles, etc.),
prefixed `BP_`. Keep logic in script; keep content in Blueprints/DataAssets.

## 7. Open decisions (resolve early)

1. **Networking topology** — listen server (assumed) vs dedicated server.
2. **Ability framework** — custom component (faster) vs GAS (heavier, more capable).
3. **Meta-progression scope** — shared per-session vs per-player persistent unlocks.
4. **Max party size** — affects spawn budgets, scaling curves, and UI layout.
5. **Late join / drop-in** — supported, or run-locked once started?
