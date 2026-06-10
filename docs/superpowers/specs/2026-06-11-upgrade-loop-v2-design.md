# Upgrade Loop v2 — Design Spec

Date: 2026-06-11
Status: approved (user, 2026-06-11)
Builds on: D-0018 (in-raid upgrade loop), D-0013 (upgrades are GAS GameplayEffects)
Research basis: LoL Swarm (primary), Vampire Survivors, Risk of Rain 2, Brotato/DRG:Survivor, Hades.

## Goal

Evolve the existing in-raid upgrade loop from "works" to "feels good in co-op", and lay the
framework for the project's core vision: **multiple abilities that synergize with each other**,
with synergy cards earned by what the squad actually built during the raid.

Scope ordered by priority (all four approved):
1. **A — Pick-flow ergonomics** (per-player hands, lock-in status, reroll, fallbacks, auto-pick)
2. **B — Synergy framework + content** (squad eligibility, 4–6 new synergy cards)
3. **C — Upgrade levels, caps, milestone choices** (DRG-overclock style, GE-only)
4. **D — Pacing & rarity balance** (XP curve, rarity floors+caps)

Explicitly **out of scope** this pass: full Swarm-style weapon evolutions (transformative new
weapon behaviors), banish, meta-progression, per-player persistent pools.

## Key research findings driving the design

- Swarm (1–4p co-op, Riot) validates our exact pause model: shared XP → simultaneous level-up →
  global pause → 3-card picks → resume when all locked in, 30 s timeout, ~1 s eased resume.
  Swarm rolls **independent hands per player** — we currently broadcast one shared hand.
- Swarm filters offers by capacity (full slots ⇒ not offered) and **never shows a dead screen**:
  pool exhausted ⇒ utility consolation (heal 25% / gold to all).
- Hades **duo boons** = the model for cross-player synergy legibility: prerequisites from two
  sources, unique framing, celebrated when taken.
- Brotato: rarity should be **floors + capped ceilings** by progress, not periodic spikes.
- DRG:Survivor: milestone "choose a modifier" picks at track levels; tag-conditional offers.
- Pacing target for a ~15-min raid: **8–12 team level-ups, front-loaded** (Swarm levels much
  faster but pauses cheaper; we cap pause count instead).

## A. Per-player hands & pick-flow ergonomics

- **Per-player rolls.** `RaidGameMode.RollOptions` gains a per-player salt:
  `Seed = MasterSeed + OfferCounter*6151 + PlayerIdx*<prime>`, where `PlayerIdx` is a stable,
  replication-order-independent index (join order on the server). Each controller receives its
  own hand via its own client RPC (per-PC `Client_OfferUpgrades`). Deterministic: same master
  seed + same squad composition ⇒ same hands.
- **Per-player filtering.** A hand never contains a card the player already has at `MaxStacks`;
  milestone-eligible modifier cards are preferentially included (see C).
- **Lock-in visibility.** Per-player "has picked" flag replicated (PlayerState); the card screen
  shows the squad's pick status ("Waiting on: P2…").
- **Timeout auto-pick.** When the 30 s watchdog fires, the server auto-applies the **first card**
  of each unpicked player's hand (today the offer is simply lost on timeout).
- **Exhaustion fallback.** If a player's filtered pool yields < 3 cards, pad the hand with
  utility cards: *Field Dressing* (squad heals 25% max HP) and *Adrenal Surge* (team XP grant).
  Utility cards are ordinary `URogueUpgradeDef`s with instant-style effects; never a dead/short hand.
- **Squad reroll.** 1 per raid, squad-shared. A reroll button on the card screen sends a
  `Server_RequestReroll` RPC; the server validates the squad counter, re-rolls **that player's**
  hand with a fresh salt, and re-sends it. Counter replicated for UI.

## B. Synergy framework + content

- **Squad-level eligibility.** Prerequisites generalize beyond "this player owns X": a synergy
  card may require upgrades/tracks held by **different players** (Hades duo gate, mapped to
  players instead of gods). Example: *Wildfire* requires (any player with ≥1 Burn stack) AND
  (any Bombardier with ≥1 barrage upgrade).
- **Data model.** `URogueUpgradeDef` gains a prerequisite list — each entry: required upgrade
  (or track), required stacks, and a scope (`Self` / `AnyPlayer` / `OtherPlayer`). Level-up
  filtering uses `Self`-scoped prereqs; chest eligibility evaluates the full squad.
