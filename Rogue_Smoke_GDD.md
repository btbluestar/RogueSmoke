# Rogue Smoke — Game Design Document

> **Status:** v0.1 — living document
> **Working title:** "Rogue Smoke" (placeholder, not final)
> **Purpose:** Single source of truth for design intent. Read this before implementing systems. When something here is marked **[DECISION NEEDED]** or **[TBD]**, ask before locking in an implementation rather than guessing.

---

## 1. Elevator Pitch

A co-op roguelike where 2–4 players drop into procedurally-built raids, fight through swarms of enemies, grab run-altering upgrades, complete an objective, and try to extract alive. The hook is **player synergy**: upgrades and abilities are designed to combine across teammates, so a coordinated team is far more than the sum of its parts.

---

## 2. Core Pillars

These are the load-bearing design values. Every feature should serve at least one. If a feature fights a pillar, flag it.

1. **Roguelike** — Runs are self-contained. Death (or a failed extraction) ends the run. Power comes from in-run upgrades, with chance and choice driving variety. Meta-progression is **[DECISION NEEDED]** (see §6.3).
2. **Co-op first** — Designed around 2–4 players playing together. Solo play is **[DECISION NEEDED]** — either unsupported, or supported via scaling/companion. The game should feel built for teammates, not single-player with co-op bolted on.
3. **Player synergies** — Abilities and upgrades are intentionally designed to combine across players (setup → payoff). This is the primary differentiator. See §5.
4. **Procedural levels** — Raids are assembled procedurally so layouts stay fresh. Marked "if possible" by the team; treat as a strong goal with a handcrafted fallback (see §8).
5. **A lot of enemies** — Combat is built around large enemy counts (swarms, waves, density). This is both a fantasy and a hard technical constraint that shapes engine/netcode choices (see §11).

---

## 3. Core Gameplay Loop

### 3.1 The Raid (moment-to-moment → run)

```
START RAID
   ↓
[1] Fight enemies        ← swarm combat, the core verb
   ↓
[2] Get upgrades         ← roguelike-style choices between/within encounters
   ↓
[3] Reach the raid goal  ← objective(s) that define the raid
   ↓
[4] Survive & extract    ← reach an exit / hold an extraction; failure = lose the run
   ↓
END RAID → (next raid OR run summary)
```

### 3.2 Loop layers

- **Combat loop (seconds):** target → position → use ability → trigger/enable a teammate's payoff → manage incoming damage.
- **Encounter loop (minutes):** clear a space → take an upgrade → push to the next space.
- **Raid loop (one session):** complete objective → extract → resolve rewards.
- **Run loop (one or more raids):** **[DECISION NEEDED]** — is a "run" a single raid, or a chain of escalating raids that ends on death/extraction? This materially changes pacing, difficulty curves, and how upgrades stack. Recommend deciding this early; it's upstream of almost everything else.
- **Meta loop (across runs):** **[DECISION NEEDED]** — unlocks, persistent currency, character progression, or pure run-to-run with cosmetics only.

### 3.3 Extraction

Extraction is the tension cap on every raid — it's the difference between "I survived" and "I won." Design space to decide on:
- **Trigger:** fixed exit point, called-in extraction with a timer/defend phase, or escalating-pressure exit ("the longer you stay, the worse it gets").
- **Risk/reward:** can players choose to push deeper for more loot at higher risk before extracting? (Strong roguelike + co-op tension generator — recommended.)
- **Failure state:** what's lost on a failed extraction — the whole run, or just the current raid's gains? Ties directly to the §3.2 run-structure decision.

---

## 4. Players & Co-op

- **Player count:** 2–4. **[DECISION NEEDED]** confirm max (3 vs 4 changes balance and netcode budget).
- **Roles/classes:** **[DECISION NEEDED]** — fixed classes, freeform builds, or hybrid. Synergy design (§5) leans toward **distinct kits** so combinations are legible, but freeform builds can also create emergent synergy. Recommend at least loosely-defined archetypes so the synergy fantasy reads clearly.
- **Shared vs individual progression:** **[DECISION NEEDED]** — are upgrades picked per-player, or are some team-wide? Per-player picks create build identity; team picks reinforce the co-op pillar. A mix is likely best (see §6.2).
- **Friendly fire:** **[DECISION NEEDED]** — off (more accessible, fewer grief vectors), or limited/positional (more tactical). Given "a lot of enemies" + AoE synergies, recommend **off** by default.
- **Down/revive system:** strongly recommended for a co-op roguelike — downed-but-revivable states create the cooperative drama the genre runs on.

