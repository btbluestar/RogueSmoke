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
    of a slide clip onto `SK_Mannequin` + a Slide state in `ABP_TP_Rifle` — see animation-pass notes).
    ~~A live `MoveSpeed` attribute-changed callback~~ — since wired (`HeroCharacter.OnMoveSpeedChanged`),
    so MoveSpeed GEs (Swift) take effect live.

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
  full loop with no editor wiring). **Done since this entry:** Spitter projectile + line-of-sight gate
  (`ASpitterProjectile`, `HasLineOfSightToActor`); Lunger smooth multi-frame dash (`StartDash` +
  `DashContactRange`). **Still open / polish:** per-archetype creature art + readability tint; real
  telegraph VFX + attack/death GameplayCues (debug-draw for now); a boss healthbar. Cross-links D-0002
  (seam), D-0003 (Mass fodder), D-0013 (upgrades as GEs), D-0014 (shooter), D-0015 (slide dodges the Lunger).

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

### D-0020 — Behavior evolutions, hero ability tracks, wave director

- **Status:** Decided — spec `docs/superpowers/specs/2026-06-11-upgrade-loop-v3-design.md`.
  Builds on D-0019; delivers the behavior evolutions D-0019 deferred.
- **Decision:** (1) **Behavior evolutions execute hybrid**: hit-path branches compile into
  `UCombatSubsystem::ProcOnHitEffects`, switched by new `URogueCombatSet` attributes
  (`ChainIgniteFraction` = arcs ignite, `ClusterChainBonusArcs` = extra arcs vs Clustered);
  death-path behaviors (`PoisonBurstDps` poison death cloud, `ClusterKillShieldAmount` squad
  shield) run in AngelScript on `USpawnDirector::OnEnemyKilled` — it broadcasts BEFORE pool
  recycle, so corpse DoT/Clustered state is readable. **No generic on-hit event**: that would
  script the per-bullet hot path (D-0002). (2) **Hero ability tracks** gated by
  `URogueUpgradeDef.RequiredHeroClass` (checked in `IsEligible`; null pawn = ineligible).
  Stats → milestone → evolution via D-0019 self-prereqs. Taunt: Magnetic Pull / Iron Grip →
  Concussive Taunt → **Event Horizon** (vortex re-pulls + refreshes Clustered); Barrage:
  High Explosives → Twin Salvo → **Carpet Bombing** (telegraphed strip of 5 pads). Ability
  behaviors are server-side timers in the instanced GAS abilities, switched by flag attributes
  (≥ 1 = on). (3) **Wave director** = pure function `RaidDirector::ComputeWavePlan(level,
  waveIndex, players, tunables)` — no RNG, no world reads — consumed by
  `ARaidObjective.TickFodderWaves`: +0.8 fodder/level, interval −0.35s/level (floor 3.5s),
  elite injections every 3rd wave from level 4 / every 2nd from level 8 (rotation keyed to
  wave index), wave size ×1.5 per extra player, caps raised per extra player. Injected elites
  get `SetCountsAsObjectiveTarget(false)` — pressure, never clear-gates.
- **Mechanics:** 11 new combat-set attributes; two seam additions (`ApplyDotInSphere`,
  `GrantShieldToSquad` — clamps to MaxShield in C++); Toxic Burst applies DoT only (no instant
  damage), so cascades are time-gated by dot ticks and need no recursion guard. Pool 24 → 35
  cards (`Tools/py_make_loop_v3_content.py`).
- **Verification:** `EvoSmoke` (7 behavior checks vs director-spawned dummies) +
  `DirectorReport` (6 pure-function checks) run as a separate SmokeTest boot from
  `UpgradeSmoke` (which pre-sets every flag GE); `UpgradeFlowSmoke` check 7 covers hero gating.
- **Consequences:** synergy cards now transform play, not just stats; Vanguard/Bombardier each
  have a build identity track; raid pressure follows the team power curve. Balance numbers are
  first-pass; Niagara cues for arcs/bursts/vortex remain on the cue-pass backlog.

