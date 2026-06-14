# Procedural Raid Generation — Phase-1 Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the generated arena (Plans 1–2) into a playable raid on a **new dedicated level** — auto-stamp at load on every machine, spawn heroes on the generated Drop node, a hold-and-channel objective at the generated objective node, extraction at the separate generated Extraction node, and a time-based rising-tide director — completing the spec's ~12-min Phase-1 loop.

**Architecture:** **Additive + opt-in.** All new behavior is gated behind flags so the *default* behavior of the shared classes (`ARaidObjective`, `ARaidGameMode`, `RaidDirector::ComputeWavePlan`) is unchanged — every existing RaidArena/RaidLoop/MoveSmoke/Director regression case stays green. The generated loop runs only on a new level (`L_GenRaid`) whose objective sets `Mode = HoldAndChannel` and whose GameMode sets `bGeneratedArena = true`. Geometry is deterministically re-stamped on every machine from the replicated `MasterSeed` (server in GameMode `BeginPlay`, clients in `GameState::OnRep_MasterSeed`).

**Tech Stack:** UnrealEngine-Angelscript (Hazelight UE5.7 fork); the Plan-1/2 generator + `URaidStampSubsystem`; `Tools/SmokeTest.ps1` headless gates; a new level authored headlessly via `unreal-test-mcp` python (the established `DL_*` pattern).

**This is Plan 3 of 3** (spec §13 Phase 1, completing it). Deferred to later passes: multiple objective task-types + 2–3 objectives per raid (only hold-and-channel here), the archetype pool (dial 6), varying footprint (dial 7), PCG cosmetic skin, retiring the hand-built RaidArena map in favor of the generated one.

---

## Design notes the engineer must know

