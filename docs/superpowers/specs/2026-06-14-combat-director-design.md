# Combat Director (Attack Tokens) — Design Spec

> Status: **Design approved (brainstorming)** — 2026-06-14. Branch: `combat-director`.
> This is the *what/why*; the implementation plan is the *how/when*.
> Research-backed (combat-director / attack-token / combat-circle pattern). See memory
> `combat-director-attack-tokens` and `enemies-on-terrain-architecture`.

## 1. Goal

Make encounters *feel* like a dangerous crowd while keeping the number of genuinely-attacking,
expensive-AI enemies small. A server-side **combat director** grants a limited number of **attack
tokens**; only token-holders ("Engaged") run the full telegraph→attack loop, while everyone else
("Background") cheaply circles and threatens until promoted or killed. This is the
DOOM-2016 / Batman-Arkham / Left-4-Dead model, tailored to RogueSmoke.

This **removes any need for Mass** (decided): modest pooled Actors + an intelligent director, not
thousands of ECS agents. It is the first slice of the enemy-AI workstream.

## 2. Why this shape (research grounding)

- **Attack tokens** (DOOM 2016): enemies request a token, attack, then release it; per-attack-class
  pools; token *stealing* by better-positioned units (deferred). The release-on-timeout backstop
  matters — DOOM Eternal visibly stalls when tokens never release.
- **Combat circle** (Batman Arkham): ~2–3 simultaneous attackers; the rest circle and pose so the
  player always has a reaction window.
- **AI Director** (Left 4 Dead): population cap + out-of-view spawn + recycle + a build-up→peak→relax
  intensity curve (the pacing layer; mostly deferred — our `RaidWaveDirector` already paces spawns).
- **Combat Coordinator** (The Last of Us): a central manager assigns roles and limits how many do each
  action at once; promotion scored by distance + LoS + spread.
- **Co-op:** per-player token budgets prevent ganging; a *separate* elite-injection rate limit avoids
  dumping multiple telegraphed elites at once (the Darktide failure mode — deferred but noted).

## 3. Architecture (decided: custom-tick + director; NOT Behavior Trees for the MVP)

