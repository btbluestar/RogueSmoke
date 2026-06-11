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
- **Status:** ~~Decided~~ **SUPERSEDED by D-0014.**
- **Decision:** ~~Top-down camera (spring arm, ~-60° pitch, fixed rotation).~~
- **Why:** ~~Maximizes readability of swarms and cross-player setups — the two hardest pillars —
  at lowest cost. GDD §9; implemented in MVP arch §6.~~ Reversed in favour of a third-person
  shooter camera for the desired *feel* — see **D-0014**.
- **Consequences:** ~~Controller-first input is viable; art/readability budget assumes a top-down read.~~

### D-0006 — Ability framework: custom component for MVP, defer GAS
- **Status:** ~~Decided (MVP scope)~~ **SUPERSEDED by D-0013.**
- **Decision:** ~~Use a lightweight custom `UAbilityComponent` (cooldown + server/multicast
  plumbing) for the vertical slice. Re-evaluate Gameplay Ability System later.~~
- **Why:** ~~Faster to iterate in script; GAS is heavy for a first slice.~~ The custom component
  was replaced wholesale by GAS once we confirmed the engine fork ships the **AngelscriptGAS**
  plugin, which lets us write abilities/granting in AngelScript (so GAS is no longer "heavy C++").
- **Consequences:** The custom `UAbilityComponent`/`UStatsComponent`/`UUpgradeEffect` were deleted.
  See **D-0013**.

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

### D-0011 — Core run-state layer in AngelScript (RunManager / GameState / GameMode)
- **Status:** Decided
- **Decision:** The run lifecycle's front half (ARCHITECTURE §3) is implemented in AngelScript
  under `Script/Core/`: `ARaidGameState` (replicated seed/phase/floor/score), `URunManager`
  (server-only seed roll + state machine owning the seeded `FRandomStream`), `ARaidGameMode`
  (creates RunManager, rolls the seed on `BeginPlay`), and `ALobbyGameMode`.
- **Why:** The seeded-generation pillar (D-0007) and the floor loop need an authoritative owner;
  it belongs in the script layer (D-0002) since it's iteration-heavy run logic, not hot-path sim.
  Lyra's GameModes/Experiences were the *pattern* reference, reimplemented in AngelScript rather
  than ported. (Abilities/attributes themselves *do* now copy Lyra onto GAS — see **D-0013** — but
  via the AngelscriptGAS plugin, so the bulk stays in script per D-0002.)
- **Consequences:** Deterministic generators must draw from `URunManager::GetStream()`, never
  unseeded global random. Clients read run state only from the replicated `ARaidGameState`.

### D-0012 — Multiplayer connection: LAN / direct IP first (NULL subsystem)
- **Status:** Decided (MVP scope; revisit for shipping)
- **Decision:** Host/join goes through `URaidSessionSubsystem` (a `UGameInstanceSubsystem`):
  host = `OpenLevel` with `?listen`, join = `ClientTravel` to a typed address, over the
  built-in NULL online subsystem. Menu → lobby → run flow via `UMainMenuWidget` /
  `ULobbyWidget` (logic) + BP widgets (layout). Lobby → raid uses `ServerTravel` to keep
  connections.
- **Why:** Zero external dependencies, works today, proves the listen-server topology (D-0004)
  without Steam/EOS setup. Lyra's CommonUser was the pattern reference, not a dependency.
- **Consequences:** Friends join by IP for now. Swapping to Steam/EOS later touches **only**
  `URaidSessionSubsystem` — host/join callers don't change. Content wiring (maps, BP subclasses,
  default map) is documented in `MULTIPLAYER_SETUP.md`.