### D-0021 — Movement & shooting feel pass (Deadlock-lean physics, slide-hop, layered ABP, feedback stack)

- **Status:** Decided — spec `docs/superpowers/specs/2026-06-11-movement-shooting-feel-design.md`,
  plan `docs/superpowers/plans/2026-06-11-movement-shooting-feel.md`. Numbers are live-tunable
  (MoveTune) and may shift in feel sessions; this entry records the architecture + shipped defaults.
- **Decision:**
  1. **Movement physics (Deadlock-lean, stock CMC):** `GravityScale 1.8` (Deadlock ~2.07, Apex
     ~1.46), `JumpZVelocity 750` / second jump 700 (fixed impulse, `JumpMaxHoldTime 0`),
     `MaxAcceleration 6144` (full speed in ~0.15 s), `BrakingDecelerationWalking 3072`,
     `GroundFriction 10` / `BrakingFriction 6`, `AirControl 0.9`, `FallingLateralFriction 0`
     (air keeps slide-hop momentum). Base 600, sprint ×1.6 = 960. This kills the "floaty" feel:
     snappier arcs, instant starts/stops.
  2. **Slide-hop ruleset (Apex/Deadlock):** slide is the fastest ground state — entry needs
     sprint at ≥ 0.85× sprint speed; boost to **1.3× sprint (1248)**, hard cap **1.5× (1440)**;
     **anti-bhop arming**: the boost only applies below 1.1× sprint speed (plus a 1.8 s
     cooldown), so chained slide-hops *carry* speed but can't *build* it. Jumping from a slide
     auto-stands (stock `CanJump` refuses while crouched) and keeps momentum; landing with
     crouch held re-enters the slide. Slide friction 0.6 / braking 256; downhill sustain kept
     from D-0015. **Supersedes the D-0015 slide tunables list** (boost-to-900 et al.).
  3. **Recoil split (Destiny model):** authoritative aim displacement stays on the controller
     (weapon `RecoilPitchPerShot`), while `UCameraFeelComponent` adds a **cosmetic, spring-
     recovered camera kick** (0.4° pitch, ±0.15° yaw, 2.5° stacked clamp, ~0.1 s recovery) as
     position-displacement impulses (frame-rate independent), plus landing dip (0.02 cm per
     cm/s of fall speed, max 22) and sprint/slide FOV bonuses (+5/+8). Camera lag is OFF.
  4. **Layered ABP architecture:** AngelScript `URogueHeroAnimInstance` computes every graph
     variable (speed/direction/play-rate, aim deltas from `BaseAimRotation` so sim proxies
     track, slide/sprint/land state); the user-built **`ABP_Hero`** graph only blends:
     `BS_Rifle_Strafe` (18 samples, walk 466 / jog 955 — measured authored speeds), layered
     blend per bone at `spine_01` (mesh-space rotation), `AO_Rifle` aim offset, `UpperBody`
     montage slot for fire/reload. **Replaces the template `ABP_TP_Rifle`** on both heroes —
     this decouples lower-body locomotion from upper-body aim (the original complaint).
     `AO_Rifle` axes rescaled −1..1 → **−90..90 degrees** (samples at ±90), so `AimPitch`
     wires directly; the D-0015-era `PitchN` sin-wiring note is obsolete with `ABP_TP_Rifle`.
  5. **Live tuning + regression:** `MoveTune [param] [value]` exec (39 locomotion/camera knobs,
     host-only, `MoveTune dump` to bake) and `MoveSmoke` (3-check slide-rule battery: boost,
     cap, anti-bhop) — MoveSmoke runs headlessly in `SmokeTest.ps1` (RaidArena case).
- **Why:** traversal and gunfeel are the moment-to-moment game (GDD); the floatiness diagnosis
  was gravity 1.0 + slow accel + template ABP + camera lag, all four addressed here.
