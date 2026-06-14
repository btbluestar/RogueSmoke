# Procedural Raid Generation — Design Spec

> Status: **Design approved (brainstorming)** — 2026-06-14. Branch `ProcedualLevelGeneration`.
> Next step: implementation plan (writing-plans). This is the *what/why*; the plan is the *how/when*.
> Theme flavor: **QUARANTINE RUN** (IBS game-show, void-gates) — see `GameSetting.md`. The
> concept doc's ASCII layout is **flavor, not a blueprint** (per design call 2026-06-14).

## 1. Goal

Generate, per run, a **compact co-op mission** — not a single static arena. Each raid is a small
procedurally-generated level: a shared drop-in, **2–3 main objective sites** (each a typed task in a
combat space), an optional secondary or two, and a **separate extraction site**, all under a
**raid director that escalates from a gentle trickle to a relentless tide** over ~12 minutes.

Hard constraints inherited from the project:
- **Deterministic from one master seed** (D-0007): identical world on host + clients; we replicate the
  seed, never stream geometry. No unseeded random, no iteration-order / wall-clock dependence.
- **Server-authoritative** (D-0004): the server generates + validates; only the server spawns
  replicated actors, advances the run, applies damage/loot.
- **Script the decisions, compile the simulation** (D-0002): layout *decisions* in AngelScript; the
  ISM stamping + PCG-driving *seam* in C++. Designer-tunable content in DataAssets, no logic.
- **Make heavy use of UE 5.7 tooling, especially PCG** — satisfied by the two-layer split (§7): PCG
  owns the entire cosmetic skin; it never owns anything gameplay reads.

## 2. References & research grounding

Five research briefings informed this design (full source lists in §15). The load-bearing findings:

- **PCG is *not* network-deterministic.** Float + async-ordering diverge across machines; Epic's own
  guidance is "replicate the seed, generate locally." → structural geometry must be **seeded
  `FRandomStream` + modular stamping we own**; PCG is **cosmetic only**. (Determinism briefing.)
- **Shipping roguelikes are "authored shell + procedural fill."** RoR2, Hades, Returnal, Warframe all
  handcraft the load-bearing space and randomize selection/spawns/props. Nobody random-generates
  fight geometry from scratch except via strict tile/socket grammars. (Arena-archetypes briefing.)
- **A 15-point fairness/playability checklist** for swarm + high-mobility arenas (escape-proof bounds
  vs the slide-hop kit, no infinite-kite topology, contestable high ground, etc.) — distilled into the
  **design invariants** every generated space must pass (§9). (Level-design briefing.)
- **The mission loop** = LoL:Swarm time-escalation × Helldivers objective-then-separate-extraction,
  scaled to a ~12-min compact level with 2–3 all-visible objectives and a 3-spike escalation curve.
  (Swarm/Helldivers briefing.)

## 3. Decisions made during brainstorming (the locked picks)

| # | Decision | Choice | Why |
|---|----------|--------|-----|
| 1 | Degree of procedurality | **Slot-template hybrid** (model B), expandable toward C later | Fair/learnable + safe for determinism; the shipping consensus |
| 2 | Seed dials in MVP scope | Module swap, reposition-within-zone, blue-noise scatter, void-gate count/placement, theme roll | High freshness, low fairness risk |
| 2b | Later dials | Multiple archetypes (dial 6, Phase 3), varying footprint (dial 7, Phase 4) | Architecture accepts them without rewrite |
| 3 | Tech split | **Owned-deterministic structure + PCG cosmetic skin** | Both: heavy PCG use *and* determinism safe |
| 4 | Anchor archetype | **Skatepark**, built as authored shell + procedural fill | Best showcase of the *whole* game (movement **and** combat), retires the scariest validation early |
| 5 | Party / spawn | **1–4 players, shared drop-pod**; brawl radius scales with headcount | Sidesteps co-op spawn asymmetry |
| 6 | Void-gate role | **Spawn + atmosphere only** (objective is *not* about gates) | Lowest new work; builds on existing `RaidObjective` |
| 7 | Raid end / extraction | **Hybrid (C)**: objectives gate extraction; tide climbs regardless; mini-boss spike at the call | Helldivers structure + Swarm feel; fits existing clear→upgrade→extract bones |
| 8 | Length / objective count | **~12 min; 2–3 main objectives, all-visible** (squad picks order); +1–2 optional | Grounded in Swarm/Helldivers/DRG precedent |
| 9 | Objective task pool | Hold-and-channel, Activate-and-defend, Collect-and-deposit, Destroy-structure (+Eliminate optional); **escort excluded** | Best fits for a small level + mobility kit |
| 10 | MVP objective | **Hold-and-channel** | Simplest defensible crucible; exercises swarm + slide repositioning + a clean spawn-spike |