### D-0013 — Abilities/attributes/upgrades on GAS, Lyra-style (supersedes D-0006)
- **Status:** Decided
- **Decision:** Adopt Unreal's **Gameplay Ability System** for all player abilities, attributes,
  and roguelike upgrades, copying **Lyra** patterns instead of inventing systems. Implementation:
  - **ASC on the PlayerState** (Lyra-style; survives pawn respawn). Player ASC = the stock
    `UAngelscriptAbilitySystemComponent`.
  - **Thin C++ layer only** (because `IAbilitySystemInterface` can't be implemented in AngelScript):
    `ARoguePlayerState` (owns the ASC), `ARogueHeroBase` (forwards `GetAbilitySystemComponent`,
    inits actor info on possess/`OnRep_PlayerState`, fires `OnAbilitySystemReady`), and the
    **attribute sets in C++** (`URogueHealthSet`, `URogueCombatSet`) subclassing
    `UAngelscriptAttributeSet`.
  - **Everything else in AngelScript** (~80%): abilities (`UGA_RogueAbility` → `UGA_Taunt`/
    `UGA_Barrage` over `UAngelscriptGASAbility`), data-driven granting/input (`URogueAbilitySet`,
    `URogueInputConfig` — AS ports of `ULyraAbilitySet`/`ULyraInputConfig`), and upgrades
    (`URogueUpgradeDef` carrying a `UGameplayEffect`).
- **Why:** The engine fork ships the **AngelscriptGAS** plugin, so GAS is no longer "heavy C++"
  (the D-0006 rationale). GAS gives cooldowns/costs/effects/cues/replication for free and matches
  the Lyra reference the project already cites. Stats-as-attributes makes upgrades just
  GameplayEffects (e.g. Chain Detonation adds to `BarrageRadiusBonus`/`BarrageClusterBonus`).
- **Consequences:** Deleted the custom `UAbilityComponent`, `UStatsComponent`, `UUpgradeEffect`,
  and the taunt/barrage components (D-0006). The C++/Mass combat seam (`UCombatSubsystem`, D-0002)
  is unchanged — GAS abilities call it server-side. Ability **activation is server-initiated** via
  the pawn's `Server_ActivateAbilityInput` RPC (`NetExecutionPolicy = ServerOnly`); GAS input
  prediction can be added later. Content to author in-editor: BP abilities (with Cooldown GEs),
  `DA_*_AbilitySet`/`DA_*_InputConfig`, the upgrade GEs, and GameplayCues for cosmetics. Player
  damage intake should become a damage GameplayEffect against the player ASC (open follow-up).

### D-0014 — Camera: third-person shooter, strafe/aim-locked (supersedes D-0005)
- **Status:** Decided
- **Decision:** Third-person over-shoulder **shooter** camera, **strafe/aim-locked**. Spring arm
  behind the character (`bUsePawnControlRotation = true`, short arm with a shoulder
  `SocketOffset`); the character faces the control/aim rotation
  (`bUseControllerRotationYaw = true`, `CharacterMovement.bOrientRotationToMovement = false`) and
  moves strafe-relative. Reference: the engine fork's **`F:\UEAS\Templates\TP_ThirdPerson`** template
  (read-only) and Epic's [Third Person Template docs](https://dev.epicgames.com/documentation/unreal-engine/third-person-template-in-unreal-engine),
  adjusted from orient-to-movement to aim-locked strafing.
- **Why:** The desired *feel* is an action shooter, not an arena twin-stick. D-0005 chose top-down
  purely for swarm/synergy readability; we accept the tradeoff and protect those pillars another way
  (below) rather than constrain the camera.
- **Consequences:**
  - **Readability is now an active risk, not a free win.** Swarm density and cross-player setup
    legibility — the two hardest pillars (GDD §7, §11; D-0008 synergy) — are harder to read from
    behind the shoulder than from overhead. Mitigate with strong silhouettes, off-screen/edge
    indicators for teammates and clustered enemies, AoE/telegraph ground decals, and audio
    callouts for "setup ready"/"payoff landed" (GDD §10). Validate the **taunt→barrage** combo still
    reads at speed (D-0008) in this view before adding content.
  - **Aiming is first-class.** Right-stick / mouse drives both character yaw and camera; an aim
    `InputAction` (and likely an ADS state) joins the InputConfig. Controller-first is no longer
    assumed — mouse-aim is a primary path.
  - **Code:** `Script/Player/HeroCharacter.as` still carries the old top-down `CameraBoom`
    (`TargetArmLength = 1500`, `RelativeRotation = -60°`) / `TopDownCamera`; it must be rewired to
    the third-person values above (boom behind char, pawn control rotation, shoulder offset, follow
    camera). Tracked as a follow-up — docs updated first.

---

### D-0015 — Movement: jump/double-jump + sprint + slide (AngelScript MVP)
- **Status:** Decided
- **Decision:** A mobility kit drawing on **Apex/Deadlock** feel — hold-to-**sprint** (omnidirectional),
  **crouch**, momentum **slide**, and **double-jump** — built **in AngelScript on the stock
  `UCharacterMovementComponent`** (no custom C++ movement mode for the MVP). Lives in a composition
  component `URogueLocomotionComponent` (`Script/Player/LocomotionComponent.as`) on `AHeroCharacter`;
  input is wired on `ARaidPlayerController` (Space/Shift/Ctrl → `IA_Jump`/`IA_Sprint`/`IA_Crouch`).
- **Why:** The game is a third-person shooter (D-0014); traversal *is* the moment-to-moment feel.
  AngelScript keeps it fast to iterate, and the mechanics are all reachable from script
  (`MaxWalkSpeed`, `GroundFriction`, `BrakingDecelerationWalking`, `JumpMaxCount`, `Jump`,
  `Crouch`/`UnCrouch`) — no engine edits, `F:\UEAS` stays read-only.
- **Networking:** Driven input-event style on the **locally-controlled client and the server** (same
  pattern as weapon fire), so stock CMC prediction reconciles. Jump/crouch are natively net-aware;
  sprint is a `MaxWalkSpeed` change; the slide entry impulse is **idempotent** (sets horizontal speed
  to `max(current, boost)`, never additive) so running on both sides can't double-apply. Accepted
  tradeoff: minor correction possible on a remote client under heavy lag — the **host** (authority +
  local) and single-player are exact.
- **Meta-progression hook (not wired yet):** every knob (sprint multiplier, `MaxJumpCount`, jump
  height, slide boost/friction/duration) is a tunable field re-applied via `ApplyGroundSpeed()`. A
  later upgrade GE (D-0013) bumps `MoveSpeed`/these fields and re-applies. `MaxJumpCount` defaults to
  **2** now (double-jump playable); when meta lands it becomes an unlock starting at 1.
- **Consequences:**
  - Verified in PIE: sprint 600↔960, crouch (capsule + speed), slide engages (friction 0.4, boost to
    900, carries) and auto-ends restoring friction, double-jump caps at 2.
  - **Downhill slides sustain (Apex-style):** `SlideMaxDuration` (1.5s) only burns down on flat/uphill
    ground. While genuinely descending (horizontal floor-normal ≥ `SlideSustainMinSlope` *and* moving
    down it) the timer refreshes, so a long ramp keeps you sliding instead of popping you upright
    mid-slope; the slide still ends when the slope flattens and speed bleeds below `SlideMinSpeed`, on
    release, or on leaving the ground. (Downhill accel along the slope was already applied.)
  - **Slide netcode upgrade path:** if remote-client slide feel proves insufficient, port to a C++
    `UCharacterMovementComponent` subclass with a `MOVE_Custom` slide mode + `FSavedMove` prediction
    (the "real" Apex/Deadlock approach) — isolated to C++, no AngelScript API change.
  - **Deferred:** wall-run/mantle; a dedicated slide animation pose (needs a cross-skeleton retarget
    of a slide clip onto `SK_Mannequin` + a Slide state in `ABP_TP_Rifle` — see animation-pass notes);
    a live `MoveSpeed` attribute-changed callback (base speed is currently seeded once on possession).

### Animation pass (aim offset) — fix
- **Aim offset now driven:** `ABP_TP_Rifle` computed `PitchN` (= sin of aim pitch, −1..1) every frame
  but never wired it to the `AO_Rifle` aim-offset node, so the body always aimed flat regardless of
  look pitch. Wired `PitchN` → the node's **Y** pin (AO_Rifle axes are −1..1, vertical = pitch).
  Verified in PIE: right-hand height swings ~41 cm across the aim range (was ~1 cm / dead-flat). Note
  bullets already traced from the camera, so only the *pose* was wrong. Also switched **BP_Bombardier**
  from `ABP_Unarmed` → `ABP_TP_Rifle` (it's a shooter too; was missing the aim offset entirely).

---

### D-0016 — UI framework: CommonUI for menus (refines D-0012)
- **Status:** Decided
- **Decision:** Adopt Epic's **CommonUI** plugin for the menu/front-end layer. The script UI
  controllers `UMainMenuWidget` / `ULobbyWidget` (`Script/UI/`) now extend
  **`UCommonActivatableWidget`** (was `UUserWidget`), so screens are pushed/popped on a
  `UCommonActivatableWidgetStack` and the **Back** action routes natively. Buttons use
  `UCommonButtonBase` (starter `WBP_RogueButton` + `B_RogueButtonStyle`).
- **Why:** CommonUI gives input-method-aware focus, gamepad/keyboard navigation, an action bar, and
  a clean activatable-stack model for the menu→lobby→run flow (D-0012) — instead of hand-rolling
  focus/back handling on plain `UUserWidget`s. It's the same stack Lyra's front-end uses, matching
  the project's Lyra-pattern reference (D-0011/D-0013).
- **Setup (in repo):** plugin enabled in `RogueSmoke.uproject`; **Input Data** is a
  `TSoftClassPtr<UCommonUIInputData>` (a *class*, not a Data Asset) → `B_RogueUIInputData` (Blueprint
  parented to `CommonUIInputData`) backed by `DT_CommonInputActions` (Click = Enter/FaceButton_Bottom,
  Back = Esc/FaceButton_Right); wired in `Config/DefaultGame.ini`
  (`[/Script/CommonInput.CommonInputSettings] InputData=…/B_RogueUIInputData_C`). Assets under
  `/Game/UI/` (`WBP_MainMenu`, `WBP_Lobby`, `WBP_RogueButton`, `B_RogueButtonStyle`) and
  `/Game/UI/Input/`.
- **Consequences:** Per house rules, **no `ICommon*` interfaces from AngelScript** — only the base
  classes (`UCommonActivatableWidget`/`UCommonUserWidget`/`UCommonButtonBase`), which are fully
  reflected and bound. UMG layout for `WBP_MainMenu`/`WBP_Lobby` (buttons + IP box / player count +
  Start) is still authored in the designer. A `CommonUI` UI-manager/HUD that owns the activatable
  stack and pushes the initial screen is the follow-up wiring. D-0012's host/join subsystem flow is
  unchanged — only the widget base class and front-end plumbing.

---

### D-0017 — Enemy roster + attack/damage model (bio-horde)
- **Status:** Decided — resolves the GDD §128–131 enemy TBDs (theme / damage model / aggro).
- **Decision:** Theme = **bio-horde creatures**. Roster by role: **Crawler** (swarm fodder, contact
  melee — C++ `AFodderEnemy`, Mass-bound later) plus AngelScript elites over a C++ **`AAttackingElite`**
  base: **Carapace** (tanky radial-slam shield elite — the taunt/cluster synergy anchor), **Spitter**
  (ranged kiter), **Bloater** (suicide-bomber radial, blast on contact-or-death), **Lunger** (telegraph
  → lunge gap-closer), **Brood-mother** (mini-boss: spit / summon-wave / artillery-AoE). Damage model =
  **melee + telegraphed radial/ranged**; hitscan stays the *player* weapon's model. Aggro = nearest
  **living** hero (GAS Health > 0), taunt overrides.
- **Damage seam:** enemy→player damage goes through a new outbound seam —
  `UCombatSubsystem::ApplyDamageToPlayer` / `ApplyRadialDamageToPlayers` — applying an instant Damage GE
  to the hero ASC (armor/shield/health resolve in `RogueHealthSet`). It is the mirror of `FireHitscan`.
  Every attack is **telegraphed** (GDD §10 readability); the wind-up is the player's counterplay window.
- **Why:** GDD wants fodder + synergy-elites + a boss, with density and telegraphing. `AAttackingElite`
  is self-contained (visible body + Visibility-blocking collision, like `AFodderEnemy`) so elites are
  visible/hittable with no Blueprint; the per-archetype attack is a `BlueprintNativeEvent` that
  AngelScript overrides — "script the decision, compile the simulation" (D-0002).
- **Consequences:** elites count toward "clear the arena", fodder does not (`bCountsAsObjectiveTarget`).
  `RaidObjective` spawns a seeded elite mix + boss at raid start (defaults to this roster, so a raid is a
  full loop with no editor wiring). **Open / polish:** per-archetype creature art + readability tint;
  real telegraph VFX + attack/death GameplayCues (debug-draw for now); projectile + line-of-sight for
  Spitter; smooth dash for Lunger; a boss healthbar. Cross-links D-0002 (seam), D-0003 (Mass fodder),
  D-0013 (upgrades as GEs), D-0014 (shooter), D-0015 (slide dodges the Lunger).

### D-0018 — Upgrade acquisition loop: shared XP levels + mini-boss chest

- **Status:** Decided — user's UpgradeLoop concept (flowchart, 2026-06-11). Supersedes the
  interim "one offer on arena clear" flow from D-0013 (the clear offer remains as a bonus).
- **Decision:** Two in-raid acquisition paths.
  **(1) Shared XP levels:** every kill feeds ONE team pool (`XPValue` per archetype: fodder 5,
  elites 25, boss 150; replicated on `ARaidGameState` as TeamLevel/TeamXP/XPToNextLevel). Each
  level-up **pauses the raid for all players** and presents a 3-card pick; rarity is
  level-weighted — default common-leaning, every 5th level boosts moderate (r2), every 10th
  boosts rare (r3). **(2) Mini-boss chest:** the Brood-mother drops an `AUpgradeChest` where it
  fell; any living player standing next to it opens it → paused pick rolled ONLY from
  **synergy upgrades** (`bSynergyUpgrade` on `URogueUpgradeDef`; level offers exclude them).
- **Mechanics:** pause = `UGameplayStatics::SetGamePaused` via `RogueGame::SetRaidPaused`
  (WorldSettings pauser replicates → clients pause; UI input + RPCs still work). Resume when
  every player picked, or a GameMode watchdog timeout (30s, ticks-while-paused). Kill event =
  `USpawnDirector::OnEnemyKilled` (broadcast before pool recycle). Rolls stay seeded
  (fresh stream salted by offer index — CODING_STANDARDS §5).
- **Consequences:** builds grow DURING the fight (a pick every ~2 fodder waves at current
  tuning); the synergy pool needs more cards (only Chain Detonation today); XP curve + weights
  are balance-pass material; HUD shows `LVL n — x/y XP`. Cross-links D-0013 (upgrades as GEs),
  D-0017 (roster XP values).

### D-0019 — Upgrade loop v2: per-player hands, stacks/prereqs, squad eligibility

- **Status:** Decided — spec `docs/superpowers/specs/2026-06-11-upgrade-loop-v2-design.md`
  (research: LoL Swarm primary; VS / RoR2 / Brotato / Hades survey). Builds on D-0018.
- **Decision:** (1) **Per-player hands** — each player's 3 cards roll from a fresh stream salted
  by offer index AND PlayerArray index; picks validated against the offered hand server-side.
  (2) **Stacks/caps/prereqs on `URogueUpgradeDef`** — `MaxStacks` (≤0 = unlimited), self-scope
  prereqs gate **milestone** modifier cards (guaranteed hand slots once eligible), squad-scope
  duo prereqs (A and B on two different players; relaxed to one player solo) gate **synergy**
  cards. (3) **Pick-flow**: watchdog now AUTO-PICKS card 0 (offer honored, not lost); 1
  squad-shared reroll per raid; short hands pad from a `UtilityPool` (squad heal / filler);
  offers requested mid-pick queue (one deep). (4) **Rarity floors+caps** replace the %5/%10
  spikes: r2 from team level 3 (cap 60), r3 from level 6 (cap 25), commons floored at 10.
  XP curve front-loaded (base 50, growth 35; ~8–12 levels per raid).
- **Mechanics:** all per-player state is server-only `FPlayerUpgradeRecord`s on the GameMode;
  the UI gets stack counts in the offer RPC payload; `AwaitingPickNames`/`SquadRerollsRemaining`
  replicate on `ARaidGameState`. `bApplyToSquad` applies a card's GE to every hero's ASC
  (synergy/team cards). Verification: `UpgradeFlowSmoke` exec battery in SmokeTest.ps1.
- **Consequences:** synergy pool now has 5 duo-gated cards; milestone pairs exist for the
  pierce and chain tracks; full Swarm-style behavior evolutions (new weapon mechanics) remain
  future work; balance numbers are first-pass.

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
