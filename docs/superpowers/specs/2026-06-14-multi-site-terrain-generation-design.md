# Multi-Site Mission + Generated Terrain — Design Spec

> Status: **Design approved (brainstorming)** — 2026-06-14. Branch: next slice after PR #2
> (`ProcedualLevelGeneration`). This is the *what/why*; the implementation plan is the *how/when*.
> Builds directly on the procgen MVP shipped in PR #2
> (`docs/superpowers/specs/2026-06-14-procedural-raid-generation-design.md`).
> **Sequencing rule (user):** deepen LEVEL GENERATION before any graphics — no PCG / theme / final
> art in this slice (see memory `procgen-deepen-before-graphics`).

## 1. Goal

Turn the generator from a **single flat greybox arena** into a small **multi-site mission on
generated terrain**: one large bounded footprint of procedural terrain, **2–3 hold-and-channel
objective zones grown onto it**, a shared drop and a separate extraction at opposite ends, traversed
in any order under a rising tide. The terrain *is* the floor (the player fights on it); the arena
reads as a quarantine deployment **dropped onto a contaminated site** (boundary ring + deployed
structures + terrain poking through), not a flat slab.

This is the **multi-site mission graph** slice (chosen from four candidates: multi-site graph,
modular grammar, designer DataAssets, archetype variety). It makes the generator a real *level*
generator and gives the other three slices a richer skeleton to plug into later.

## 2. Hard constraints (inherited)

- **Deterministic from one master seed** (D-0007): identical world on host + clients; replicate the
  **seed only**, never stream geometry. The research is explicit that **float math is not bit-identical
  across machines** — so **all geometry-affecting math is integer / fixed-point**, and any float noise
  is an *offline/authoring* tool only. Terrain heights come from **integer hash value-noise**, quantized.
- **Server-authoritative** (D-0004); only the server advances the run. Geometry is **re-stamped
  locally on every machine from the replicated seed** (the existing `URaidStampSubsystem` pattern),
  never replicated.
- **Script the decisions, compile the simulation** (D-0002): layout decisions in AngelScript
  (`Script/Generation/`); the ISM stamping seam in C++ (reused as-is — **no new C++ required for MVP**).
- **Additive + opt-in:** all new behavior runs only on the generated level (`L_GenRaid`) behind the
  existing `bGeneratedArena` / objective-`Mode` flags. The single-site `RaidArena` / `RaidLoop` /
  `MoveSmoke` regression cases stay green.

## 3. Research grounding (briefing 2026-06-14)

Load-bearing findings (full sources in §11):

- **Float terrain off the hot path.** Float noise desyncs host↔client; the robust pattern is
  discrete geometry chosen by the integer seed — exactly the ISM-seam pattern we already own. Runtime
  Landscape is GPU/headless-hostile (no dedicated-server / NullRHI) → avoid. (Gaffer; Errant; Epic.)
- **Reserve-then-populate** (Risk of Rain 2 Scene Director): reserve the must-have anchors first as
  exclusion zones, then fill around them. Objective placement is a **separation/exclusion** problem,
  not free scatter (Helldivers 2 POIs vs objective footprint).
- **Poisson-disk / blue-noise** guarantees "close but never closer than r" — kills clumping; ideal for
  zone anchors (large r) + cover (small r). **Voronoi partition** around anchors gives each zone a
  territory and turns the borders into natural traversal lanes.
- **Terrain craft:** layered value-noise + redistribution (`pow`) for broad flats, domain-warp/ridged
  for the wild rim, **flatten-masks + slope-clamp under play zones** so fast movement + Mass pathing
  stay clean. Push drama to the rim, keep the interior graded-but-walkable.
- **"Dropped" aesthetic** = the *joint* between a constructed layer (boundary ring, deployed
  structures, flat-ish play) and a found layer (warped terrain, poke-throughs). We chose the
  **terrain-floor-forward** reading (model B): play on the terrain everywhere; structures + boundary
  sell "deployed."

## 4. Decisions locked during brainstorming