## 4. The raid loop (~12 min, hybrid)

```
DROP-IN (0:00)        shared drop-pod; gentle fodder trickle; traverse to first site.
   │   ── escalation spine: time-based per-~30s wave step, scaled by player count ──
OBJECTIVE PHASE (~0:30–8:00)
   │   2–3 main objectives, ALL VISIBLE, squad picks order.
   │   • ELITE INJECTION #1 (~3:00 shoulder)
   │   • each objective complete → spawn SPIKE → 20–40s RELAX lull (= upgrade-pick pause)
   │   • ELITE INJECTION #2 (~6:30 pre-climax)
MINI-BOSS / CLIMAX SPIKE (~8:00)   drops the upgrade chest where it falls (existing AUpgradeChest).
EXTRACTION REVEAL & ARM (~8:30)    beacon at a SEPARATE, ~opposite site — only now revealed.
EXTRACTION HOLD (~9:00–11:00)      call it → ~90–120s countdown → defend the LZ under flood spawns;
   │                               ≥1 player must stay on the pad. Board to win.
VICTORY   (or party-wipe → results + meta payout)
```

Escalation = **time-based spine** (Swarm/VS legibility: your power curve races the tide) **+ 3
designer spikes** (elite-1, mini-boss, extraction-flood) **each followed by a relax lull** (L4D
anti-fatigue). The lulls are the shared-XP upgrade pauses you already have. Numbers are tuning
hypotheses, not constants.

## 5. Mission structure & what the generator produces

The generator emits an **`FRaidLayout`** (data) that the rest of the systems consume:

- **Drop node** — low-threat shared insertion point.
- **Extraction node** — a *separate*, ~opposite site; hidden/inert until mains complete.
- **2–3 main objective sites** — each typed (§6) and shaped as one **archetype** combat space (§8).
- **1–2 optional secondary / POI sites** — replay variety + an upgrade-tempo lever.
- **Connectivity** — a small *graph* (not a corridor): multiple routes between sites so order is a
  real choice and the mobility kit has lateral/vertical options. Spacing ≈ **10–15 s traversal** at
  slide/double-jump speed between adjacent sites.
- **Void-gate placements** — the Maw (central spawner per site/level), ambient gates (cosmetic),
  breach-gate anchors (dynamic escalation).
- **Spawn-director metadata** — per-site **intensity caps** + the escalation curve params (baseline
  per-time budget, the two elite-injection timestamps/sites, per-objective spike triggers, the
  extraction-flood profile). The director reads this; it is **not** hardcoded.

## 6. Objective task-types (the pool)

The generator rolls 2–3 mains per raid from this pool; each declares its layout need.

| Task | Player does | Layout need | In MVP? |
|------|-------------|-------------|---------|
| **Hold-and-channel** | Stand in a node's radius until a bar fills | One defensible point: cover + sightlines | **Yes (anchor)** |
| **Activate-and-defend** | Hit terminal(s) → survive the triggered wave | 1+ interaction nodes + retreat lanes | Phase 2 |
| **Collect-and-deposit** | Gather N scattered cells → deposit at a hub | Central hub + surrounding scatter spawns | Phase 2 |
| **Destroy-structure** | Blow 2–3 nests/relays | Multiple discrete reachable structures | Phase 2 |
| **Eliminate-target** (optional) | Kill a roaming elite | Reuses the elite system; any space | Phase 3 |
| ~~Escort-payload~~ | — | hand-routed lane, vehicle pace | **Excluded** |

Survive-timer is **not** a standalone main — it is the extraction-hold capstone.

## 7. Tech split — two layers, one seed

```
master seed (RunManager)
   └─► LAYER 1  STRUCTURE  (server-owned, deterministic)
        seeded FRandomStream + modular ISM/actor stamping via a C++ seam, AS orchestration.
        Owns everything gameplay & navmesh read:
          bounds + diegetic boundary collision · archetype/slot layout · module & catwalk graph ·
          the Maw, spawn nodes, HoldAnchor, objective nodes, extraction pad ·
          cover monolith positions (they block bullets → structural) ·
          validation + reroll (server-side).
        Replicated to clients as the SEED ONLY; clients reproduce structure locally.
   └─► LAYER 2  SKIN  (PCG, local/cosmetic, same seed + chosen theme)
        debris/greeble/"placed-over" trim · ambient void-gate VFX · crowd silhouettes, billboards,
        camera-drones, neon · surface scatter sampling Layer-1 modules · theme/relight roll.
        A client divergence here costs at most a misplaced prop — never gameplay.
```