- **Networking:** unchanged authority model — locomotion stays input-event-driven on owner +
  server (D-0015 pattern); all FX/feedback RPCs are cosmetic `Unreliable` multicasts/client
  RPCs; damage numbers and kill confirms go **only to the owning shooter** (`Client_` RPCs).
- **Verification:** SmokeTest 9/9 incl. MoveSmoke 3/3; PIE battery (run/strafe/jump/land/
  sprint/slide anim vars, AimPitch tracking, UpperBody montages) on `ABP_Hero`.
- **Consequences / deferred:** jump air time dropped ~40% vs the old gravity-1.0 arc — enemy
  telegraph windows (Lunger lunge, Bloater blast) need a dodge-feel check; **flag for a balance
  pass, do not retune enemies in this workstream**. Niagara/audio asset slots on
  `URogueWeaponDefinition` (muzzle/tracer/impacts, fire/tail/reload/hit-tick/kill-confirm) are
  null-safe and empty until a Lyra-content/free-pack asset drop. Turn-in-place, hitstop, crit
  styling, footstep foley remain deferred.

### D-0022 — Lyra animation stack adopted (linked layers via CoreRedirect re-parent, skeleton swap)

- **Status:** Decided — spec `docs/superpowers/specs/2026-06-12-lyra-anim-migration-design.md`,
  plan `docs/superpowers/plans/2026-06-12-lyra-anim-migration.md` (execution log inline).
  v1 hand-built stack (`ABP_Hero`, `BS_Rifle_Strafe`, GASP-retargeted clips) stays on disk,
  unhooked, until the user's parity sign-off; then a cleanup commit retires it.
- **Decision:**
  1. **Heroes run Lyra's production anim stack**: `ABP_Mannequin_Base` (distance matching,
     turn-in-place/RootYawOffset, cardinal strafes) + `ALI_ItemAnimLayers` linked-layer system
     with `ABP_RifleAnimLayers` linked at spawn (`Mesh.LinkAnimClassLayers`); future weapons =
     link a different layer asset, data not graph work. Hero mesh/skeleton swapped to Lyra's
     full-detail `SKM_Manny` / `SK_Mannequin` (UE5-mannequin hierarchy — no retargets for Lyra
     content, ever).
  2. **CoreRedirects are the migration mechanism, not graph surgery**: ini ClassRedirects map
     `LyraAnimInstance` → our AS `URogueHeroAnimInstance` (the ABP re-parents at load; graph
     compiles with ZERO errors after deleting 7 shadowed BP vars), and the ContextEffects
     classes → our C++ ports. **StructRedirects are required separately** for any renamed
     USTRUCT a copied asset serializes (learned the hard way: library entries deserialized
     empty without them).
  3. **Parent surface stays thin**: the graph computes locomotion data itself from the pawn;
     our AS instance supplies only `GroundDistance` (airborne trace) + `GameplayTag_*` bools
     (IsFiring/IsADS/IsDashing/... from replicated state) + `AimPitch`/`AimYaw` (now
     BlueprintReadWrite — the Lyra graph writes them per-frame in its own instance).
  4. **Slide stays ours**: GASP slide set retargeted onto the Lyra skeleton
     (`Animations/Slide/MM_Slide_*`, via GASP's own `RTG_UEFN_to_UE5_Mannequin`), played as
     **dynamic montages on DefaultSlot** from a Tick edge-detector on `Locomotion.IsSliding()`
     — zero Lyra-graph modifications. v1's actor-level idle free-look (`TickFacing`) is gated
     OFF (`bActorLevelFreeLook=false`): RootYawOffset + authored turn anims own feet-planted
     idle now.
  5. **Guns are real**: `DA_Weapon_AssaultRifle` gained `WeaponMesh = SK_Rifle` (the v1 slot
     was EMPTY — no visible gun ever), `WeaponAnimClass = ABP_Weap_Rifle` (animated bolt/mag),
     weapon-mesh fire/reload montages; character montages swapped to `AM_MM_Rifle_*`; grip
     prefers the authored `weapon_r` socket. **`bFullAuto` was False in the DA** — fixed; with
     the Tick fix below this is why held-LMB full-auto never worked.
  6. **Lyra FX/audio fill the (previously all-empty) FireFX slots**: layered
     `MSS_Weapons_Rifle_Fire` MetaSound, `NS_WeaponFire_MuzzleFlash_Rifle`, tracer, concrete
     impact, AutoRifle tail wave. ContextEffects (5-class C++ port in
     `Source/RogueSmoke/Feedback/ContextEffects/`) drives surface-aware footsteps from the
     notifies already authored on Lyra's anims (tag tables `DT_AnimEffectTags`/`DT_SurfaceTypes`
     registered; surface→context map in DefaultGame.ini). Heat→spread gained an optional
     Lyra-shaped piecewise curve (`HeatToSpreadCurve`, flat-early/steep-late; empty = legacy
     linear lerp).
  7. **Plugins enabled**: `AnimationLocomotionLibrary`, `AnimationWarping`, `Metasound`.