| # | Decision | Choice |
|---|----------|--------|
| 1 | Which deepening slice first | **Multi-site mission graph** |
| 2 | Geometry/connectivity model | **Continuous partitioned arena** (one footprint, zones inside, open-ground travel — NOT rooms+corridors) |
| 3 | Site count & order | **Seed-rolled 2–3 sites, free order, extraction gated on all** |
| 4 | Objective task type (this slice) | **All hold-and-channel** (other types stay Phase 2) |
| 5 | Site placement | **Partition-then-place**: reserve drop/extraction → Poisson zone anchors → Voronoi partition |
| 6 | Drop / extraction | **Opposite ends** of the footprint (no zone on the spawn→exit critical path) |
| 7 | Floor × "dropped" relationship | **Model B** — generated terrain is the play floor everywhere; light deployed structures + boundary sell "dropped" |
| 8 | Terrain method | **Integer hash-noise → quantized height-tile grid**, stamped via the ISM seam (slope-clamped in zones, wild rim) |
| 9 | Director across sites | **Active-site focus + global tide**: pressure concentrates on the site being channeled; quiet sites stay calm |
| 10 | Mini-boss + chest | Fire at the **last objective completed** (order-driven, reuses chest-at-HoldAnchor) |

## 5. The generation pipeline

Each pass is one deterministic, seeded, **integer/fixed-point** stage. One master seed →
per-channel integer sub-seeds (terrain / partition / zones / cover) so channels don't correlate.

1. **Terrain** — `RaidTerrain::Generate`: integer hash value-noise → a grid of **quantized tile
   heights**. Redistribution biases broad flats; the **rim** keeps the raw (wilder) heights.
2. **Reserve + place** — `RaidPartition::Place`: pick the drop/extraction axis (opposite ends);
   reserve those anchors; **Poisson-disk** place 2–3 zone anchors with min-separation; assign each
   terrain tile to its nearest anchor (**Voronoi**) → zone territories; borders = traversal lanes.
3. **Flatten zones** — `RaidTerrain::FlattenZones`: lerp tile heights toward each zone's plane and
   **slope-clamp** inside zones (walkable grades for slide-hop + Mass); lanes/rim stay graded.
4. **Per-zone fill** — existing per-site fill, looped per zone: Maw (`CombatCore`), hold pad
   (`HoldAnchor`), high-ground platforms, **blue-noise cover**.
5. **Dropped structures** — boundary ring (diegetic quarantine wall), Maw/void-gate rigs, a few
   seeded terrain poke-throughs. (Greybox primitives; full art deferred.)
6. **Validate + reroll** — the §7 battery; failure advances the seed and re-rolls deterministically;
   capped retries → known-good safe fallback (never softlock).

## 6. Data model & system changes

**Data (`Script/Generation/`)**
- `FRaidLayout.MainSites` — already a `TArray<FRaidSite>`; **fill with 2–3** instead of 1. Existing
  `FRaidSite` / `FRaidNode` / `FRaidCover` already carry Maw / hold / high-ground / cover.
- New `FRaidTerrain` — heightfield contract: grid resolution, tile size, **per-tile quantized height**
  (ints), per-zone flatten plane + slope-clamp params.
- New `FRaidZonePartition` — zone anchor points + footprint bounds (Voronoi cells are derived, not stored).
- `Drop` / `Extraction` anchored to opposite ends (deterministic axis pick).

**Generation (`RaidGen` and new namespaces)**
- `RaidTerrain` — integer hash-noise heightfield + `FlattenZones` (NOT float Perlin).
- `RaidPartition` — reserve-then-populate + Poisson anchors + Voronoi assignment.
- `RaidGen::Generate` — orchestrates terrain → partition → flatten → per-zone fill → validate.

**Stamping (`RaidStamper.as` + existing C++ seam)**
- New `StampTerrain` pass: stamp the height-tile grid (brown tint) via the existing
  `StampBoxesColored` — **reuses the seam; no new C++ for MVP**.
- Boundary ring + per-zone elements: stamp as today, looped over N zones, on the blockout palette.

**Director (`RaidWaveDirector` / `RaidObjective`)**
- Track the **active channeling site**; concentrate spawn pressure on its Maw while the global
  time-tide rises; quiet sites stay calm. Per-site `IntensityCap` already exists on `FRaidNode`.

**Objective manager (`RaidObjective.as`)**
- Generalize from one hold point to a **multi-site manager**: read `MainSites`, track per-site channel
  progress, **gate extraction until all complete**, fire **mini-boss + chest at the last-completed
  site**. **Keep the existing phase/replication bridges** (`SetPhase` → ExtractionReady → CallExtraction
  → Extracting → Extracted; party-wipe → Failed) untouched — they are the run-result contract.