---

## 5. Player Synergies (Signature System)

The headline feature. Synergies are abilities/upgrades that combine **across players** for an effect greater than either alone. Design them as **setup → payoff** pairs and chains.

### 5.1 Canonical example (from the brief)

> Player A uses a **taunt/pull** that clusters enemies together → Player B uses a **barrage/AoE** that devastates the bunched-up group.

This is the model: one player **creates a condition**, another **exploits it**.

### 5.2 Synergy taxonomy (design framework)

| Type | Setup (Player A) | Payoff (Player B) |
|------|------------------|-------------------|
| **Crowd control + AoE** | Pull / taunt / slow / root to cluster enemies | Barrage / bomb / nova that rewards density |
| **Mark + detonate** | Apply a "mark"/debuff to a target or group | Ability that consumes marks for bonus damage/effect |
| **Buff stacking** | Grant a buff (haste, damage, shield) to a teammate | Teammate's ability scales with the buff |
| **Resource sharing** | Generate ammo/energy/health drops | Teammate's kit converts the resource into power |
| **Zone control** | Create terrain/hazard (wall, fire, gravity well) | Push/knock enemies into the hazard |
| **Aggro management** | Tank holds enemy attention | DPS gets safe windows / backstab bonuses |

### 5.3 Design rules for synergies

- **Legibility:** a synergy should be *recognizable* in the moment — clear visual/audio language for "this is set up, hit it now."
- **Optional but rewarding:** uncoordinated play should still function; coordinated play should feel dramatically better. Never *require* a specific synergy to progress (it breaks if a player leaves/drops).
- **Upgrades should deepen synergies:** roguelike upgrades (§6) are a great place to introduce, strengthen, or unlock new synergy interactions mid-run — this marries the two signature pillars.
- **Self-synergy:** a solo player or a single kit should have *some* internal setup→payoff so the system isn't dead-on-arrival without perfect coordination.

---

## 6. Roguelike Systems

### 6.1 Upgrade acquisition

Upgrades are offered between/within encounters (post-fight, on objective completion, from elites/chests). Standard roguelike presentation: choose 1 of N, with rarity tiers. Pacing tied to the §3.2 run-structure decision.

### 6.2 Upgrade scope

- **Personal upgrades:** modify the picking player's kit/stats/abilities → build identity.
- **Team upgrades:** affect the whole squad → reinforce co-op pillar.
- **Synergy upgrades:** unlock or strengthen cross-player interactions (§5.2) → the special sauce. **Prioritize having these exist by first playable.**
- **[DECISION NEEDED]:** ratio/availability of each type, and whether players see/share an upgrade pool or draw independently.

### 6.3 Meta-progression **[DECISION NEEDED]**