- **Root-cause fix recorded:** the hero BPs carried template EventGraph cruft — an unconnected
  `Event Tick` node had been **silently swallowing the AS `AHeroCharacter.Tick` since v1**
  (locomotion tick, idle facing, camera-feel tick, full-auto refire never ran), plus a BP-side
  `AddMappingContext` and an `IA_Move → DoMove` double input path. All purged; hero BPs are
  pure asset assignment again (see bp-parent-class gotcha).
- **Verification:** ABP chain compiles clean on the AS parent; machine probes green
  (turn-in-place RootYawOffset −120→−7.5 under smooth yaw with feet <3 cm drift;
  DisplacementSpeed==GroundSpeed at run; GroundDistance feeds distance-matched landings; slide
  In→Loop→Out montages live; char+gun fire montages per shot; full-auto empties the mag with
  auto-reload mid-hold; footstep audio components spawn on the mesh while running); SmokeTest
  9/9 per phase. User feel checkpoints: `docs/superpowers/plans/2026-06-12-lyra-checkpoint-A.md`
  (+B at finish).
- **Consequences / deferred:** known sim-proxy gaps for the replication pass (remote slide
  montage is owner+server-gated; `GameplayTag_IsFiring` doesn't reach simulated proxies — fire
  montages multicast so shots still animate); slide-in is left-foot-only and the In→Loop
  handover is untested on long slides; tracer endpoint param not wired (steered by spawn
  rotation); `ImpactEnemyFX` deliberately empty (no Lyra flesh FX); Lyra dynamic audio mixing,
  accolades, Quinn body remain catalogued gems.

### D-0023 — Stamina pips (Deadlock model) on GAS attributes

- **Status:** Decided — part of the D-0022 workstream (spec §Phase 6).
- **Decision:** 3 discrete stamina pips; **slide costs 1, slide-hop's jump costs 1, sprint is
  free** (gating sprint would fight the movement identity). Pips regen one at a time
  (`StaminaRegenSeconds 2.5`, post-spend pause `StaminaRegenDelay 1.0`; both MoveTune knobs).
  `Stamina`/`MaxStamina` live on **`URogueMovementSet`** (C++ `UAngelscriptAttributeSet`,
  granted via both hero `DA_*_AbilitySet` assets) so future meta-progression upgrades are plain
  GameplayEffects on `MaxStamina`/regen — same shape as every other card.
- **Authority:** server spends (slide-start edge in `TickSlideAnim`; slide-jump via
  `DoJump`/`OnJumped` paths) and regens; the owning client predicts the gate by reading the
  replicated attribute (out of pips → crouch instead of slide, input never eaten).
- **UI:** HUD pip row under the health bar (accent color, spent = 25 % alpha); pip count tracks
  `MaxStamina` so +1-pip upgrades render automatically.
- **Verification:** MoveSmoke check 4 (spend/restore atomic, battery now 4/4 — SmokeTest gate
  updated); live probe: slide spends 3→2, regen returns to 3.

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