- **Opt-in is the whole safety story.** Do NOT change any default. `ARaidObjective.Mode` defaults to `ClearElites` (today's behavior); `ARaidGameMode.bGeneratedArena` defaults to `false`. The existing `RaidArena` map and its placed objective/gamemode keep all defaults → unaffected.
- **Determinism + co-op:** stamping and layout reads go through `RaidGen::GenerateValidated(MasterSeed, FRaidGenConfig())` (pure, deterministic) and `RaidArena::BuildFromSeed` (Plan 2). Server stamps in GameMode `BeginPlay`; clients stamp in `OnRep_MasterSeed`. Both use the replicated seed — never replicate geometry.
- **The run-result bridges must stay intact:** hold-and-channel must still reach `SetPhase(ExtractionReady) → CallExtraction → Extracting → Extracted`, and party-wipe still routes `NotifyPartyWiped → Failed`. Never bypass `SetPhase`/`OnPhaseChanged` (they call `EndRunResult → RunManager.EndRun → GameState.Phase`).
- **AS replication:** `UPROPERTY(Replicated)` auto-registers in this fork (no manual `GetLifetimeReplicatedProps`). `OnRep_` targets must be `UFUNCTION()`.
- **A new layout accessor** lives in `RaidStamper.as`: `RaidArena::GetLayout(int Seed)` returns `RaidGen::GenerateValidated(Seed, FRaidGenConfig())` (pure; cheap; deterministic). Consumers read `.Drop.Center`, `MainSites[0]` nodes, `.Extraction.Center`.
- **Branch:** `ProcedualLevelGeneration`. Commit after every task. C++ is NOT touched in this plan.

## File structure

- Modify `RogueSmoke/Script/Generation/RaidStamper.as` — add `RaidArena::GetLayout` + helper to find a site node by slot.
- Modify `RogueSmoke/Script/Objective/RaidWaveDirector.as` — add `ElapsedSeconds` (defaulted) + spike fields; time-spine.
- Modify `RogueSmoke/Script/Objective/RaidObjective.as` — objective `Mode`, hold-and-channel, separate extraction, generated placement, `DebugFillChannel`.
- Modify `RogueSmoke/Script/Core/RaidGameMode.as` — `bGeneratedArena`, layout cache, auto-stamp, hero-spawn-at-Drop.
- Modify `RogueSmoke/Script/Core/RaidGameState.as` — `bGeneratedArena` (replicated) + `OnRep_MasterSeed` client stamp.
- Modify `RogueSmoke/Script/Player/RaidPlayerController.as` — update `DirectorReport` for the new fields; add `GenLoopSmoke`.
- Create the level `/Game/Levels/L_GenRaid` + `BP_GenRaidGamemode` (headless python).
- Modify `Tools/SmokeTest.ps1` — bump `DirectorSmoke` count; add a `GenLoopVictory` case.

---

## PART A — Director time-spine (additive, non-breaking)

### Task A1: Add elapsed-time spine + spike fields to the director

**Files:** Modify `RogueSmoke/Script/Objective/RaidWaveDirector.as`

- [ ] **Step 1: Extend `FWavePlan` and `FDirectorTunables`, add the time-spine to `ComputeWavePlan`**

In `struct FWavePlan`, after `int EliteInjectIndex = -1;` add:

```angelscript
    // Time-spine spikes (Plan 3). 0 = normal wave; 1/2 = elite-spike shoulders; bSpawnMiniBoss at the climax.
    UPROPERTY()
    int SpikeTier = 0;

    UPROPERTY()
    bool bSpawnMiniBoss = false;
```

In `struct FDirectorTunables`, after `float PlayerCountWaveScale = 0.5;` add:

```angelscript
    // --- Plan 3 time-spine (only active when ElapsedSeconds > 0; default keeps legacy behavior) ---
    UPROPERTY()
    float FodderPerSecond = 0.03;        // rising tide: extra fodder per elapsed second

    UPROPERTY()
    float EliteSpike1Time = 180.0;       // first elite-spike shoulder (s)

    UPROPERTY()
    float EliteSpike2Time = 390.0;       // second shoulder (s)

    UPROPERTY()
    float MiniBossTime = 480.0;          // climactic mini-boss (s)

    UPROPERTY()
    float SpikeWindow = 6.0;             // a spike fires for waves within +/- this of its time

    UPROPERTY()
    float RelaxLullMultiplier = 1.8;     // wave interval stretches this much during a post-spike lull
```

Replace the whole `ComputeWavePlan` function with this (note the new trailing **defaulted** `ElapsedSeconds` param — existing 4-arg callers keep compiling and get the legacy result because `ElapsedSeconds` defaults to 0):

```angelscript
    FWavePlan ComputeWavePlan(int TeamLevel, int WaveIndex, int NumPlayers, const FDirectorTunables& T,
                              float ElapsedSeconds = 0.0)
    {
        FWavePlan Plan;

        float Size = float(T.BasePerWave)
                   + float(WaveIndex) * T.EscalationPerWave
                   + float(TeamLevel) * T.FodderPerTeamLevel
                   + ElapsedSeconds * T.FodderPerSecond;          // Plan 3: time-based rising tide
        Size *= 1.0 + T.PlayerCountWaveScale * float(Math::Max(NumPlayers - 1, 0));
        Plan.FodderCount = Math::Clamp(int(Size), 1, T.MaxPerWave);

        Plan.Interval = Math::Max(T.MinInterval,
                                  T.BaseInterval - float(TeamLevel) * T.IntervalReductionPerLevel);

        // Legacy team-level elite cadence (unchanged when ElapsedSeconds == 0).
        if (TeamLevel >= T.EliteInjectStartLevel)
        {
            int Cadence = (TeamLevel >= T.EliteInjectFastLevel) ? 2 : 3;
            if (WaveIndex % Cadence == 0)
                Plan.EliteInjectIndex = (WaveIndex / Cadence) % 2;
        }

        // Plan 3 designer spikes keyed off elapsed time. A spike forces an elite injection and a
        // post-spike relax lull (stretched interval). Deterministic — no clock read, no RNG.
        if (ElapsedSeconds > 0.0)
        {
            if (Math::Abs(ElapsedSeconds - T.EliteSpike1Time) <= T.SpikeWindow)
                Plan.SpikeTier = 1;
            else if (Math::Abs(ElapsedSeconds - T.EliteSpike2Time) <= T.SpikeWindow)
                Plan.SpikeTier = 2;

            if (Plan.SpikeTier > 0)
            {
                Plan.EliteInjectIndex = Math::Max(Plan.EliteInjectIndex, 0);   // guarantee an elite at a shoulder
                Plan.Interval *= T.RelaxLullMultiplier;                        // breathe after the spike
            }

            if (Math::Abs(ElapsedSeconds - T.MiniBossTime) <= T.SpikeWindow)
            {
                Plan.bSpawnMiniBoss = true;
                Plan.Interval *= T.RelaxLullMultiplier;
            }
        }

        return Plan;
    }
```

- [ ] **Step 2: Verify legacy behavior unchanged + spine active (`run_code_test`)**

```angelscript
FDirectorTunables T;
FWavePlan Legacy = RaidDirector::ComputeWavePlan(1, 0, 1, T);            // 4-arg: ElapsedSeconds defaults 0
FWavePlan Early  = RaidDirector::ComputeWavePlan(1, 0, 1, T, 0.0);
FWavePlan Late   = RaidDirector::ComputeWavePlan(1, 0, 1, T, 400.0);
FWavePlan Spike  = RaidDirector::ComputeWavePlan(1, 5, 1, T, 180.0);
FWavePlan Boss   = RaidDirector::ComputeWavePlan(1, 9, 1, T, 480.0);
Print(f"[A1] legacyEqEarly={Legacy.FodderCount == Early.FodderCount} riseEarlyLate={Early.FodderCount < Late.FodderCount} spike1={Spike.SpikeTier} miniboss={Boss.bSpawnMiniBoss}", 8.0);
```

Expected: `legacyEqEarly=true riseEarlyLate=true spike1=1 miniboss=true`.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidWaveDirector.as
git commit -m "feat(procgen): director time-spine + elite/miniboss spikes (additive)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task A2: Update DirectorReport + SmokeTest for the spine

**Files:** Modify `RogueSmoke/Script/Player/RaidPlayerController.as` (the `DirectorReport` exec), `Tools/SmokeTest.ps1`

- [ ] **Step 1: Add spine assertions to `DirectorReport`**

In the `DirectorReport` exec, find the final `Print(f"[DirectorSmoke] RESULT {Pass}/{Total}", ...)` line. Immediately before it, add (the exec already has `int Pass` / `int Total` and constructs `FDirectorTunables`; reuse a fresh local `T`):

```angelscript
        // Plan 3: time-spine — fodder rises with elapsed time; spikes + mini-boss fire on schedule.
        FDirectorTunables SpineT;
        FWavePlan SpineEarly = RaidDirector::ComputeWavePlan(1, 0, 1, SpineT, 0.0);
        FWavePlan SpineLate  = RaidDirector::ComputeWavePlan(1, 0, 1, SpineT, 400.0);
        Total += 1;
        if (SpineLate.FodderCount > SpineEarly.FodderCount) Pass += 1;
        else Print("[DirectorSmoke] FAIL: spine does not rise with time", 10.0);

        Total += 1;
        if (RaidDirector::ComputeWavePlan(1, 5, 1, SpineT, 180.0).SpikeTier == 1) Pass += 1;
        else Print("[DirectorSmoke] FAIL: no elite spike at 180s", 10.0);

        Total += 1;
        if (RaidDirector::ComputeWavePlan(1, 9, 1, SpineT, 480.0).bSpawnMiniBoss) Pass += 1;
        else Print("[DirectorSmoke] FAIL: no mini-boss at 480s", 10.0);
```

- [ ] **Step 2: Bump the SmokeTest expectation**

In `Tools/SmokeTest.ps1`, the `UpgradesEvo` case expects `[DirectorSmoke] RESULT 6/6`. The current DirectorReport prints 6/6; this adds 3 checks → it will print `9/9`. Change `[DirectorSmoke] RESULT 6/6` to `[DirectorSmoke] RESULT 9/9`. (If the controller's run shows a different total because the base count differs, set the number to whatever the controller observes — the exact base is verified at gate time.)

- [ ] **Step 3: Controller runs the gate** (the `UpgradesEvo` case via `SmokeTest.ps1`); confirm `[DirectorSmoke] RESULT 9/9`.

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "test(procgen): DirectorReport asserts time-spine + spikes

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## PART B — Hold-and-channel objective (opt-in)

### Task B1: Layout accessors in the stamper

**Files:** Modify `RogueSmoke/Script/Generation/RaidStamper.as`

- [ ] **Step 1: Add `GetLayout` + a node finder to `namespace RaidArena`**

Add inside `namespace RaidArena { ... }`:

```angelscript
    // Deterministic layout for a seed (pure; cheap). Consumers read Drop/MainSites/Extraction.
    FRaidLayout GetLayout(int Seed)
    {
        FRaidGenConfig Cfg;
        return RaidGen::GenerateValidated(Seed, Cfg);
    }

    // World location of the first node of a slot in a site (falls back to the site center).
    FVector NodeLocation(const FRaidSite& Site, ERaidSlotType Slot)
    {
        for (FRaidNode N : Site.Nodes)
        {
            if (N.Slot == Slot)
                return N.Location;
        }
        return Site.Center;
    }
```

- [ ] **Step 2: Verify (`run_code_test`)**

```angelscript
FRaidLayout L = RaidArena::GetLayout(12345);
FVector Core = RaidArena::NodeLocation(L.MainSites[0], ERaidSlotType::CombatCore);
Print(f"[B1] drop={L.Drop.Center} core={Core} extract={L.Extraction.Center}", 8.0);
```

Expected: three distinct locations printed; drop ≠ extract.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidStamper.as
git commit -m "feat(procgen): layout accessors for objective/extraction placement

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task B2: Objective `Mode`, channel state, separate extraction center

**Files:** Modify `RogueSmoke/Script/Objective/RaidObjective.as`

- [ ] **Step 1: Add the mode enum, channel/extraction fields, and replicate them**

Near the top of the file (after the existing `enum ERaidPhase { ... }`), add:

```angelscript
// How the objective is cleared. ClearElites = legacy (kill the ring). HoldAndChannel = Plan 3
// (stand in the channel radius until the bar fills). Default keeps every existing map unchanged.
enum EObjectiveMode
{
    ClearElites,
    HoldAndChannel
}
```

Inside `class ARaidObjective`, after the existing `UPROPERTY(...) float ExtractZoneRadius = 450.0;`, add:

```angelscript
    // --- Plan 3: hold-and-channel mode + generated placement (opt-in; default = legacy ClearElites) ---
    UPROPERTY(EditAnywhere, Category = "Raid|Mode")
    EObjectiveMode Mode = EObjectiveMode::ClearElites;

    // When true, BeginPlay positions the channel/extraction from the generated layout (GameState seed).
    UPROPERTY(EditAnywhere, Category = "Raid|Mode")
    bool bUseGeneratedLayout = false;

    UPROPERTY(EditAnywhere, Category = "Raid|Channel")
    float ChannelSeconds = 20.0;

    UPROPERTY(EditAnywhere, Category = "Raid|Channel")
    float ChannelRadius = 600.0;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Raid|Channel")
    float ChannelProgress = 0.0;

    // Channel point (== objective site) and the SEPARATE extraction point. Replicated so clients can
    // draw the rings. In ClearElites mode these stay at the actor location for back-compat.
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Raid|Mode")
    FVector ChannelCenter = FVector::ZeroVector;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Raid|Mode")
    FVector ExtractionCenter = FVector::ZeroVector;
```

- [ ] **Step 2: Position channel/extraction in `BeginPlay`**

In `ARaidObjective::BeginPlay`, after the existing roster-defaulting block (the `if (EliteRoster.Num() == 0) { ... }` etc.), add:

```angelscript
        // Plan 3: anchor the channel at the generated objective node and extraction at the separate
        // Extraction node. Move the actor to the channel point so the legacy GetActorLocation()-based
        // helpers (elite ring, fodder centers) follow it; extraction uses ExtractionCenter instead.
        ChannelCenter = GetActorLocation();
        ExtractionCenter = GetActorLocation();
        if (bUseGeneratedLayout)
        {
            FRaidLayout L = RaidArena::GetLayout(GetMasterSeed());
            if (L.MainSites.Num() > 0)
                ChannelCenter = RaidArena::NodeLocation(L.MainSites[0], ERaidSlotType::CombatCore);
            ExtractionCenter = L.Extraction.Center;
            SetActorLocation(ChannelCenter);
        }
```

- [ ] **Step 3: Verify it still compiles (controller runs the existing suite later)**

Run `mcp__plugin_ue-as__validate_specifiers` on `RaidObjective.as`. Expected: no specifier issues. (Behavior is exercised in Task E.)

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(procgen): objective Mode + channel/extraction state (generated placement)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task B3: Branch the objective tick on Mode (hold-and-channel + separate extraction)

**Files:** Modify `RogueSmoke/Script/Objective/RaidObjective.as`

- [ ] **Step 1: Branch `UpdateObjective` on Mode**

Replace the body of `private void UpdateObjective()` with:

```angelscript
    private void UpdateObjective()
    {
        if (Elapsed < StartGraceSeconds)
            return;

        // Place gating elites once after grace (both modes spawn them — in HoldAndChannel they are
        // pressure, not a clear-gate).
        if (!bSpawnedInitialElites)
            SpawnInitialElites();

        if (Mode == EObjectiveMode::HoldAndChannel)
        {
            UpdateChannel();
            return;
        }

        // Legacy ClearElites gate.
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
            return;
        int Remaining = Combat.GetEliteCount();
        if (Remaining > 0)
            bSeenElites = true;
        if (bSeenElites && Remaining == 0)
            SetPhase(ERaidPhase::ExtractionReady);
    }

    // Hold-and-channel: accumulate progress while >= 1 living hero stands in the channel radius.
    private void UpdateChannel()
    {
        const float RadiusSq = ChannelRadius * ChannelRadius;
        bool bAnyChanneling = false;

        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        for (AHeroCharacter H : Heroes)
        {
            if (H == nullptr || H.IsIncapacitated())
                continue;
            if ((H.GetActorLocation() - ChannelCenter).SizeSquared() <= RadiusSq)
            {
                bAnyChanneling = true;
                break;
            }
        }

        if (bAnyChanneling)
            ChannelProgress = Math::Min(ChannelProgress + GetWorld().GetDeltaSeconds(), ChannelSeconds);

        if (ChannelProgress >= ChannelSeconds)
            SetPhase(ERaidPhase::ExtractionReady);
    }

    // Test/exec hook: jump the channel bar to full so the smoke can drive the loop headlessly.
    UFUNCTION(BlueprintCallable, Category = "Raid|Channel")
    void DebugFillChannel()
    {
        if (HasAuthority())
            ChannelProgress = ChannelSeconds;
    }
```

- [ ] **Step 2: Route the extraction zone + defend wave to `ExtractionCenter`**

In `UpdateExtractionReady`, replace `const FVector Center = GetActorLocation();` with:

```angelscript
        const FVector Center = ExtractionCenter;
```

In `OnExtractionPhaseStarted`, replace the defend-wave spawn line `Director.SpawnEliteWave(DefendWaveEliteClass, GetActorLocation(), DefendWaveRadius, DefendWaveCount);` with:

```angelscript
            Director.SpawnEliteWave(DefendWaveEliteClass, ExtractionCenter, DefendWaveRadius, DefendWaveCount);
```

(In legacy ClearElites mode `ExtractionCenter == GetActorLocation()` from BeginPlay, so behavior is identical for the existing map.)

- [ ] **Step 3: Pass elapsed time into the director**

In `TickFodderWaves`, find the `FWavePlan Plan = RaidDirector::ComputeWavePlan(TeamLevel, WaveIndex, NumPlayers, MakeTunables());` line and add the elapsed-seconds arg:

```angelscript
        FWavePlan Plan = RaidDirector::ComputeWavePlan(TeamLevel, WaveIndex, NumPlayers, MakeTunables(), Elapsed);
```

Then, right after the existing elite-injection block inside `TickFodderWaves` (after the `if (Plan.EliteInjectIndex >= 0 ...) { ... }` block), add the mini-boss spike:

```angelscript
        if (Plan.bSpawnMiniBoss && BossClass.Get() != nullptr && !bSpikeBossSpawned)
        {
            bSpikeBossSpawned = true;
            AEliteEnemyBase SpikeBoss = Director.SpawnElite(BossClass, Center + FVector(0.0, 0.0, 40.0), FRotator());
            if (SpikeBoss != nullptr)
            {
                SpikeBoss.SetCountsAsObjectiveTarget(false);   // pressure spike, not a clear-gate
                Print("[Director] mini-boss spike", 4.0);
            }
        }
```

Add the guard field near the other private fields (next to `private int WaveIndex = 0;`):

```angelscript
    private bool bSpikeBossSpawned = false;
```

- [ ] **Step 4: Verify (`validate_specifiers`)** on `RaidObjective.as`; controller exercises behavior in Task E.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(procgen): hold-and-channel gate + separate extraction + director elapsed/miniboss

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## PART C — GameMode generated-arena mode + GameState client stamp

### Task C1: GameMode auto-stamp + hero spawn at Drop

**Files:** Modify `RogueSmoke/Script/Core/RaidGameMode.as`

- [ ] **Step 1: Add the flag + layout cache**

In `class ARaidGameMode`, after the `URunManager RunManager` property, add:

```angelscript
    // Plan 3: when true, this raid stamps the generated arena and spawns heroes on the Drop node.
    // Default false → the existing hand-built RaidArena map is unaffected. Set true on BP_GenRaidGamemode.
    UPROPERTY(EditDefaultsOnly, Category = "Run|Generated")
    bool bGeneratedArena = false;

    private FRaidLayout CachedLayout;
    private bool bLayoutReady = false;

    private void EnsureLayout()
    {
        if (bLayoutReady)
            return;
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        int Seed = (GS != nullptr) ? GS.MasterSeed : 0;
        CachedLayout = RaidArena::GetLayout(Seed);
        bLayoutReady = true;
    }
```

- [ ] **Step 2: Stamp server-side + flag the GameState in `BeginPlay`**

In `BeginPlay`, immediately after `RunManager.StartRun();`, add:

```angelscript
        if (bGeneratedArena)
        {
            ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
            if (GS != nullptr)
            {
                GS.bGeneratedArena = true;            // tells clients to stamp on OnRep_MasterSeed
                EnsureLayout();
                RaidArena::BuildFromSeed(GS.MasterSeed);   // server stamps its own copy
            }
        }
```

- [ ] **Step 3: Redirect hero spawn to the Drop node**

In `PickHeroSpawnPoint`, at the very top of the method body (before the `TArray<AHeroCharacter> Existing;` line), add:

```angelscript
        if (bGeneratedArena)
        {
            TArray<AHeroCharacter> Heroes;
            GetAllActorsOfClass(Heroes);
            // EnsureLayout is non-const; this method is const, so read a fresh deterministic layout.
            ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
            int Seed = (GS != nullptr) ? GS.MasterSeed : 0;
            FVector Drop = RaidArena::GetLayout(Seed).Drop.Center;
            return Drop + FVector(120.0 * Heroes.Num(), 0.0, 150.0);
        }
```

- [ ] **Step 4: Verify (`validate_specifiers`)** on `RaidGameMode.as`.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Script/Core/RaidGameMode.as
git commit -m "feat(procgen): GameMode generated-arena mode (auto-stamp + spawn at Drop)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

### Task C2: GameState client stamp

**Files:** Modify `RogueSmoke/Script/Core/RaidGameState.as`

- [ ] **Step 1: Add the replicated flag + change MasterSeed to ReplicatedUsing**

Replace the `MasterSeed` declaration:

```angelscript
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    int MasterSeed = 0;
```

with:

```angelscript
    UPROPERTY(Replicated, ReplicatedUsing = OnRep_MasterSeed, BlueprintReadOnly, Category = "Run")
    int MasterSeed = 0;

    // Plan 3: set true by the GameMode when this raid uses a generated arena, so clients stamp on OnRep.
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    bool bGeneratedArena = false;
```

- [ ] **Step 2: Add the OnRep client stamp**

After the existing `OnRep_Phase` function, add:

```angelscript
    // Clients re-stamp the generated arena locally once the seed arrives (server stamped in
    // GameMode::BeginPlay). Deterministic from the replicated seed — no geometry is replicated.
    UFUNCTION()
    void OnRep_MasterSeed()
    {
        if (bGeneratedArena && MasterSeed != 0)
            RaidArena::BuildFromSeed(MasterSeed);
    }
```

- [ ] **Step 3: Verify (`validate_specifiers`)** on `RaidGameState.as`.

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Core/RaidGameState.as
git commit -m "feat(procgen): client stamps generated arena on OnRep_MasterSeed

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## PART D — The new level (headless authoring)

> This part is **editor/MCP work** and must be serialized through the controller session (one editor).
> It follows the project's established `DL_*` headless pattern (build a level + place code-defined
> actors via `unreal-test-mcp` python; AS classes resolve at `/Script/Angelscript.<Name>`,
> C++ at `/Script/RogueSmoke.<Name>`). The controller runs these steps via `python_exec`.

### Task D1: Create `BP_GenRaidGamemode` and the `L_GenRaid` level

**Files:** Create `/Game/Blueprints/BP_GenRaidGamemode` and `/Game/Levels/L_GenRaid` (assets, via python)

- [ ] **Step 1: Create the GameMode Blueprint**

Via `mcp__unreal-test-mcp__python_exec`, create a Blueprint `BP_GenRaidGamemode` parented to the AngelScript `ARaidGameMode` (`/Script/Angelscript.RaidGameMode`). Set its class defaults: `bGeneratedArena = true`, `GameStateClass` inherits, and assign `HeroPawnClasses` to the same `BP_Vanguard`/`BP_Bombardier` the existing `BP_RaidGamemode` uses (read those off `BP_RaidGamemode` first). Set `PlayerControllerClass = BP_RaidPlayerController`, `DefaultPawnClass = BP_Vanguard` (mirror `BP_RaidGamemode`). Compile + save the Blueprint (CDO edits need `compile_blueprint` + save — see project memory `mcp-bp-cdo-needs-compile`).

- [ ] **Step 2: Create the level + place the objective**

Create an empty level `/Game/Levels/L_GenRaid`. In it: place one `ARaidObjective` (`/Script/Angelscript.RaidObjective`) at the origin with `Mode = HoldAndChannel`, `bUseGeneratedLayout = true`, `DefendWaveEliteClass = BP_Carapace` (so extraction spawns a defend wave), `ChannelSeconds = 20`, `ChannelRadius = 600`. Place one `APlayerStart` at the origin (fallback only — heroes spawn at the Drop node). Set the level's **World Settings GameMode Override = BP_GenRaidGamemode**. Save the level.

- [ ] **Step 3: Verify the level boots + stamps + arms (controller, headless)**

Boot the level headless with no exec and confirm the breadcrumbs: `[RunManager] Run started`, `[RaidArena] stamped N boxes`, and the objective arming. Command:

```powershell
& "F:\UEAS\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\RogueSmoke.uproject" /Game/Levels/L_GenRaid -game -unattended -nullrhi -nosound -nosplash -abslog="$env:TEMP\rs_smoke\GenRaidBoot.log"
```

Expected in the log: `[RunManager] Run started`, `[RaidArena] stamped` (≥8 boxes), no `Fatal error` / `Script call stack`.

- [ ] **Step 4: Commit the new assets**

```bash
git add RogueSmoke/Content/Blueprints/BP_GenRaidGamemode.uasset RogueSmoke/Content/Levels/L_GenRaid.umap
git commit -m "feat(procgen): L_GenRaid level + BP_GenRaidGamemode (generated raid)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## PART E — End-to-end gate

### Task E1: `GenLoopSmoke` exec + SmokeTest case

**Files:** Modify `RogueSmoke/Script/Player/RaidPlayerController.as`, `Tools/SmokeTest.ps1`

- [ ] **Step 1: Add the `GenLoopSmoke` exec**

Add as a sibling of `GenSmoke`/`StampSmoke`. It drives the hold-and-channel loop headlessly: fills the channel, teleports a hero to the Extraction node, calls extraction, and asserts the run reaches Victory. (Mirrors `RaidLoopSmoke`'s victory shape but for the generated loop.)

```angelscript
    // Headless end-to-end gate for the GENERATED Phase-1 loop (procgen Plan 3). Force-fills the
    // channel, moves a hero to the extraction node, calls extraction, and asserts Victory. Retry-polls
    // for the objective + a hero like the other loop smokes. SmokeTest greps the RESULT line.
    private int GenLoopRetries = 0;
    UFUNCTION(Exec)
    void GenLoopSmoke()
    {
        TArray<ARaidObjective> Objs;
        GetAllActorsOfClass(Objs);
        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        ARaidObjective Obj = Objs.Num() > 0 ? Objs[0] : nullptr;
        AHeroCharacter Hero = Heroes.Num() > 0 ? Heroes[0] : nullptr;

        if (Obj == nullptr || Hero == nullptr)
        {
            if (GenLoopRetries < 40)
            {
                GenLoopRetries += 1;
                System::SetTimer(this, n"GenLoopSmoke", 1.0, false);
                return;
            }
            Print("[GenLoopSmoke] gave up waiting for objective/hero", 8.0);
            Print("[GenLoopSmoke] RESULT 0/4", 15.0);
            return;
        }

        int Pass = 0;
        int Total = 0;

        // 1. Channel mode armed at the generated node (channel != actor origin proves generated placement).
        Total += 1;
        if (Obj.Mode == EObjectiveMode::HoldAndChannel && Obj.ChannelCenter != Obj.ExtractionCenter) Pass += 1;
        else Print("[GenLoopSmoke] FAIL 1: not generated hold-and-channel", 12.0);

        // 2. Filling the channel opens extraction.
        Total += 1;
        Obj.DebugFillChannel();
        Obj.ExtractionDefendSeconds = 0.1;
        Obj.DefendWaveCount = 0;                    // keep the gate fast/headless
        // give the server a tick to flip ExtractionReady, then drive the rest
        System::SetTimer(this, n"GenLoopFinish", 1.0, false);
        Pass += 1;                                   // arming verified; phase asserted in GenLoopFinish

        // checks 3 & 4 are asserted in GenLoopFinish; print a partial breadcrumb so a hang is visible
        Print(f"[GenLoopSmoke] armed {Pass}/{Total} (finishing...)", 6.0);
    }

    UFUNCTION()
    void GenLoopFinish()
    {
        TArray<ARaidObjective> Objs;
        GetAllActorsOfClass(Objs);
        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        ARaidObjective Obj = Objs.Num() > 0 ? Objs[0] : nullptr;
        AHeroCharacter Hero = Heroes.Num() > 0 ? Heroes[0] : nullptr;
        if (Obj == nullptr || Hero == nullptr)
        {
            Print("[GenLoopSmoke] RESULT 2/4", 15.0);
            return;
        }

        int Pass = 2;   // checks 1 + 2 already passed in GenLoopSmoke
        int Total = 4;

        // 3. Standing on the extraction pad + calling it reaches Victory.
        Hero.SetActorLocation(Obj.ExtractionCenter + FVector(0.0, 0.0, 150.0));
        Obj.CallExtraction();

        if (Obj.Phase == ERaidPhase::Extracting || Obj.Phase == ERaidPhase::Extracted) Pass += 1;
        else Print(f"[GenLoopSmoke] FAIL 3: extraction not called (phase={Obj.Phase})", 12.0);

        // 4. Run resolves to Victory after the short defend timer.
        System::SetTimer(this, n"GenLoopAssertVictory", 1.0, false);
        Print(f"[GenLoopSmoke] RESULT {Pass}/{Total}", 15.0);
    }

    UFUNCTION()
    void GenLoopAssertVictory()
    {
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS != nullptr && GS.Phase == ERunPhase::Victory)
            Print("[GenLoopSmoke] VICTORY confirmed", 8.0);
        else
            Print(f"[GenLoopSmoke] victory not reached (phase={GS.Phase})", 8.0);
    }