Pick a philosophy early; it shapes scope heavily:
- **(A) Pure roguelike:** no persistent power; only cosmetics/unlocked content. Easiest to balance, most "fair," least retention pull.
- **(B) Roguelite:** persistent currency → permanent unlocks/upgrades between runs. Strong retention, more balancing surface.
- **(C) Character progression:** persistent levels/skill trees per character/class.
- **Recommendation for MVP:** ship the in-run roguelike loop first (A's mechanics) and defer the meta decision until the core loop is fun. Don't build a meta layer on top of an unproven core.

---

## 7. Combat & Enemies

- **Core verb:** combat against large numbers of enemies. Feel target: powerful, kinetic, readable even when the screen is full.
- **Enemy design:** mostly **swarm/fodder** units (cheap, numerous) plus **specials/elites** (introduce decisions and create synergy setups, e.g. a unit worth taunting/clustering) plus **mini-bosses/bosses** as raid-goal anchors.
- **Density as a mechanic:** since clustering enemies is a synergy enabler (§5.1), enemy grouping/flocking behavior is a *design feature*, not just AI flavor.
- **Telegraphing:** with high enemy counts and AoE flying around, clear telegraphs and a strong readability pass are essential.
- **[TBD]:** enemy faction/theme, damage model (hitscan vs projectile vs melee mix), and how aggro is distributed across 2–4 players.

---

## 8. Procedural Generation

Goal: raids are assembled procedurally so layouts and encounters stay fresh. Treat as a tiered goal so the project isn't blocked on it:

- **Tier 1 (fallback):** handcrafted rooms/arenas with **procedural enemy spawns, upgrade placement, and objective placement.** Cheapest path to "every run feels different" without full level gen.
- **Tier 2 (target):** **room/module-based generation** — handcrafted chunks stitched procedurally (the proven approach for most action roguelikes: controllable quality + variety).
- **Tier 3 (stretch):** fully procedural terrain/layout. High risk, high cost, easy to produce incoherent or unfun spaces. Only pursue if Tier 2 proves insufficient.
- **Co-op constraint:** generation must be **deterministic / authoritative** so all clients share an identical layout (see §11). Seed-based generation on the host (or a shared seed) is the standard solution.
- **Recommendation:** build Tier 1 first, design content as modular chunks from day one so Tier 2 is a natural evolution, not a rewrite.

---

## 9. Camera & Perspective **[DECIDED — third-person shooter (D-0014)]**

**Decision: third-person over-shoulder *shooter*, strafe/aim-locked.** Camera sits on a short spring
arm behind the character with a shoulder offset and uses pawn control rotation; the character faces
the aim/control rotation and moves strafe-relative (Gears/Lyra-style), not orient-to-movement.
Built from the engine fork's `TP_ThirdPerson` template, adjusted to aim-locked strafing. See
**DECISIONS.md D-0014**.

The options below are retained for context on the trade-off we accepted:

| Option | Fits "lots of enemies"? | Co-op readability | Notes |
|--------|------------------------|-------------------|-------|
| **Top-down / twin-stick** | Excellent | Excellent (whole arena visible) | Classic for swarm + co-op roguelikes; easiest to read density & synergies. Was the prior default (D-0005). |
| **Isometric / 3-4 angle** | Very good | Very good | Stylish, good spatial read; more art cost. |
| **3rd person over-shoulder** | Good | Harder (each player sees differently) | **Chosen (D-0014).** More immersive/action-y; readability of swarms + teammate setups is harder — mitigated deliberately (§10). |
| **First person** | Risky | Hard | Swarm readability and "see your teammate's setup" both suffer; not recommended for this concept. |
| **Side-scroller 2D/2.5D** | Good | Good | Constrains the synergy/positioning space to 1 axis. |

**Consequence to protect:** third person trades away the overhead read that made swarm density and
cross-player setups legible for free. Readability is now an active design cost, not a freebie —
lean hard on the §10 readability pass (silhouettes, edge/off-screen indicators for teammates and
clustered enemies, AoE ground telegraphs, audio callouts). Validate the taunt→barrage combo (§12)
still reads at speed in this view before scaling content.

### 9.1 Movement & traversal **[DECIDED — AngelScript MVP (D-0015)]**

Movement is a first-class part of the feel, drawing on **Apex Legends / Deadlock**: fluid, momentum-y,
expressive. The MVP kit:

- **Sprint** — hold to move faster in any direction (omnidirectional, fits the strafe-aim camera).
- **Crouch** — hold to crouch-walk (smaller silhouette, slower).
- **Slide** — crouch while sprinting to slide: a momentum burst that carries on low friction and
  decays, with downhill speed-up on slopes. The core skill-expression move.
- **Jump + double-jump** — a second air jump for repositioning and dodging.

**Meta-progression ties directly into movement** (scope still open, §6.3): upgrades are expected to
tune **move speed, jump height, jump count (e.g. unlock the double-jump), and slide distance/friction**.
The implementation keeps each of these as a tunable knob so an upgrade can modify it without a rewrite
— but no movement upgrades are wired yet. See **DECISIONS.md D-0015** for the technical approach
(AngelScript on stock CharacterMovement, server-authoritative, idempotent slide impulse).

---

## 10. Art, Audio & Tone **[TBD]**

- **Theme/setting:** undecided. (The "Smoke" in the title suggests a possible aesthetic anchor — gunpowder, industrial, noir, or literal smoke/obscurement mechanics. Worth exploring whether smoke is *mechanical* as well as thematic.)
- **Art direction:** must prioritize **readability at high entity counts** — strong silhouettes, restrained background noise, clear friend/enemy/hazard color language.
- **Audio:** critical for synergy callouts and for legibility when the screen is full — distinct audio cues for "setup ready" and "payoff landed."

---

## 11. Technical Considerations (read before architecting)

These constraints are unusually important because **co-op + lots of enemies + procedural** is a demanding combination.

### 11.1 Networking (the hard part)
- Co-op with high enemy counts is **network-heavy.** Decide the model early:
  - **Host/authoritative server (recommended):** one machine simulates enemies/world; clients send input, receive state. Simplest correctness for "lots of enemies." Maps cleanly onto UE's **listen-server / dedicated-server** model and the built-in Actor replication + RPC system — use the engine's networking rather than rolling your own.
  - Avoid lockstep/deterministic-sim approaches unless the team has prior netcode depth — they're brittle with large dynamic entity counts.
- **Entity replication budget** is the gating constraint. Hundreds of enemies × 2–4 clients = aggressive need for **net relevancy / interest management**, replication graph, property/state compression, and `NetUpdateFrequency` tuning per enemy class. Design enemy behaviors to be cheap to replicate (favor replicating compact state over many fine-grained properties).
- AngelScript can author replicated gameplay, but keep the **replication-heavy enemy path in C++/Mass** (§11.3) and expose results to script.
- **Procedural gen must be shared/deterministic** across clients: generate from a **host-chosen seed** replicated to clients, so everyone builds the identical layout (see §8). Don't stream generated geometry over the wire — share the seed, run the same generator.

### 11.2 Performance
- "A lot of enemies" implies: pooling, spatial partitioning, batched/instanced rendering, and data-oriented design for enemy simulation.
- **In UE specifically, evaluate the Mass Entity framework (MassEntity / MassAI) for the swarm.** It's UE's built-in ECS-style system purpose-built for large agent counts (crowds, swarms) and is the natural answer to this pillar — far cheaper than thousands of full `AActor`/`ACharacter` instances each with their own tick and AIController. Use lightweight Mass agents for fodder; reserve full Actors for elites/bosses that need rich behavior. **[ACTION: spike Mass vs Actor-pool early to validate the enemy-count target.]**
- Rendering: prefer instanced/Niagara-driven or ISM/HISM representations for fodder over individual skeletal meshes where the silhouette still reads (ties to the readability pass in §7/§10).
- Set an **enemy-count target early** (e.g. "N concurrent on screen at stable frame rate on target hardware") — it's a design constraint, not just an optimization goal. This number drives the Mass-vs-Actor decision and the replication budget (§11.1).

### 11.3 Engine — **DECIDED: Unreal Engine 5.7 + AngelScript fork**

**Engine:** Unreal Engine 5.7 (current stable, released Nov 2025).
**Scripting:** Hazelight's **UnrealEngine-Angelscript** fork (`github.com/Hazelight/UnrealEngine-Angelscript`) — AngelScript as the primary gameplay language alongside C++.

**Why this is a good fit:**
- AngelScript is battle-tested at scale — Hazelight shipped *It Takes Two* and *Split Fiction* (1.7M+ lines of AngelScript) with the majority of gameplay in it. Mature for a real production, not a toy.
- **Hot-reload** is the headline win: non-structural script changes reload live during Play-In-Editor without exiting the session or recompiling C++. For a roguelike with many upgrades/synergies to tune (§5, §6), that iteration speed is a direct multiplier on how fast the team can find the fun.
- First-class VS Code tooling (LSP autocomplete, error-on-save, breakpoint debugging) via the `vscode-unreal-angelscript` extension. A JetBrains/Rider plugin also exists.

**Constraints the team must plan for (call these out before architecting):**
- **It's an engine-source fork, not a drop-in plugin.** The engine must be built from source with the fork applied. Budget for a source-build pipeline and CI from day one.
- **Have an engine programmer available.** Hazelight provides no support guarantee and explicitly recommends a C++/engine programmer on hand for commercial use. Treat the fork as a dependency the team owns, not a black box.
- **Confirm a UE 5.7 branch of the fork exists and is stable before the whole team commits.** The fork tracks engine releases and can lag a given UE version; verify 5.7 support (or pin to the latest fork-supported UE version) rather than assuming. **[ACTION: verify fork ↔ 5.7 compatibility.]**
- Note the vanilla-plugin reimplementations (e.g. community `UE-Angelscript` / `UNREANGEL` forks) exist but lag engine versions and feature parity — the Hazelight source fork is the reference target here.

**C++ vs AngelScript division of labour (important given the "lots of enemies" pillar):**
- **AngelScript:** gameplay logic, abilities, synergies, upgrade effects, objective/extraction flow, UI glue. Anything iterated frequently. This is where the hot-reload payoff lives.
- **C++ (and/or Mass — see §11.2):** the hot path. Enemy simulation at high counts, spawning/pooling, spatial queries, replication-heavy systems. Don't run hundreds of per-frame enemy ticks through script; expose tuned C++/Mass systems to AngelScript via clean interfaces.
- Rule of thumb: **script the decisions, compile the simulation.**

### 11.4 Target platform(s) **[TBD]**
- PC, console, both? Affects input scheme, perf budget, and online infrastructure. Note the camera is
  now a third-person **shooter** (D-0014, §9): mouse-aim is a first-class path, so input is no longer
  controller-first by default — support mouse/keyboard aim and gamepad aim equally.

---

## 12. MVP / First Playable Scope

Prove the fun before building breadth. Suggested vertical slice:

- [ ] 2-player co-op, host-authoritative networking
- [ ] One arena (handcrafted) with procedural enemy spawns (Tier 1 procgen)
- [ ] One swarm enemy type + one elite/"clusterable" enemy
- [ ] Two player kits that form **one working synergy** (e.g. taunt/pull + AoE barrage)
- [ ] Roguelike upgrade screen with ~6–10 upgrades, including at least **one synergy upgrade**
- [ ] One raid goal + a basic extraction
- [ ] Down/revive

**Success criterion:** is coordinated, synergy-driven co-op combat *fun* with this minimal content? If yes, expand. If no, the rest doesn't matter yet.

---

## 13. Open Decisions (consolidated)

Track and resolve these — most are upstream of implementation:

1. **Run structure** — single raid vs chained escalating raids. *(§3.2 — highest priority)*
2. ~~**Camera/perspective.**~~ **Resolved → third-person shooter, strafe/aim-locked (D-0014).** *(§9)*
3. **Meta-progression model** — pure roguelike / roguelite / character progression. *(§6.3)*
4. **Class system** — fixed kits / freeform / hybrid. *(§4)*
5. **Upgrade scope mix** — personal / team / synergy ratio & shared vs independent draws. *(§6.2)*
6. **Player count max** — 3 vs 4. *(§4)*
7. **Solo support** — yes/no/scaled. *(§2, §4)*
8. **Friendly fire** — on/off/positional. *(§4)*
9. **Extraction design** — trigger, risk/reward, failure cost. *(§3.3)*
10. **Target platform(s)** — PC / console / both. *(Engine is decided: UE 5.7 + AngelScript fork, §11.3. Platform still open, §11.4.)*
11. **Theme/setting & whether "smoke" is mechanical.** *(§10)*
12. **Enemy-count target on target hardware.** *(§11.2)*

---

## 14. Notes for Claude Code

- This is a **design reference**, not a spec — it states *intent*. When intent is ambiguous or marked **[DECISION NEEDED]/[TBD]**, surface the question rather than assuming.
- The two non-negotiable pillars to protect in any implementation are **cross-player synergy legibility** (§5.3) and **performance/readability at high enemy counts** (§7, §11).
- Sequence implementation around the **MVP vertical slice** (§12) before building breadth.
- **Stack is UE 5.7 + Hazelight AngelScript fork (§11.3).** Default to writing gameplay (abilities, synergies, upgrade effects, objective/extraction flow, UI glue) in **AngelScript** for hot-reload iteration. Keep the **enemy-simulation hot path and replication-heavy systems in C++/Mass** and expose them to AngelScript via clean interfaces — "script the decisions, compile the simulation." Don't put per-frame, per-enemy logic in script at high counts.