PCG is driven from the C++ seam: set component Seed + graph user-parameters from the layout/theme,
wait for `IsGenerating()==false`, then `GenerateLocal(true)` (synchronous, behind the raid-load
screen — avoids PCG's async-ordering nondeterminism). Use `GenerateOnDemand`, **not**
`GenerateAtRuntime`/Runtime-Hierarchical (those are streaming-open-world features). Verify the PCG
API's AngelScript exposure in `F:\UEAS` 5.7 source; add a thin `ScriptCallable` wrapper if absent.

## 8. The 5-layer design grammar (the generator's spine)

Each layer is one deterministic, seeded pass.

- **A · Archetype** — roll the macro-shape of each objective site from a weighted, seeded pool. Sets
  the legal slot set + intensity profile. *Rule: the type vocabulary is constant across runs so player
  skill transfers.*
- **B · Slots** — fixed typed roles per archetype: `Entrance`, `CombatCore` (the Maw), `Flank/Loop`,
  `HighGround`, `HoldAnchor`, `Exit`. Each carries an **intensity cap** the director reads. *Rule:
  exactly one `CombatCore` and one `HoldAnchor`; `HoldAnchor` is where the mini-boss falls.*
- **C · Modules** — fill each slot from a typed pool; pieces connect only via **matching sockets**
  (Warframe rule), with per-archetype caps on count/branch-depth. *Rule: forgettable space =
  procedural, identity space = authored set-piece.*
- **D · Scatter** — seeded blue-noise (Poisson-disk) cover, props, spawn nodes, the chest; toggle big
  features on/off. *Rule: ALL randomness through the one seeded stream.*
- **E · Theme** — visual skin + the diegetic holographic boundary (the IBS "placed-over" look).
  *Rule: cosmetic ONLY — never changes collision, sightlines, or spawn logic.*

**Archetype pool (5):** Combat Bowl · **Skatepark (anchor)** · Figure-8 · Tiered Pit · Open Sprawl.
At MVP only Skatepark exists; the rest are Phase 3. (Archetypes are now the shapes of *objective
sites*, not whole levels.)

## 9. Design invariants (validation battery + reroll)

Every generated space is validated server-side; failure advances the seed deterministically and
re-rolls the offending stage (capped retries, then fall back to a known-good authored layout so a
run never softlocks). Rebuild navmesh **after** placement, **before** validation.

1. **Escape-proof bounds** — sim the fastest kit line (slide-hop off the highest point + double-jump)
   against every boundary; zero out-of-bounds reachable.
2. **Full reachability both sides** — BFS/flood-fill: every floor tile reachable by players *and* by
   enemy pathing (or covered by a ranged-elite sightline). No enemy-unreachable safe spot.
3. **One continuous traversal loop, no terminal dead-end** long enough to enable infinite kiting.
4. **Central, visible, contestable Maw** — ≥3 sightlines onto it; min spawn-distance from any player.
5. **3–4 chokepoints, none coverable from a single position.**
6. **Max 3 floor planes**; each reachable by ramp/pad/jump (player) and pad/ramp/ranged-coverage (enemy).
7. **Every high-ground tile threatened** by ≥1 ranged-elite position or artillery zone (no roof camp).
8. **Low-cover bias** in brawl zones; full cover only at sightline-breaks/revive pockets; cap density.
9. **Cover = blue-noise** (min separation, max gap).
10. **Extraction LZ:** 3–4 approach lanes, none coverable from one spot; good-but-imperfect cover;
    inside the loop, not a dead end; ≥1 open/high threat angle (defensible, not turtle).
11. **4–6 nameable zones**, power position offset from dead center.
12. **Co-op spread:** extraction lanes need ≥2 mutually-supporting positions to cover.
13. **Telegraph-clear floor** (no neon-on-neon camouflage of ground danger zones).
14. **Revive feasibility** near likely down-locations.
15. **Stable grammar, varied specifics** — vary which/where, hold zone *types/roles* constant.

The two highest-risk, project-specific invariants are **#1 (escape-proof vs slide-hop)** and **#3
(no infinite-kite)** — both consequences of pairing a fast kit with a swarm. The slide/double-jump
**reach envelope** (from D-0021 numbers) is the single source of truth for both #1 and #6; keep it in
one place so movement re-tuning can't silently break validation.

## 10. The void-gate system (spawn + atmosphere)

- **The Maw** — central rift per combat core; primary swarm source + the landmark; telegraphed,
  contestable. *Drives layout* (placed first; everything else relative).
- **Ambient gates** — smaller gates scattered around/under the rim. *Mostly cosmetic* (PCG, Layer 2);
  a few can flare to spit a flanking pocket but never gate the objective. Dial #4.
- **Breach gates** — open mid-fight to escalate; the director's *spatial voice*. Telegraphed, spawn
  at fair distance, then collapse. (Phase 2.)

The "placed over an area with void-gates" read is a cheap material/skybox trick: deck cutouts +
see-through grates + emissive void below, with the diegetic holographic boundary licensing the whole
modular, reconfigurable arena.

## 11. Component architecture (units, each with one purpose)

Decisions in AngelScript (`Script/Generation/`, new); ISM/PCG seam in C++ (`Source/RogueSmoke/`).

- **`URaidLevelGenerator`** (AS, server) — orchestrates the 5-layer pipeline from the `RunManager`
  seed; produces an `FRaidLayout`; runs validation + reroll. *Depends on:* archetype/module/theme
  DataAssets, the validator, the C++ stamp seam. *Interface:* `Generate(seed) -> FRaidLayout`.
- **`FRaidLayout`** (struct) — the produced data: sites (typed), node positions (drop / Maw /
  HoldAnchor / objective / extraction / spawn), module placements, connectivity graph, void-gate
  anchors, per-site intensity caps + escalation metadata. The contract between generation and the
  rest of the game.
- **C++ stamp seam** (`URaidStampSubsystem` or equivalent) — stamps modular meshes/actors (ISM/HISM)
  from `FRaidLayout`; drives the Layer-2 PCG component (seed + user-params → `GenerateLocal`). The
  "compile the simulation" half. *Interface:* `Stamp(const FRaidLayout&)`, `DecorateCosmetic(seed, theme)`.
- **`URaidLayoutValidator`** (AS or C++) — the §9 battery: navmesh connectivity, jump-reachability
  (uses the D-0021 reach envelope), escape-proof, sightline, cover, LZ checks. *Interface:*
  `Validate(const FRaidLayout&) -> {ok, failedStage}`.
- **DataAssets** (designer-authored, no logic):
  - `DA_ArenaArchetype` — slot set, module pools per slot, socket rules, intensity caps, constraints.
  - `DA_ArenaModule` — mesh/prefab, sockets (typed + sized), bounds, tags.
  - `DA_ArenaTheme` — materials, lighting, void-gate cosmetic params (the relight roll).
  - `DA_ObjectiveType` — task type → node requirements, channel time, wave triggers.
- **Objective system** — evolve `ARaidObjective` from "one arena, clear elites" into a **multi-site
  objective manager**: typed `FObjectiveSite`s, all-visible, extraction gated on completion. Reuses
  the existing phase/replication scaffolding and the extraction-defend timer.
- **Raid director** — evolve `RaidWaveDirector` / `RaidDirector::ComputeWavePlan` to the time-spine +
  3-spike + relax-lull model, reading per-site intensity caps + escalation metadata from `FRaidLayout`.
- **Void-gate actors** — the Maw (spawner), ambient gates (cosmetic), breach gates (dynamic).

## 12. Integration with existing systems

- **Seed:** `URunManager` already rolls + replicates the master seed and exposes `GetStream()`. The
  generator draws a salted sub-stream (never perturbs the master), exactly like `RaidObjective`'s
  fodder placement does today.
- **`ARaidGameMode`** (server-only) creates the `RunManager`; it will also kick the generator at
  raid load and place the squad at the generated drop node (replacing `PickHeroSpawnPoint`'s
  PlayerStart scan with the generated drop node).
- **`ARaidObjective`** → generalized to the multi-objective manager; keeps the extraction-defend
  timer, party-wipe loss, and the run-result bridge (`RunManager.EndRun`).
- **`USpawnDirector` / `UCombatSubsystem`** (C++ seam) — unchanged contract; the director now spawns
  from generated spawn nodes / breach gates instead of rings around a fixed objective.
- **Upgrade loop** (`ARaidGameMode` XP/chest) — unchanged; the relax lulls are its pick pauses, and
  the mini-boss chest drops at the generated `HoldAnchor`.
- **Determinism harness** — extend the `DL_*` debug-level + `SmokeTest.ps1` pattern: a `GenSmoke`
  exec regenerates with a fixed seed and asserts (a) identical layout for identical seed, (b) all §9
  invariants pass, headlessly.

## 13. MVP slice & phase roadmap (reconciled to the mission shape)

**MVP = Phase 0 + 1: a playable, deterministic, validated compact mission.**

- **Phase 0 — Foundation.** The 5-layer pipeline scaffolding; the C++ stamp seam + AS orchestration;
  seed hookup off `RunManager`; the `FRaidLayout` data contract; the determinism harness (`GenSmoke`).
- **Phase 1 — Skatepark + the mission spine (playable).** One archetype (Skatepark, authored shell +
  procedural fill: seeded module pick/rotate/jitter, blue-noise scatter); the full §9 validation +
  reroll; **drop node + ONE hold-and-channel objective site + a separate extraction site**; the
  evolved raid director (time-spine + spikes + lulls); one theme + a starter PCG cosmetic skin with
  ambient void-gates. → *a generated mission you can play end-to-end.*
- **Phase 2 — Variety & feel.** The other 3 objective task-types; 2–3 mains per raid; deeper module
  pools; full theme/relight roll; breach gates; tuning against the §9 checklist.
- **Phase 3 — The archetype pool (dial 6).** Combat Bowl, Tiered Pit, Figure-8, Open Sprawl as
  authored shells + fill; the seed rolls the archetype per site. Eliminate-target objective.
- **Phase 4 — Varying footprint (dial 7).** Socket-based assembly so bounds/shape vary per seed
  (Warframe model). Highest risk; deliberately last, on a proven pipeline.

**Parallelism (CLAUDE.md "one editor" rule):** authoring is fan-out-able — module meshes/prefabs, the
PCG cosmetic graph, validation-rule code, DataAsset schemas, and independent `.as`/`.cpp` files can be
built by parallel subagents. **All** build / `run_code_test` / PIE / MCP verification funnels back
through the single editor session, sequentially. No git worktrees for editor work.

## 14. Risks & open questions

- **Jump-reachability validation is custom work** — UE navmesh won't model double-jump/slide-hop
  links. Build it against the Skatepark anchor early (that's why Skatepark is the anchor). Keep the
  reach envelope in one source of truth shared with D-0021.
- **Reroll thrash / non-termination** — over-tight constraints can fail most seeds. Cap retries; keep
  an authored safe-fallback layout per archetype.
- **PCG AngelScript exposure unverified** — confirm `UPCGComponent::GenerateLocal` etc. are callable
  from AS in the fork; add a C++ `ScriptCallable` seam if not.
- **PCG cross-client cosmetic divergence** — acceptable by design (cosmetic only), but verify nothing
  gameplay reads ever samples Layer-2 output.
- **Scope** — this is a *mission* generator, materially larger than "arena generator." Phases 0–1 are
  the contract; 2–4 are real but deferrable. Confirm the team is comfortable building the multi-system
  spine (generation + objectives + director evolution) as one workstream vs. splitting it.
- **Numbers are hypotheses** — ~12 min, 10–15 s site spacing, 90–120 s extraction hold, spike
  timestamps: all playtest-tuned, not proven.
- **DECISIONS.md updates** — this design touches several open items (max party size, run structure
  detail) and should produce new decision entries (procedural raid generation, the archetype/grammar
  model, the two-layer tech split) once implementation begins.

## 15. Sources (research)

PCG/determinism: Epic PCG docs (Overview, Generation Modes, Runtime Hierarchical, `UPCGComponent`),
Epic forums (PCG multiplayer; runtime params), Bugnet (PCG determinism), Devtricks (low-bandwidth MP
procgen). Arena layout/design: Level Design Book (combat, cover, balance, typology, flow,
verticality), UE Shape Grammar, Poisson-disk refs, Snappable-Meshes navigability, RoR2/Hades/Returnal
write-ups, Halo "Chill Out" study, Doom 2016 design notes. Roguelike structure: Warframe (Tile Sets,
AI Director, Defense/Excavation), GameAIPro Ch.7 (Warframe pacing), Remnant 2 world-gen, DRG missions,
Gunfire Reborn, The Finals (procedural arenas), Vermintide Last Stand. Mission loop: LoL Swarm (LoL
wiki, Mobalytics, Riot tech blog), Helldivers 2 (missions, extraction, Pelican-1), RoR2 difficulty,
L4D Director, Vampire Survivors scaling. (Full URLs in the brainstorming transcript / agent outputs.)