The existing enemies are plain `AActor`s (`AAttackingElite : AEliteEnemyBase : AActor`) running a
hand-written C++ tick loop with per-archetype attacks overridden in AngelScript. We keep that and add
the director on top. (Behavior Trees / `AIController` — the reference Transarms stack — stay a *later*
option only if bespoke behaviors outgrow custom states; they'd require making enemies Pawns + navmesh.)

Two components, split along "script the decision / compile the simulation":

- **`RaidCombatDirector`** (NEW, AngelScript, `Script/Objective/`, **server-only**) — the *decision*.
  A focused class the `RaidObjective` owns and ticks on a throttled ~0.25s cadence. Each tick it gathers
  living token-using elites + players, maintains per-player token pools, scores eligible `Background`
  elites, and **grants/releases** tokens by writing each elite's engage-state. Separate from the
  already-large `RaidObjective` so it has one clear responsibility and is independently testable.
- **`AAttackingElite`** (EXTENDED, C++) — the *simulation*. Gains:
  - `enum class EEngageState { Background, Engaged }` (server-set; default Background).
  - `bool bUsesAttackToken = true` — **`AFodderEnemy` sets it false** so Crawlers are exempt
    (fodder inherits from `AAttackingElite`; a flag is the clean exclusion).
  - A **Background movement branch** in `Tick`/`ApproachTarget`: approach to a *ring* standoff
    (further than attack range) and hold / gently strafe, **skipping telegraph + attack**.
    `Engaged` = today's exact approach→telegraph→attack loop, unchanged.
  - A server setter the director calls, e.g. `SetEngageState(EEngageState)`, plus read access to
    position/target/archetype for scoring.

**Data flow:** director (AS) decides *who* attacks → `SetEngageState` on each elite → elite (C++)
executes *how* it moves/attacks. `RaidObjective` ticks the director (server, `HasAuthority`).

## 4. Token model & rules (MVP)

- **Per-player pools.** Each elite targets the nearest living player (already does via `AcquireTarget`)
  and draws a token from *that player's* pool — per-player budgets stop one player being ganged.
- **Budget: a single per-player active-attacker count, default 3.** (The melee/ranged split — ~2 melee
  + 3 ranged, the DOOM insight — is the first *refinement*, deferred; it needs each archetype tagged
  with an attack class.)
- **Eligibility for promotion:** `Background` + alive + `bUsesAttackToken` + within an engagement range
  of its target player + off the re-eligibility cooldown.
- **Promotion scoring (pick the freed token's holder):** primarily **nearest / soonest-to-reach** the
  target, with two tiebreaks: **spread** (penalize an elite on the same side as one already attacking
  that player → they surround) and **archetype variety** (prefer a different archetype than already
  engaged). LoS-scoring and arc-filling are deferred polish.
- **Release** (token returns to the pool) on any of: attack committed (`PerformAttack` fired), death,
  target lost, or a **timeout backstop (~6s)** so an Engaged elite that can't reach its target doesn't
  hog a token. On release the elite gets a **~2.5s re-eligibility cooldown** so tokens visibly *rotate*
  through the crowd.
- **Cadence:** the director re-evaluates every ~0.25s (cheap; no need for frame-perfect promotion).

Recommended MVP defaults (all `UPROPERTY`, designer-tunable): **3 active attackers/player, 6s timeout,
2.5s rotation cooldown, 0.25s director tick, ring standoff ≈ 1.6× `AttackRange`.**

## 5. Authority & replication

- The director runs **server-only** (`HasAuthority` gate). It is wired behind a flag
  `bEnableCombatDirector` (default **on**) on the objective, so it can be disabled per-map if needed.
- **No new replication.** The visible difference between Background (circling/holding) and Engaged
  (telegraphing/attacking) is just actor movement (transforms already replicate) plus the
  already-replicated `bTelegraphing`. Clients reproduce the look for free; the engage-state flag is a
  server-internal scheduling detail.

## 6. MVP scope cut & deferred

**In:** `RaidCombatDirector` (single per-player budget, nearest+spread+variety promotion,
attack/death/target-lost/timeout release, rotation cooldown, 0.25s tick); `AAttackingElite` engage-state
+ `bUsesAttackToken` + Background ring/hold branch; `RaidObjective` ticks it behind `bEnableCombatDirector`.

**Deferred:** melee/ranged split pools; per-zone director instances (the multi-site zones exist, but one
global director for MVP); L4D intensity curve (build-up→peak→relax); coordination barks/audio; token
stealing; suppression-fire / taunt-feint background sub-states; target-rebalancing fairness for split
parties; LoS / arc-filling promotion scoring; terrain ground-Z movement for enemies (tracked separately
in `enemies-on-terrain-architecture`).

## 7. Testing

- **`ue-cpp build`** — the `AAttackingElite` extension compiles (0 errors).
- **`run_code_test`** — the AngelScript director compiles + runs.
- **New headless `CombatDirectorSmoke` exec** on a debug enemy level (`DL_Enemy_*` / `DL_Combat`): spawn
  many elites around a player, tick, and assert **(a)** the count of Engaged/telegraphing elites ≤ the
  per-player budget, and **(b)** tokens **rotate** — different elites are Engaged across two sampled
  windows (not the same set frozen). Prints `[CombatDirectorSmoke] RESULT n/n`; added to `SmokeTest.ps1`.
- **Full `SmokeTest.ps1` suite stays green.** Verify the director (now active on `RaidArena`'s elites)
  doesn't break `RaidLoopSmoke` or the enemy cases — if any depend on all-enemies-attacking,
  `bEnableCombatDirector` is the escape hatch.
- **Optional PIE proof:** a 1-client PIE screenshot/trace showing the combat circle — a few elites
  telegraphing, the rest holding at the ring.

## 8. Risks

- **The director gating attacks on `RaidArena`** could change existing combat feel / a regression case
  that assumes all elites attack. Mitigated by `bEnableCombatDirector` (flag) + the suite gate.
- **Background "strafe" cost** at higher counts — keep Background movement trivial (approach-to-ring +
  hold; strafe is optional/cheap), and the director ticks at 0.25s, not per frame.
- **Timeout tuning** — too short and tokens thrash; too long and a stuck elite hogs a slot. 6s is a
  starting point; tune in playtest.
- **Enemies are custom-moved `AActor`s (not CMC)** — Background ring-approach still moves them on the
  current flat-ish plane; terrain-aware movement is the separate enemies-on-terrain item, not this slice.

## 9. Sources (research briefing 2026-06-14)

DOOM 2016 attack tokens (Game Developer; GDC "Push Forward Combat"); DOOM Eternal token-stall
(Doomworld); Batman Arkham Freeflow (Arkham wiki; Game Developer); battle-circle tutorials (Tuts+;
signalsandlight); Shadow of Mordor / AC tokens (Steam); The Last of Us Combat Coordinator (Game
Developer; Game AI Pro 2 Ch.34; GDC Vault); F.E.A.R. GOAP (Game Developer); Halo 2/3 AI (Game Developer;
WPI PDF); Left 4 Dead AI Director (Mike Booth/Valve PDF; L4D wiki); Gears Tactics simultaneous actions
(Game AI Pro Online 2021 Ch.3); Alien Isolation director (Game Developer); Vermintide/Darktide AI
director (Hastewire; Steam). Full URLs in the research agent output for this session.