## 7. Validation invariants (additive to the existing battery)

Pure-geometric, integer/fixed-point, server-side; failure → deterministic reroll → safe fallback.

1. **Zone min-separation** — no two zone anchors closer than r (anti-clustering).
2. **Drop↔Extraction far apart** — at/near opposite ends; both off every zone's footprint.
3. **No zone-footprint overlap** — zones own disjoint territory.
4. **Every zone reachable** — flood/route across the terrain from drop to each zone and to extraction
   (geometric reachability; navmesh-based check stays deferred until Mass pathfinds).
5. **Terrain walkable in zones** — max slope (height delta between adjacent tiles) inside any zone ≤
   the walkable threshold (slide-hop + Mass).
6. **Escape-proof still holds** — the existing reach-envelope check over the new terrain + boundary.

The existing checks (jump-reachability, cover blue-noise, high-ground reach, etc.) continue to run.

## 8. Determinism & testing

- One master seed → integer sub-seeds; integer/fixed-point everywhere geometry depends on it; stamped
  on every machine from the replicated seed.
- **`run_code_test`** — pure-logic compile/run for terrain, partition, validation.
- **`GenSmoke`** (extend) — assert 2–3 sites, terrain present, **same-seed → identical layout incl.
  tile heights**, all §7 invariants pass.
- **`GenLoopSmoke`** (extend) — multi-site victory: clear all sites (any order) → mini-boss at last →
  extraction arms → defend → win; party-wipe still routes to Defeat.
- **Director** assertion — active-site focus (pressure on the channeling site, not the quiet ones).
- **`GenShots`** — colored greybox capture for visual proof (terrain + multi-zone footprint).
- Full **`SmokeTest.ps1`** suite stays green (single-site cases unaffected by the opt-in flags).

## 9. MVP scope cut & deferred

**In:** generated terrain floor; 2–3 hold-and-channel zones via partition-then-place; drop/extraction
opposite ends; multi-site objective manager (extraction-gated, last-site climax); active-site director;
additive validation + reroll + fallback; all stamped as colored greybox via the existing seam; runs on
`L_GenRaid`.

**Deferred (explicitly):**
- PCG cosmetic skin / theme / final art (the "before graphics" rule).
- Modular **socket grammar** (richer per-zone geometry) — geometry stays parametric greybox.
- **`DA_Arena*` DataAssets** — tuning stays in `FRaidGenConfig`; the "designer DataAssets" slice is separate.
- Other objective task types (Phase 2); archetype variety beyond Skatepark (Phase 3); navmesh
  reachability (until Mass pathfinds); dynamic breach/ambient gates; literal corridors / full decking / stilts.

## 10. Risks

- **Terrain tile count vs performance** — keep the grid coarse for greybox; tile size is a tunable.
- **Integer-noise terrain reads blocky** — acceptable (and on-theme) for a blockout; it's the floor's
  *shape* we're proving, not its finish.
- **Multi-site objective refactor** is the biggest single change — it must preserve the run-result
  bridges; the regression suite (`RaidLoop*`, `GenLoopSmoke`) is the guard.
- **Mass pathing on stepped terrain** — verify swarm agents traverse the slope-clamped tiles cleanly;
  if not, tighten the slope clamp. (Research couldn't confirm Mass-on-uneven-ground; verify in-editor.)

## 11. Sources (research briefing 2026-06-14)

Red Blob Games (terrain-from-noise); Inigo Quilez (domain warping); Book of Shaders (fBm); Bridson /
Extreme Learning (Poisson-disk); Deepnight & Game Developer (Dead Cells hybrid); DRG cave-gen (UE
forum); RoR2 Wiki (Directors / Access Node); Helldivers 2 POIs (Steam guide / wiki); Gradientspace
(GeometryScript/DynamicMesh FAQ); Errant (runtime Landscape limits); Gaffer On Games (floating-point
determinism / deterministic lockstep); UE 5.7 docs (NavMesh generation); UE forum (Mass landscape
navigation); arXiv 2308.07307 (deterministic WFC); Wikipedia (stilts). Full URLs in the research agent
output for this session.
