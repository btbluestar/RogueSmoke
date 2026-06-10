# Handoff — gameplay loop playable + debug (2026-06-10)

Made the raid run loop complete and built a playable level for it. **Everything is committed on
`main` and compiles/builds clean** (`as-helper run_code_test` exit 0), and the level was **boot-verified
headless** (`-game -nullrhi`): the run starts and the objective spawns the roster with no fatals.

## TL;DR — how to play it
1. Open **`/Game/Levels/RaidArena`** in the editor (Content/Levels/RaidArena).
2. Press **Play** (single-player is fine; for co-op use PIE 2 players / Net Mode = *Play As Listen Server*,
   per `TESTING.md`).
3. You spawn as **BP_Vanguard**. Expected start-to-finish sequence:
   - Run starts (log: `[RunManager] Run started — master seed N`).
   - After ~1s the objective spawns **4 ring elites + the Brood-mother boss** (log: `[Raid] spawned 4
     ring elites + boss`), and fodder (Crawler) waves begin as pressure.
   - **Kill all the gating elites + boss** (shoot them; hold **RMB** to focus-aim). Fodder doesn't gate.
   - On clear → "OBJECTIVE COMPLETE", you're **offered an upgrade**, and the objective flips to
     *ExtractionReady* (debug sphere turns green).
   - **Walk onto the objective** (within ~450u) → extraction is called; a **30s defend timer** runs
     (sphere red, final defend wave spawns). Survive it → **EXTRACTED = run won** (sphere blue;
     `ARaidGameState.Phase = Victory`).
   - If the whole party goes down/bleeds out first → **party wipe = run lost** (`Phase = Defeat`).

## Commits (newest first)
- `fffcd3a` playable RaidArena level + `[Raid] spawned` breadcrumb
- `9cd194c` close the raid loop (zone-extraction + EndRun) + console debug camera
- (earlier today: `260ee5e` handoff, `e8a1358` ranged LoS gate, `eeb3a56` focus input wiring)

## What changed (the loop was ~90% there; these closed it)
1. **The run now actually ends.** `RaidObjective` reached `Extracted`/`Failed` but only printed — it never
   told the `RunManager`, so `ARaidGameState.Phase` (the replicated run phase) sat on `InProgress`
   forever. Added `EndRunResult(bVictory)` → `RunManager.EndRun()` → `Phase = Victory/Defeat` (replicates
   to clients for results UI).
2. **You can call extraction solo.** `Server_CallExtraction` existed but nothing invoked it (no input/
   trigger), so solo play dead-ended at *ExtractionReady*. Added a **stand-on-the-pad** trigger: a living
   hero within `ExtractZoneRadius` (450u) of the objective calls it in. Remote clients still also have
   `AHeroCharacter::Server_CallExtraction`.
3. **Playable level** `/Game/Levels/RaidArena`: built from the known-good `DL_Combat` (which is also a
   valid play level) — full lighting, PlayerStart, **GameMode override = BP_RaidGamemode**, objective
   raised to z=100 so spawned elite bodies sit on the floor, **4 perimeter walls** to contain the arena,
   legacy hand-placed test elites removed (the objective auto-spawns the real bio-horde roster).

## Debug aids
- **`RaidDebugCam`** — type it in the **`~` console** during play to toggle a **top-down overhead view**
  of the whole arena (watch the objective phase ring/label, elite telegraph spheres, fodder waves), then
  type it again to return to the over-shoulder camera. Works in any level, no input binding needed.
- **In-world debug draw is on** (`RaidDebug::bEnabled = true`): objective phase sphere+label
  (white=clearing, green=ready, red=defending, blue=extracted), elite telegraph spheres (yellow) + slam
  footprints. Flip `Script/Debug/DebugConfig.as` to `false` + save to kill all of it.
- **`DL_Combat`** remains the scratch debug level (has 5 hand-placed elites + the auto-spawn roster, so
  it's busier — good for combat testing; `RaidArena` is the clean playthrough level).
- Engine free-fly debug cam: `ToggleDebugCamera` console command (backtick) also works.

## Verified vs. needs-your-playtest
- **Verified headless:** AngelScript compiles; `RaidArena` boots a live run; the objective spawns 4 ring
  elites + boss; no fatal/script errors in a 28s boot.
- **Needs your Play press (interactive PIE — the in-editor PIE MCP is still wedged, so I can't drive it):**
  the *feel* of combat, that elites can actually be killed and then the **clear → upgrade → walk-to-zone →
  defend 30s → EXTRACTED** transitions fire, and the party-wipe → DEFEAT path. The logic compiles and the
  spawn/start half is confirmed; the rest is straightforward phase transitions but unproven in real play.

## Known caveats (MVP-acceptable)
- Elites are kinematic and hold spawn Z (no gravity); objective at z=100 keeps bodies roughly on the
  floor. If a creature looks half-sunk, nudge the objective's Z.
- Walls are plain cubes (containment, not art). Floor is an 8000u plane (radius ~4000).
- Debug draw is on by default (see above) — turn off for a "clean" capture.
- No results screen / return-to-menu yet — the run phase flips to Victory/Defeat and prints; wiring a
  results UMG + travel back to lobby is the next loop-polish step.
- Headless `-game` can't prove the *full* loop because there's no AI to kill the elites for you — that
  part is your Play test.