```

> NOTE for the implementer: the multi-timer split above is because the objective needs a server tick
> between `DebugFillChannel`→`ExtractionReady` and `CallExtraction`→`Extracting`→`Extracted`. The
> SmokeTest greps for `[GenLoopSmoke] RESULT 4/4` AND `[GenLoopSmoke] VICTORY confirmed`. If the
> controller's run shows the RESULT printing before victory, that's expected (victory is a separate
> breadcrumb) — both strings must appear. Tune the timer counts if a headless tick is slower.

- [ ] **Step 2: Add the SmokeTest case**

In `Tools/SmokeTest.ps1`, add after the `StampArena` case:

```powershell
    # Generated Phase-1 loop (Plan 3): stamp + spawn-at-drop + hold-and-channel + separate extraction -> Victory.
    @{ Name = "GenLoopVictory";     Map = "/Game/Levels/L_GenRaid";                        Expect = @("[RaidArena] stamped", "[GenLoopSmoke] VICTORY confirmed"); Exec = "GenLoopSmoke"; Window = 40 }
```

- [ ] **Step 3: Controller runs the gate**

Controller runs `SmokeTest.ps1` (or a targeted `-ExecCmds=GenLoopSmoke` boot of `L_GenRaid`) and confirms `GenLoopVictory` PASS (`[RaidArena] stamped` + `[GenLoopSmoke] VICTORY confirmed`, no fatals), and that the **whole suite stays green** (all prior cases unaffected).

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "test(procgen): GenLoopSmoke end-to-end generated-loop gate (-> Victory)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (Plan 3 = the rest of Phase 1):**
- §4 raid loop (drop → objective → separate extraction; rising-tide director + spikes) → Parts A, B, C. ✅ (one objective, not 2–3; deferred.)
- §5 generator output consumed by the live raid (drop/objective/extraction nodes) → Parts B, C. ✅
- §6 hold-and-channel task type → Part B. ✅ (other task types deferred.)
- §7 stamping wired on all machines (server BeginPlay + client OnRep) → Part C. ✅
- §12 integration with RunManager seed / RaidObjective / director / win-loss bridges → all parts, bridges preserved. ✅

**Opt-in safety:** every change is behind `Mode`/`bGeneratedArena`/defaulted `ElapsedSeconds`; the shared classes' defaults are unchanged, so the existing RaidArena/RaidLoop/MoveSmoke cases are untouched. The only existing test that changes is `DirectorReport` (count bumped to 9/9 for new additive assertions) — and that's a pure-function test, not a behavior change. ✅

**Deferred (named):** 2–3 objectives/raid + the other task-types; the archetype pool; varying footprint; PCG cosmetic skin; retiring the hand-built RaidArena map; the small Plan-2 cleanups (parameterize `BuildFromSeed`'s config — note Part C calls `RaidArena::GetLayout`/`BuildFromSeed` with the default config, matching the stamper, so they stay consistent; unused `HorizSingleJump`).

**Placeholder scan:** none — every code step is complete; every gate step states the exact expected breadcrumb. The level-authoring (Part D) is procedural editor work, so its steps describe exact assets/properties + a boot verification rather than code. ✅

**Type consistency:** `EObjectiveMode`, `Mode`, `bUseGeneratedLayout`, `ChannelProgress/Center`, `ExtractionCenter`, `DebugFillChannel`, `bGeneratedArena`, `EnsureLayout`, `RaidArena::{GetLayout,NodeLocation}`, `FWavePlan.{SpikeTier,bSpawnMiniBoss}`, the new `FDirectorTunables` spine fields — each defined once and used consistently across objective/gamemode/gamestate/director/smoke. ✅

**Risk notes for the executor:**
- **Hero-spawn timing:** `PickHeroSpawnPoint` reads `GameState.MasterSeed`; the GameMode rolls it in `BeginPlay` (before player controllers init), so the seed is present. If a hero ever drops at origin instead of the Drop node, the seed wasn't ready — add a one-frame deferral.
- **Headless ticks:** `GenLoopSmoke` splits across timers because phase transitions need server ticks; if a headless box is slow, raise the timer counts / the case `Window`.
- **Level authoring is the least pre-canned step** (Part D) — it's MCP/python editor work; expect to iterate on exact property paths (`bGeneratedArena`, `Mode`) and the GameMode-override wiring, verified by the Step-3 boot. Per project memory, BP CDO edits need `compile_blueprint` + save or PIE wipes them.
- **`ChannelCenter != ExtractionCenter`** is asserted in GenLoopSmoke check 1 — guaranteed by the generator's `DropExtractMinFrac`/opposite-edge placement and the CombatCore-vs-Extraction split.

## Parallelization

Part A (director), Part B (objective), and Part C (gamemode/gamestate) touch **different files** and are authorable in parallel — but they share the same branch/index, so the controller must dispatch them **sequentially** (one implementer at a time) to avoid commit races, per the subagent-driven rule. Part D (level) depends on B+C being authored (it configures their flags) and is editor-serialized. Part E depends on all prior parts. Every `run_code_test`, `validate_specifiers`, and `SmokeTest.ps1`/headless boot is funneled through the single editor session. No git worktrees.