- **Chest flow.** Keeps the 3-card pick, but draws only from **eligible** synergy cards, padded
  with team-utility cards when fewer than 3 qualify. (Single-curated-card presentation deferred;
  revisit once the pool is deep.)
- **Content: 4–6 new synergy cards** alongside Chain Detonation. Candidate list — final set
  chosen at planning time against what existing abilities/attributes support without major C++:
  - taunt + chain: chained arcs prefer/bonus-damage **Clustered** targets
  - burn + barrage: barrage ignites struck enemies
  - poison + taunt: poisoned enemies dying near Clustered enemies spread poison
  - shield + slide: sliding through enemies grants overshield / slams Clustered enemies
  - pierce + taunt: pierced hits on Clustered targets refund ammo or add damage
- **Duo framing.** Synergy cards display **both contributing kits** (names/colors) on the card
  and fire a squad-wide callout when taken — synergy must read as special and shared.

## C. Upgrade levels, caps, milestone choices

- `URogueUpgradeDef` gains `MaxStacks` (default per-card; stat cards typically 3–5, milestone
  modifiers 1). Per-player stack counts tracked on PlayerState (replicated).
- Card UI shows progression: *"Piercing Rounds II → III"* with a stack pip row.
- **Milestone modifiers (DRG-overclock style).** When a player's track reaches its milestone
  stack (e.g. 3rd pierce pick), their next hand features a **choice of two modifiers** for that
  track (e.g. *Drill Rounds*: pierce keeps full damage per pass vs *Shrapnel*: pierced hits
  splinter to 1 nearby enemy). Milestone cards are ordinary `URogueUpgradeDef`s with
  `Self`-scoped prerequisites — no new offer machinery beyond prereq filtering + preferential
  inclusion.
- All effects remain **GameplayEffects on existing `URogueCombatSet` attributes**. Where a
  milestone effect needs an attribute that doesn't exist yet, that's a small, per-card C++
  addition (one attribute + one read site) — flagged in the plan, kept to a minimum.

## D. Pacing & rarity

- **XP curve.** Tune `XPBasePerLevel` / `XPGrowthPerLevel` + per-archetype `XPValue` so a
  ~15-min raid yields **8–12 team levels, front-loaded** (levels 1–3 inside ~3 minutes).
  Verified empirically via headless DL boots (kill-feed scripted XP) and a smoke exec that
  prints the level-vs-time table.
- **Rarity floors + caps** (replaces the `%5`/`%10` spike weights): T2 (moderate) offered from
  team level 3, appearance odds capped ~60%; T3 (rare) from level 6, capped ~25%; checked
  top-down (T3 → T2 → T1). Weights still ramp with level under the caps. Seeded/deterministic
  as today (fresh salted stream per offer).

## Non-functional requirements

- **Server authority:** all rolls, eligibility checks, stack counts, reroll spends, auto-picks
  happen server-side; clients only send intent (`Server_` RPCs, validated).
- **Determinism:** every roll from salted streams derived from the master seed; player salt from
  stable join order, never iteration order.
- **Replication discipline:** card hands via per-PC client RPCs (reliable is fine — rare, must
  arrive); pick-status flags replicated properties; no per-tick replication.
- **No UInterface; no logic in Blueprints.** New cards = DataAssets + GE assets only.

## Verification

- `as-helper run_code_test` after every `.as` change; `ue-cpp build` only if C++ attributes added.
- Extend the `DL_Upgrades` exec battery: stacks/caps (`GrantUpgrade` to cap and assert filtering),
  milestone eligibility, synergy squad-eligibility, reroll spend, exhaustion fallback, XP pacing
  table exec.
- `Tools\SmokeTest.ps1` before every commit.
- 2-player listen-server PIE check for: per-player distinct hands, lock-in status display,
  pause/resume, auto-pick on timeout, chest pick.

## Build order

1. Data model: `MaxStacks`, prereq list on `URogueUpgradeDef`; per-player stack tracking.
2. Per-player hands: salted per-player rolls, per-PC RPC, per-player filtering.
3. Widget UX: stack display, lock-in status, reroll button, fallback/utility cards, auto-pick.
4. Milestone modifier cards (content + preferential inclusion).
5. Synergy framework (squad eligibility) + new synergy cards.
6. Pacing & rarity numbers + smoke execs.
7. Docs: `DECISIONS.md` D-0019, `GLOSSARY.md` terms (stack, milestone, eligibility), startup.md
   thin-spots refresh.
