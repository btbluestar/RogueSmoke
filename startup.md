# startup.md — AI agent bootstrap for RogueSmoke

> Read this first if you are an AI assistant (Claude, Copilot, Gemini, …) starting a session on
> this project. It consolidates what the project is, how to build/verify anything without a human
> at the keyboard, and the non-obvious gotchas that have already cost debugging hours.
> Deep detail lives in the docs listed at the bottom — this file tells you which one to open when.

---

## 1. What this is

**RogueSmoke** — a co-op roguelike **third-person shooter** (Risk-of-Rain-ish): squad raids a map,
fights a bio-horde, levels up shared upgrades, kills a mini-boss, extracts. Built on a
**custom source-built Unreal Engine 5.7** with the **Hazelight AngelScript fork**
(https://angelscript.hazelight.se/). Co-op is a **listen server** (the host is also a player).

**The one architectural sentence:** *script the decisions, compile the simulation.*

| Layer | Where | What belongs there |
|---|---|---|
| **AngelScript** (`RogueSmoke/Script/*.as`) | gameplay you iterate on | abilities, upgrades, run/raid flow, enemies' attack logic, UI controllers |
| **C++ + Mass** (`RogueSmoke/Source/RogueSmoke/`) | the hot path | swarm simulation, replicated health, spawning/pooling, the combat seam |
| **Blueprints / DataAssets** (`RogueSmoke/Content/`) | designer data | asset assignment + tuning only — **no logic** |

This is **not** an AngelScript-only project. Do not refactor away the C++/Mass layer (D-0002).

**Three non-negotiable house rules** (full list in `CLAUDE.md` / `CODING_STANDARDS.md`):
1. **Server authority everywhere.** Only the server spawns replicated actors, applies damage,
   grants loot, advances the run. Clients send intent via `Server_` RPCs; validate server-side.
   ⚠ **AngelScript RPCs default to RELIABLE** (opposite of C++) — mark cosmetic RPCs `Unreliable`.
2. **Go through the seam.** Abilities never iterate enemies directly — all combat
   queries/AoE/pull/mark go through `UCombatSubsystem` (C++), which hides whether a target is a
   Mass agent (fodder) or an Actor (elite).
3. **Determinism.** All gameplay randomness through the seeded RNG stream. No unseeded random,
   no iteration-order or wall-clock dependence in generation paths.

Also: **no UInterface/IInterface in AngelScript** (unsupported by the fork) — use base classes,
components, or gameplay tags.

## 2. Environment & paths

- **Repo root:** `C:\Users\btblu\Documents\RogueSmoke` (this file's directory).
- **UE project:** `RogueSmoke\RogueSmoke.uproject` (one level down, *not* two).
- **Engine fork:** `F:\UEAS` — treat as **read-only**. Editor binary:
  `F:\UEAS\Engine\Binaries\Win64\UnrealEditor.exe`. Never open the project with a launcher engine.
- **Platform:** Windows 11, PowerShell 5.1 default shell.
- Docs (`*.md`), `Tools\` harnesses, and the `.uproject` folder all live in the repo root.

## 3. How to build & verify (the part that matters most)

**There is ONE editor / ONE Unreal instance.** Builds, script tests, headless boots, and PIE/MCP
tools all contend for it. Parallelize *research and file authoring* freely; **serialize anything
that touches the editor, builds, or boots the game.**

| You changed… | Verify with |
|---|---|
| `.as` script | `as-helper run_code_test` (compiles all scripts headlessly, exit 0 = pass). Do **not** launch the editor or run a C++ build just to check AS. |
| C++ | `ue-cpp build` (UnrealBuildTool + parsed errors). If an interactive editor is running, Live Coding holds a lock — use `build bounce_editor=true` (stops/builds/relaunches it). |
| Gameplay behavior | Headless debug-level boot (below) or PIE. |
| Content/assets | Editor python commandlet (headless) or MCP `python_exec` against the live editor — but asset **saves** fail (share violation) while an interactive editor has the asset open: `editor_session_end` first, re-run, `editor_session_start` (`-skipcompile`) after. |

### Headless gameplay verification (no human, no window)

Every `DL_*` level under `/Game/Levels/DebuggingLevels/` boots the **real raid loop**
(BP_RaidGamemode, hero embodiment, spawn seam), so `-game -nullrhi -nosound -unattended` boots are
faithful. AngelScript `Print()` lines land in the log as `LogBlueprintUserMessages` — assert on those.

- **One level + exec battery:**
  `Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "UpgradeSmoke" -Grep "Upgrade"`
  (Run **script files**, not inline `Start-Process` one-liners — the machine's antivirus
  AMSI-blocks inline launchers.)
- **Full regression gate:** `Tools\SmokeTest.ps1` — 9 boot cases (RaidArena, 6× `DL_Enemy_*`,
  `DL_Upgrades` twice: once for `UpgradeSmoke`+`UpgradeFlowSmoke`, once for `EvoSmoke`+
  `DirectorReport` — separate boots because UpgradeSmoke applies every pool GE and would
  pre-set the evolution flags). PASS/FAIL table, exit 1 on failure. **Run this before claiming
  a change works and before any commit.**
- **Levels:** `DL_Combat` (sandbox; taunt→cluster→barrage combo), `DL_Enemy_<Archetype>` ×6
  (one enemy each via `AEnemyTestStand`), `DL_Upgrades` (firing range — `AUpgradeTestRange` spawns
  `ATargetDummy` formations: SOLO for damage/DoT, LINE for pierce, CLUSTER for chain).
- **Console execs** (type in `~` or pass via `-ExecCmds`; all retry-poll ~30 s so they survive
  firing before the hero spawns): `ListUpgrades`, `GrantUpgrade <partial>`, `GrantAllUpgrades`,
  `UpgradeSmoke`, `UpgradeFlowSmoke` (D-0019 caps/milestones/duo-gates/reroll/hero-gate battery),
  `EvoSmoke` (D-0020 behavior-evolution battery — run WITHOUT UpgradeSmoke in the same session),
  `DirectorReport` (D-0020 wave-director plan table + checks),
  `RaidXPReport` (XP-curve table), `WeaponSmoke`, `TelegraphSmoke`, `RaidGiveXP <n>`, `RaidKillOneElite`,
  `RaidKillElites`, `RaidGoToChest`, `RaidDebugCam`, `RaidRestart`, `RaidWin`/`RaidLose`,
  `RaidResults`, `RaidPause`. Grep `[XP]`, `[Upgrades]`, `[Chest]`, `[Telegraph]`.

### Multiplayer testing
PIE with **Number of Players = 2, Net Mode = Play As Listen Server** (see `TESTING.md`).
Treat every gameplay change as a networked change.

### Commit discipline
- Never commit broken script or C++ compilation; run `SmokeTest.ps1` first.
- Small, buildable commits. Don't push to remote unless asked.
- **Do not touch:** `RogueSmoke/Content/Blueprints/GE_Upgrade_ChainDetonation.uasset`
  (pre-existing local modification) and `SUPERPOWERS_HANDOFF.md` (unrelated handoff file) —
  both belong to other workstreams; leave them unstaged.

## 4. Gotchas that already cost hours (learn from them)

**AngelScript fork bindings** (C++ reflection signatures do NOT predict AS-callable ones):
- `Gameplay::`/`System::` (UGameplayStatics/UKismetSystemLibrary) **drop the WorldContext param**
  — `Gameplay::GetGameState()`, `System::ExecuteConsoleCommand(...)`, no `this`.
- Our `UBlueprintFunctionLibrary` classes bind with the `Statics`/`Library` suffix **stripped**:
  `URogueGameStatics` → `RogueGame::`, `URogueUIStatics` → `RogueUI::`.
- Script `UGameInstanceSubsystem::Get()` takes **no argument**.
- `FRandomStream` has no `FRand()` — use `RandRange(float64, float64)` or `GetFraction()`.
- `UGameplayStatics::SetGamePaused` has no AS binding — use the C++ shim `RogueGame::SetRaidPaused(bool)`.
- `APlayerController::ClientTravel` not callable from AS — `System::ExecuteConsoleCommand("open " + Addr)`.
- Function parameters are **const** (copy to a local to mutate); `Print()` is dev-only and can't
  call non-const methods in its args.
- AS subclassing C++: reading an inherited `UPROPERTY` needs `BlueprintReadWrite` on the C++ side;
  override via `BlueprintNativeEvent`; `BeginPlay`/`Tick` are additive (no `Super::` call).

**Editor / tooling:**
- A running interactive editor blocks **UBT builds** (Live Coding lock) and **commandlet asset
  saves** (share violation). Headless `-game` boots and `run_code_test` coexist fine.
- Blueprint class-default edits via editor python/MCP **must** be followed by
  `compile_blueprint` + save, or PIE wipes them.
- MCP viewport screenshots show the game view only — **no Slate/UMG/HUD** compositing. Verify UI
  via logs and instance introspection.
- PIE under MCP free-runs in real time — assert end-state or read world time; never infer rates
  from `pie_advance_frames` counts.
- `-nullrhi` boots construct widgets but never Tick them; commandlet python `print` is lost
  (use `unreal.log` + `-abslog`).
- Verify `BP_*` parent classes before trusting a Blueprint — a misparented `BP_RaidGamemode`
  once silently killed GAS and run state.
- **CommonUI never applies a fallback root's input config** in practice: the one-shot apply in
  `FActivatableTreeRoot::ApplyLeafmostNodeConfig` is eaten by an editor-builds-only
  focus-path guard (slate focus dangles at map boot and right after a click destroys its
  button) and is never retried → stuck cursor / no mouse capture. Fix in place: the C++ shim
  `RogueUI::ApplyDesiredInputConfig` (HUD host `OnActivated` + `RogueUILayout.ReapplyTopmostConfig`
  on every screen close). Diagnose router state with `CommonUI.DumpActivatableTree` /
  `CommonUI.DumpInputConfig` + `LogUIActionRouter VeryVerbose`.
- Headless paused-tick timers elapse much faster than wall-clock (clamped per-frame dt at high
  FPS) — "30 s" watchdogs fire in ~5 s headlessly; correct in real play.
- PowerShell 5.1: no `&&`/`||`, and embedded double quotes in native-command args (e.g.
  `git commit -m`) get mangled — avoid them in commit messages.
- Code-spawned `AClusterableElite` is invisible/untraceable (its mesh is assigned per-instance in
  editor levels, not on the class) — use `ATargetDummy` / `AAttackingElite` subclasses for
  code-spawned targets.

## 5. Current state (as of 2026-06-11, branch `main`)

**Working today** (verifiable via the smoke suite):
- **GAS-based hero kit** (D-0013, Lyra-style; ASC on PlayerState, AttributeSets in C++):
  two heroes — **Vanguard** (taunt = setup) and **Bombardier** (barrage = payoff) — wired into the
  signature **taunt → Clustered → barrage** synergy.
- **Third-person shooting** (D-0014): camera→muzzle convergence, focus-aim, hitscan through the
  seam; weapon-upgrade track (pierce / chain / fire rate / burn / poison DoTs).
- **Movement** (D-0015): sprint / slide / jump via `URogueLocomotionComponent`.
- **Enemy roster** (D-0017, bio-horde): Crawler fodder + Carapace / Spitter / Bloater / Lunger
  elites (`AAttackingElite` base, per-archetype AS attack overrides) + **Brood-mother** mini-boss.
  Full telegraph language: ground danger rings, body pulse, per-archetype swell — replicated.
- **Run loop** (D-0009/0010/0011): single-raid runs, escalating fodder waves, extraction defend
  timer, results screen, run timer.
- **In-raid upgrade loop** (D-0018 + D-0019): every kill feeds a **shared team XP pool**
  (per-archetype `XPValue`); a level-up **pauses the raid for all players** — each player gets
  their **own independently-rolled 3-card hand** (seeded per offer + player), filtered by stack
  caps (`MaxStacks`), self-prereqs (milestone modifier cards) and squad duo-prereqs (synergy
  cards); short hands pad from a **UtilityPool** (squad heal / filler). Rarity uses
  floors+caps by team level (r2 from 3, r3 from 6); XP curve front-loaded (base 50, growth 35).
  One **squad reroll** per raid; the 30 s watchdog **auto-picks** card 0 for AFK players; picks
  validate server-side against the offered hand. The Brood-mother drops an **upgrade chest** —
  stand next to it for a squad **synergy-upgrade** pick (only source of synergy cards; offers
  only squad-eligible cards). Pool: 35 cards incl. 5 synergy + 4 **behavior evolutions**
  (chest: Searing Arcs / Toxic Burst / Critical Mass / Iron Bulwark), hero-gated **ability
  tracks** (Taunt → Event Horizon vortex; Barrage → Twin Salvo → Carpet Bombing), milestones
  + 2 utility (D-0020). A **wave director** scales fodder pressure with team level + squad
  size (deterministic elite injections from level 4; injected elites never gate the clear).
- **UI suite** (D-0016, CommonUI): main menu (`L_MainMenu`), hero-select lobby with ready/start,
  HUD (health, run clock, `LVL n x/y XP`), upgrade card screen, escape menu, results.
- **Multiplayer:** LAN/direct-IP listen server (D-0012, NULL subsystem).

**Known thin spots / open threads:**
- Behavior evolutions exist (D-0020) but have **no bespoke VFX** — arcs/bursts/vortex/carpet
  ride debug-draw + telegraph rings until the Niagara cue pass.
- Burn/poison DoTs have **no victim tint** (DoT state is server-only; needs a replicated flag).
- XP curve (50 + 35/level), rarity floors+caps, and per-archetype `XPValue`s are first-pass
  numbers awaiting a real-play balance pass; enemy art is placeholder shapes; no boss healthbar.
- Meta-progression scope, late-join policy, and max party size are still open decisions.

## 6. Where to look things up

| Question | Open |
|---|---|
| Project rules condensed (auto-loaded by Claude Code) | `CLAUDE.md` |
| Why is the game shaped this way? | `Rogue_Smoke_GDD.md` |
| Authoritative technical reference (read before touching combat/abilities/Mass) | `Rogue_Smoke_MVP_Architecture.md` |
| System overview + authority model | `ARCHITECTURE.md` |
| AngelScript conventions + replication patterns | `CODING_STANDARDS.md` |
| Has this been decided already? (D-0001 … D-0020) | `DECISIONS.md` — **check before re-litigating** |
| What does this term mean exactly? | `GLOSSARY.md` — one name per concept; use these exact terms |
| Engine build / project bring-up | `SETUP.md` |
| Determinism + multiplayer + headless testing detail | `TESTING.md` |
| AS language source of truth | https://angelscript.hazelight.se/ |

**Script folder map** (`RogueSmoke/Script/`): `Core/` (GameMode/GameState/run flow),
`Player/` (hero, controller, locomotion), `AbilitySystem/` + `Weapons/` (GAS abilities, gun),
`Enemies/` (AS attack overrides, Brood-mother), `Upgrades/` (upgrade defs/pool), `Objective/`,
`UI/`, `Debug/` (test ranges, dummies), `Data/`.
**C++ map** (`RogueSmoke/Source/RogueSmoke/`): `Combat/` (the seam),
`Enemies/`, `Spawning/` (SpawnDirector + pooling), `AbilitySystem/` (AttributeSets),
`Core/`, `Loot/`, `UI/`, `VFX/`. Ignore `Variant_Horror/` / `Variant_Shooter/` — UE template
leftovers, not project code.

⚠ **Mass status:** the docs/glossary describe fodder as Mass agents, but the **Mass backend is
not implemented yet** — `UCombatSubsystem` is currently an Actor-only registry
(`AEliteEnemyBase`, see "MVP STATUS" comment in `Combat/CombatSubsystem.h`), and fodder is
`AFodderEnemy` actors pooled by the SpawnDirector. The seam API is shaped so Mass plugs in later
(D-0003) — that's exactly why abilities must never bypass it.

Naming: `A`/`U`/`F`/`E`/`b` prefixes as in Unreal C++; `BP_` Blueprint subclasses (no logic),
`DA_` DataAssets, `DT_` DataTables; RPCs prefixed `Server_`/`Client_`/`Multicast_`.
