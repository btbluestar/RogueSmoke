# Decisions

> A running log of decisions that are **made**, so they aren't re-litigated.
> When the AI or a teammate is unsure "did we decide X?", this file is the answer.
> Newest decisions at the bottom. Keep each entry short: what, why, what it rules out.
> Open questions live at the end under **Still open**.

Format per entry: ID, date, status, the decision, the reasoning, and consequences.

---

### D-0001 — Engine: UE 5.7 + Hazelight AngelScript fork
- **Status:** Decided
- **Decision:** Build on Unreal Engine 5.7 using the Hazelight UnrealEngine-Angelscript fork.
- **Why:** AngelScript gives hot-reload iteration on gameplay; the fork is proven in shipped
  co-op titles (Split Fiction, ARC Raiders). GDD §11.3.
- **Consequences:** Source-built custom engine required (see `SETUP.md`); no pre-built binary
  plugins; must confirm the fork has a stable 5.7 branch before committing deeply.

### D-0002 — Language split: "script the decisions, compile the simulation"
- **Status:** Decided — **supersedes** any "gameplay is AngelScript only, no C++" wording in
  older docs.
- **Decision:** AngelScript owns iteration-heavy gameplay (abilities, synergies, upgrades,
  objective/extraction flow, UI). C++ (with Mass) owns the hot path (swarm simulation,
  authoritative combat queries, replicated health, spawning/pooling). The two layers meet at
  **one small seam: `UCombatSubsystem` (C++)**. AngelScript never iterates enemies directly —
  it calls the subsystem.
- **Why:** Pure AngelScript can't hit the enemy counts the "a lot of enemies" pillar requires
  (GDD §7, §11.2); pure C++ kills iteration speed on the fun-tuning surface. See
  `Rogue_Smoke_MVP_Architecture.md` §1–3.
- **Consequences:** The project is **not** AngelScript-only. Do not "refactor away" the C++
  combat/Mass layer. CLAUDE.md and CODING_STANDARDS.md should reflect this seam.

### D-0003 — Enemy representation: Mass for fodder, Actors for elites
- **Status:** Decided
- **Decision:** Fodder/swarm enemies are Mass agents (data-oriented, cheap). Elites and bosses
  are full Actors (`EliteEnemyBase`, extended in script). `UCombatSubsystem` damages both
  representations behind one API.
- **Why:** Density is a mechanic (clustering enables synergies, GDD §5.1/§7); only a
  data-oriented backend makes the counts tractable. MVP arch §2, §4.
- **Consequences:** Any AoE/query ability must go through the subsystem so it hits both Mass
  agents and Actor elites. Spike Mass early to validate the count target.

### D-0004 — Networking: listen server, server-authoritative
- **Status:** Decided (default; revisit if dedicated servers are needed)
- **Decision:** Co-op runs on a listen server (host is a player). The server is authoritative
  for all game state; clients send intent via `Server_` RPCs and run cosmetics only.
- **Why:** Genre standard; simplest correctness for high entity counts. ARCHITECTURE §2.
- **Consequences:** Only the server spawns replicated actors, applies damage, grants loot,
  advances the run. Host-side player logic may assume a local pawn (would not hold for a
  dedicated-server build).

### D-0005 — Camera: top-down twin-stick
- **Status:** Decided
- **Decision:** Top-down camera (spring arm, ~-60° pitch, fixed rotation).
- **Why:** Maximizes readability of swarms and cross-player setups — the two hardest pillars —
  at lowest cost. GDD §9; implemented in MVP arch §6.
- **Consequences:** Controller-first input is viable; art/readability budget assumes a top-down read.

### D-0006 — Ability framework: custom component for MVP, defer GAS
- **Status:** Decided (MVP scope)
- **Decision:** Use a lightweight custom `UAbilityComponent` (cooldown + server/multicast
  plumbing) for the vertical slice. Re-evaluate Gameplay Ability System later.
- **Why:** Faster to iterate in script; GAS is heavy for a first slice. ARCHITECTURE §4.2,
  MVP arch §5.1.
- **Consequences:** If ability complexity outgrows the custom component, revisit and record a
  new decision rather than bolting on.

### D-0007 — Procedural generation is deterministic
- **Status:** Decided
- **Decision:** All world-affecting randomness flows through a single seeded RNG stream owned by
  the RunManager; the master seed replicates via `GameState`.
- **Why:** Co-op requires identical worlds across machines; determinism also makes bugs
  reproducible. ARCHITECTURE §4.1, CODING_STANDARDS §5.
- **Consequences:** No unseeded random, no iteration-order/wall-clock dependence in generation.
  Determinism tests are mandatory (see `TESTING.md`).

### D-0008 — First synergy slice: taunt → barrage
- **Status:** Decided (MVP scope)
- **Decision:** The vertical slice proves one synergy with two kits — Vanguard (taunt: pull +
  mark `Clustered`) and Bombardier (barrage: radial damage with cluster bonus).
- **Why:** Smallest content that tests the signature pillar. GDD §12, MVP arch §5–6.
- **Consequences:** Build and tune this combo's *feel* before adding any other content.

### D-0009 — Run structure: a run is a single raid
- **Status:** Decided (MVP scope; revisit when expanding past the slice)
- **Decision:** For now, one **run = one raid**. Complete the objective, extract, run ends.
- **Why:** Smallest scope that proves the slice (GDD §12); no inter-raid state, difficulty
  curve, or "push deeper vs cash out" needed yet. GDD §3.2.
- **Consequences:** Upgrades need not persist across raids yet. Failed extraction loses the
  run (= the raid). Chained/escalating raids become a later, explicitly-recorded decision.

### D-0010 — Extraction: called-in defend timer
- **Status:** Decided (MVP scope)
- **Decision:** Objective complete → players **call in extraction**, then survive a
  **defend timer** (a final wave) in the zone before escaping. Surviving the hold wins the run.
- **Why:** Generates the co-op tension cap the genre wants (GDD §3.3) without inter-raid
  systems. Pairs with the single-raid structure (D-0009).
- **Consequences:** Needs a wave/defend spawn on extraction (hook `OnExtractionPhaseStarted`
  in `RaidObjective.as`, spawner TBD) and party-wipe detection (depends on down/revive,
  MVP §12). Implemented as `Script/Objective/RaidObjective.as`.

---

## Still open

These are unresolved and block or shape downstream work. Resolve, then move up as a D-entry.

1. **Meta-progression scope** — shared per-session vs per-player persistent unlocks
   (ARCHITECTURE §4.5, GDD §6.3).
2. **Max party size** — GDD says 2–4; MVP slice targets 2. Confirm the real cap (affects spawn
   budgets, scaling, UI).
3. **Solo support** — unsupported / scaled / companion (GDD §2, §4).
4. **Friendly fire** — off (recommended) / positional (GDD §4).
5. **Theme/setting & whether "smoke" is mechanical** (GDD §10).
6. **Enemy-count target on target hardware** — set a concrete N; it's a design constraint, not
   just optimization (GDD §11.2).

> Resolved & moved up: run structure → **D-0009**, extraction design → **D-0010**.
