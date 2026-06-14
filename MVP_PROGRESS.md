# MVP Progress — bring-up COMPLETE

> **Status (2026-06-11): the slice bring-up this file tracked is done.** Every step below
> shipped and most were then superseded by larger systems. This file is now a historical
> index of *where each step ended up* — current truth lives in **`DECISIONS.md`**
> (D-0001…D-0020) and **`startup.md`** (how to build/verify anything today).

Original tracker: bring-up order from `SETUP.md` §5 for the taunt→barrage slice (D-0008).

| # | Step (SETUP §5) | Status | Where it ended up |
|---|-----------------|--------|-------------------|
| 0 | Build UE 5.7 + AngelScript fork | ✅ | Source build at `F:\UEAS` (read-only), in daily use. |
| 1 | Hot-reload smoke test | ✅ | Pipeline proven; `as-helper run_code_test` is the routine script gate now. |
| 2 | Verify API signatures vs compiler | ✅ | Fork conventions captured in `startup.md` + `UPGRADES_SETUP.md` §"Fork API conventions". |
| 3 | `UCombatSubsystem` (C++) | ✅ | The seam, both directions: player→enemy (`FireHitscan`, AoE/pull/mark, on-hit procs D-0019/20) and enemy→player (`ApplyDamageToPlayer`, D-0017). |
| 4 | First combo on Actors | ✅ | Reimplemented on **GAS** (D-0013): `GA_Taunt`/`GA_Barrage`, each with an evolution track (D-0020). |
| 5 | Fodder swarm | ✅ (interim) | Cheap-Actor Crawlers (`AFodderEnemy`) driven by the wave director (D-0020). **Mass backend still deferred** behind the `SpawnFodderWave()` seam (D-0003). |
| 6 | Upgrade + select widget | ✅ | Grew into the full upgrade loop: team-XP level picks + synergy chest, per-player hands, stacks/prereqs, 35-card pool, behavior evolutions (D-0018/19/20). CommonUI screen, runtime-built cards. |
| 7 | Raid objective + extraction | ✅ | Playable end-to-end in `/Game/Levels/RaidArena` (see `HANDOFF_gameplay_loop.md` for the play-through TL;DR). |

## What came after the slice (chronological)

- **D-0013** — abilities/attributes/upgrades moved onto GAS (AngelscriptGAS, Lyra patterns).
- **D-0014** — camera became third-person over-shoulder shooter (was top-down).
- **D-0015** — movement kit: sprint / crouch / slide / double-jump.
- **D-0016** — CommonUI for all screens (layer stacks in `URogueUILayout`).
- **D-0017** — bio-horde enemy roster (Crawler/Carapace/Spitter/Bloater/Lunger/Brood-mother).
- **D-0018/19/20** — upgrade acquisition loop v1→v3: shared XP levels, chest, per-player hands,
  eligibility, hero ability tracks, behavior evolutions, wave director.

## Still ahead

- **Mass fodder backend** (the one ⬜ left from the original plan — D-0003, version-sensitive).
- **Niagara/GameplayCue polish pass** (telegraphs, arcs, bursts, vortex — debug draw today).
- **Balance pass** (D-0019/20 numbers are first-pass).
- **Open decisions** — meta-progression scope, party size, solo, friendly fire: see
  `DECISIONS.md` §"Still open".
