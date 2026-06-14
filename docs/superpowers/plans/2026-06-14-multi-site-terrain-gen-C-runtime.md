# Multi-Site Terrain Gen — Plan C: Multi-Site Runtime (Objective Manager + Director)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Generalize `ARaidObjective` from one hold-and-channel point into a **multi-site manager** that runs all 2–3 generated zones — each channels independently, the director's pressure concentrates on the active site, extraction is gated until every site completes, and the mini-boss fires at the last-completed site — completing a multi-site raid to Victory.

**Architecture:** Additive within the existing `ARaidObjective` (`Script/Objective/`). The single replicated `ChannelCenter`/`ChannelProgress` become the **active-site mirror** (so the existing HUD keeps working), backed by new replicated parallel arrays `ChannelCenters[]` / `ChannelProgresses[]` (one per zone). `UpdateChannel` loops the zones, accrues per-site progress, tracks the active site, and on all-complete spawns the mini-boss at the last-completed site before `ExtractionReady`. The run-result bridges (`SetPhase` → extraction → defend → `EndRunResult`) are untouched. **No new C++, no new file.** This is **Plan C of 3** (A terrain + B partition are done).

**Tech Stack:** UnrealEngine-Angelscript (Hazelight UE5.7 fork); `UPROPERTY(Replicated)` arrays auto-register in this fork; verification via `as-helper run_code_test` and `Tools/SmokeTest.ps1` (`[GenLoopSmoke] VICTORY confirmed`).

---

## Design notes the engineer must know

- **This edits a working victory loop — do not break the bridges.** The flow `SetPhase(ExtractionReady) → UpdateExtractionReady → CallExtraction → Extracting → UpdateExtraction → Extracted → OnPhaseChanged → EndRunResult(true)` must stay intact. Only the *gate into* `ExtractionReady` changes (all sites complete instead of one).
- **Back-compat:** `ARaidObjective.Mode` defaults to `ClearElites`; only the generated `L_GenRaid` objective sets `bUseGeneratedLayout=true` → `HoldAndChannel`. The legacy `RaidArena` map is unaffected. Keep the single-site `ClearElites` path working.
- **HUD reads `ChannelCenter` + `ChannelProgress` + `ExtractionCenter`** (replicated). Keep updating those to reflect the **active site** so the HUD needs no change this slice. The new arrays are additive.
- **Active site** = the zone a living hero currently stands in (channel radius). If several are occupied (split squad), the lowest-index occupied site is "active" for the director center. If none are occupied, the active index holds its last value.
- **Mini-boss at the last-completed site** (decision 10): spawn it in the objective when the final site completes, and set the existing `bSpikeBossSpawned = true` so the director's *timed* mini-boss spike never double-spawns.
- **Existing members you build on** (in `RaidObjective.as`): `ChannelCenter` (Replicated FVector), `ChannelProgress` (Replicated float), `ExtractionCenter` (Replicated FVector), `ChannelSeconds`, `ChannelRadius`, `Mode`, `bUseGeneratedLayout`, `SetPhase(ERaidPhase)`, `UpdateChannel()`, `DebugFillChannel()`, `bSpikeBossSpawned`, `GetMasterSeed()`, `BossClass`, `SpawnInitialElites`. `RaidArena::GetLayout(int) -> FRaidLayout`, `RaidArena::NodeLocation(const FRaidSite&, ERaidSlotType) -> FVector`. `USpawnDirector::Get()` + `SpawnElite(TSubclassOf<AEliteEnemyBase>, FVector, FRotator)`.
- **No `import` statements** (symbols globally visible).
- **Verification:** `run_code_test` (compile); `SmokeTest.ps1` `GenLoopVictory` boots `L_GenRaid` with `-ExecCmds=GenLoopSmoke` and greps `[GenLoopSmoke] VICTORY confirmed`.
- **Branch:** `multi-site-terrain-gen`. Commit after every task. Pure AngelScript — never build C++ or launch the interactive editor; one `run_code_test` at a time.

## File structure

- **Modify** `RogueSmoke/Script/Objective/RaidObjective.as` — multi-site channel state, BeginPlay population, multi-site `UpdateChannel`, active-site director center, `DebugFillChannel` fills all, last-site mini-boss.
- **Modify** `RogueSmoke/Script/Player/RaidPlayerController.as` — `GenLoopSmoke` asserts a multi-site victory (and/or `GenSmoke` site-channel count); confirm the exec drives all sites.
- (No `SmokeTest.ps1` change expected — the `GenLoopVictory` breadcrumb is unchanged.)

---

### Task 1: Multi-site channel state

**Files:**
- Modify: `RogueSmoke/Script/Objective/RaidObjective.as`

- [ ] **Step 1: Add replicated per-site arrays + active index** — immediately AFTER the existing `ExtractionCenter` UPROPERTY block (around line 74), add:

```angelscript
    // --- Plan C: multi-site channels. ChannelCenter/ChannelProgress above mirror the ACTIVE site
    // (HUD back-compat); these hold every zone. Parallel arrays (same length = zone count). ---
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Raid|Channel")
    TArray<FVector> ChannelCenters;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Raid|Channel")
    TArray<float> ChannelProgresses;

    // Index into ChannelCenters of the site currently being channeled (for director focus). -1 = none yet.
    private int ActiveSiteIndex = -1;
```

- [ ] **Step 2: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(procgen): multi-site channel state (per-zone arrays + active index)"
```

---

### Task 2: Populate all zones in BeginPlay

**Files:**
- Modify: `RogueSmoke/Script/Objective/RaidObjective.as` (`BeginPlay`)

- [ ] **Step 1: Replace the generated-layout block** — find this block in `BeginPlay`:

```angelscript
        if (bUseGeneratedLayout)
        {
            // A generated objective is always hold-and-channel (MVP). Set it here so the level asset
            // needn't carry the enum value (editor-python can't set AS EnumProperties).
            Mode = EObjectiveMode::HoldAndChannel;
            FRaidLayout L = RaidArena::GetLayout(GetMasterSeed());
            if (L.MainSites.Num() > 0)
                ChannelCenter = RaidArena::NodeLocation(L.MainSites[0], ERaidSlotType::CombatCore);
            ExtractionCenter = L.Extraction.Center;
            SetActorLocation(ChannelCenter);
        }
```

Replace with a version that fills one channel per zone:

```angelscript
        if (bUseGeneratedLayout)
        {
            // A generated objective is always hold-and-channel (MVP). Set it here so the level asset
            // needn't carry the enum value (editor-python can't set AS EnumProperties).
            Mode = EObjectiveMode::HoldAndChannel;
            FRaidLayout L = RaidArena::GetLayout(GetMasterSeed());
            for (int i = 0; i < L.MainSites.Num(); i++)
            {
                ChannelCenters.Add(RaidArena::NodeLocation(L.MainSites[i], ERaidSlotType::CombatCore));
                ChannelProgresses.Add(0.0);
            }
            ExtractionCenter = L.Extraction.Center;
            if (ChannelCenters.Num() > 0)
            {
                ChannelCenter = ChannelCenters[0];   // active-site mirror starts on zone 0
                SetActorLocation(ChannelCenter);
            }
        }
```

- [ ] **Step 2: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(procgen): BeginPlay builds one channel per generated zone"
```

---

### Task 3: Multi-site `UpdateChannel` + last-site mini-boss

**Files:**
- Modify: `RogueSmoke/Script/Objective/RaidObjective.as` (`UpdateChannel`)

- [ ] **Step 1: Replace `UpdateChannel`** — replace the ENTIRE existing `UpdateChannel` function:

```angelscript
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
```

with the multi-site version (single-site layouts: `ChannelCenters` stays empty → falls back to the legacy single-point behavior using `ChannelCenter`):

```angelscript
    // Hold-and-channel: every zone channels independently while >= 1 living hero stands in its radius.
    // Extraction opens only when ALL zones are full; the last zone to fill spawns the mini-boss.
    private void UpdateChannel()
    {
        const float RadiusSq = ChannelRadius * ChannelRadius;
        const float Dt = GetWorld().GetDeltaSeconds();

        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);

        // Single-point fallback (no generated zones): legacy behavior on ChannelCenter/ChannelProgress.
        if (ChannelCenters.Num() == 0)
        {
            for (AHeroCharacter H : Heroes)
            {
                if (H == nullptr || H.IsIncapacitated())
                    continue;
                if ((H.GetActorLocation() - ChannelCenter).SizeSquared() <= RadiusSq)
                {
                    ChannelProgress = Math::Min(ChannelProgress + Dt, ChannelSeconds);
                    break;
                }
            }
            if (ChannelProgress >= ChannelSeconds)
                SetPhase(ERaidPhase::ExtractionReady);
            return;
        }

        int Active = -1;
        int CompleteCount = 0;
        for (int s = 0; s < ChannelCenters.Num(); s++)
        {
            bool bOccupied = false;
            for (AHeroCharacter H : Heroes)
            {
                if (H == nullptr || H.IsIncapacitated())
                    continue;
                if ((H.GetActorLocation() - ChannelCenters[s]).SizeSquared() <= RadiusSq)
                { bOccupied = true; break; }
            }
            if (bOccupied)
            {
                ChannelProgresses[s] = Math::Min(ChannelProgresses[s] + Dt, ChannelSeconds);
                if (Active < 0)
                    Active = s;   // lowest-index occupied site is the director's focus
            }
            if (ChannelProgresses[s] >= ChannelSeconds)
                CompleteCount += 1;
        }

        // Mirror the active (or last-active) site into ChannelCenter/ChannelProgress for the HUD.
        if (Active >= 0)
            ActiveSiteIndex = Active;
        if (ActiveSiteIndex >= 0)
        {
            ChannelCenter = ChannelCenters[ActiveSiteIndex];
            ChannelProgress = ChannelProgresses[ActiveSiteIndex];
        }

        // All zones full -> climax mini-boss at the last-completed site, then open extraction.
        if (CompleteCount >= ChannelCenters.Num())
        {
            SpawnClimaxMiniBoss();
            SetPhase(ERaidPhase::ExtractionReady);
        }
    }

    // Spawn the mini-boss at the active (last-completed) site and suppress the director's timed spike.
    private void SpawnClimaxMiniBoss()
    {
        if (bSpikeBossSpawned)
            return;
        bSpikeBossSpawned = true;     // also stops RaidDirector's timed mini-boss from double-spawning
        if (BossClass.Get() == nullptr)
            return;
        int Idx = ActiveSiteIndex >= 0 ? ActiveSiteIndex : 0;
        FVector Where = (Idx < ChannelCenters.Num()) ? ChannelCenters[Idx] : GetActorLocation();
        USpawnDirector Director = USpawnDirector::Get();
        if (Director != nullptr)
        {
            AEliteEnemyBase Boss = Director.SpawnElite(BossClass, Where + FVector(0.0, 0.0, 40.0), FRotator());
            if (Boss != nullptr)
            {
                Boss.SetCountsAsObjectiveTarget(false);   // a spike, not a re-gate (extraction already opens)
                Print("[Raid] climax mini-boss at last objective", 4.0);
            }
        }
    }
```

- [ ] **Step 2: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(procgen): multi-site channel gate + last-site climax mini-boss"
```

---

### Task 4: Director pressure focuses on the active site

**Files:**
- Modify: `RogueSmoke/Script/Objective/RaidObjective.as` (`PickWaveCenter`)

- [ ] **Step 1: Bias the wave center to the active channel site** — in `PickWaveCenter`, find:

```angelscript
    private FVector PickWaveCenter(int Index)
    {
        FVector Base = GetActorLocation();

        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        if (Heroes.Num() > 0)
        {
            AHeroCharacter Target = Heroes[Index % Heroes.Num()];
            if (Target != nullptr)
                Base = Target.GetActorLocation();
        }
```

Replace it with a version that anchors on the active zone's hero in multi-site mode (so the swarm concentrates on the site being channeled):

```angelscript
    private FVector PickWaveCenter(int Index)
    {
        FVector Base = GetActorLocation();

        // Multi-site: the swarm's pressure concentrates on the active channel site (decision 9).
        if (Mode == EObjectiveMode::HoldAndChannel && ActiveSiteIndex >= 0
            && ActiveSiteIndex < ChannelCenters.Num())
            Base = ChannelCenters[ActiveSiteIndex];

        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        if (Heroes.Num() > 0)
        {
            AHeroCharacter Target = Heroes[Index % Heroes.Num()];
            if (Target != nullptr)
                Base = Target.GetActorLocation();
        }
```

(Heroes near the active site will be picked anyway; the active-site anchor matters when the squad is between zones or in the single-player headless case.)

- [ ] **Step 2: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(procgen): director wave pressure focuses on the active site"
```

---

### Task 5: `DebugFillChannel` fills every zone

**Files:**
- Modify: `RogueSmoke/Script/Objective/RaidObjective.as` (`DebugFillChannel`)

The smoke test calls `DebugFillChannel` to drive the loop headlessly. It must complete ALL zones now.

- [ ] **Step 1: Replace `DebugFillChannel`** — find:

```angelscript
    UFUNCTION(BlueprintCallable, Category = "Raid|Channel")
    void DebugFillChannel()
    {
        if (HasAuthority())
            ChannelProgress = ChannelSeconds;
    }
```

Replace with:

```angelscript
    UFUNCTION(BlueprintCallable, Category = "Raid|Channel")
    void DebugFillChannel()
    {
        if (!HasAuthority())
            return;
        ChannelProgress = ChannelSeconds;
        for (int s = 0; s < ChannelProgresses.Num(); s++)
            ChannelProgresses[s] = ChannelSeconds;
        if (ActiveSiteIndex < 0 && ChannelCenters.Num() > 0)
            ActiveSiteIndex = ChannelCenters.Num() - 1;   // attribute the climax to the last zone
    }
```

- [ ] **Step 2: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Objective/RaidObjective.as
git commit -m "feat(procgen): DebugFillChannel completes every zone"
```

---

### Task 6: `GenLoopSmoke` multi-site victory assertion

**Files:**
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as` (`GenLoopSmoke`)

- [ ] **Step 1: Read `GenLoopSmoke`** — grep `RaidPlayerController.as` for `void GenLoopSmoke` and read it. It currently finds the `ARaidObjective`, calls `DebugFillChannel`, advances phases, and prints `[GenLoopSmoke] VICTORY confirmed` plus a `RESULT n/n`. Identify how it steps the loop and its counters.

- [ ] **Step 2: Add a multi-site assertion** — before the final RESULT print, add a check that the objective has >= 2 channel sites (proving the generated multi-site objective is what drove the victory). Use the loop's existing counter names (read them in Step 1; e.g. `Pass`/`Total`):

```angelscript
        // Multi-site: the generated objective must run 2+ zones.
        Total += 1;
        ARaidObjective MObj = Cast<ARaidObjective>(GetObjectiveActor());   // use the loop's existing lookup
        if (MObj != nullptr && MObj.ChannelCenters.Num() >= 2)
            Pass += 1;
        else
            Print("[GenLoopSmoke] FAIL multisite (<2 zones)", 6.0);
```

> Step 1 will show how `GenLoopSmoke` already obtains the objective actor (it must, to call `DebugFillChannel`). Reuse that exact lookup expression instead of `GetObjectiveActor()` if the name differs. If the loop asserts an exact RESULT denominator that `SmokeTest.ps1` greps, bump that denominator by 1 in both the Print and the `SmokeTest.ps1` `GenLoopVictory` `Expect` (only if it greps a count — the current `Expect` is the literal `[GenLoopSmoke] VICTORY confirmed`, which does NOT include a count, so no SmokeTest change is needed).

- [ ] **Step 3: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as
git commit -m "test(procgen): GenLoopSmoke asserts multi-site objective drives victory"
```

---

### Task 7: Headless gate + visual proof (run from the controller session)

**Files:** none (verification only)

- [ ] **Step 1: Multi-site victory** — boot `/Game/Levels/L_GenRaid` with `-ExecCmds="GenLoopSmoke"` (~42s), grep `[GenLoopSmoke] VICTORY confirmed` and the new RESULT, no fatals. The DebugFillChannel must complete all zones → climax mini-boss → ExtractionReady → defend → Extracted → Victory.

- [ ] **Step 2: Regression** — boot `RaidArena` with `GenSmoke` (expect `12/12`) and `StampSmoke` (`2/2`, `valid=true`); boot `RaidArena` `RaidLoopSmoke victory` (expect `[RaidLoopSmoke] RESULT 4/4`) to confirm the **legacy single-site ClearElites loop still wins** (the `UpdateChannel` fallback path).

- [ ] **Step 3: Full suite** — run `Tools/SmokeTest.ps1`; expect all 14 PASS.

- [ ] **Step 4: Visual proof** — boot `L_GenRaid` `-RenderOffScreen -ExecCmds="GenShots"`; confirm the multi-zone layout (2-3 Maw markers) renders on terrain.

---

## Self-review against the spec

- **Spec §4 decision 3 (2-3 sites, free order, extraction gated on all):** Tasks 2-3 (per-zone channels, all-complete gate). ✓
- **Spec §4 decision 9 (active-site focus director):** Task 4 (`PickWaveCenter` active-site anchor) + Task 3 (`ActiveSiteIndex` tracking). ✓
- **Spec §4 decision 10 (mini-boss at last-completed site):** Task 3 (`SpawnClimaxMiniBoss` at `ActiveSiteIndex`, suppresses the timed spike). ✓
- **Spec §6 objective manager (gate on all, keep bridges):** Tasks 1-3; `SetPhase`→extraction→defend→`EndRunResult` untouched. ✓
- **Spec §6 HUD back-compat:** `ChannelCenter`/`ChannelProgress` mirror the active site (Task 3). ✓
- **Spec §8 testing (multi-site GenLoop, legacy loop stays green):** Tasks 6-7 (incl. `RaidLoopSmoke` fallback check). ✓
- **Out of scope (later):** per-player UI for multiple bars, true Voronoi lanes, objective task-type variety, PCG/art. Not here. ✓
- **Type consistency:** `ChannelCenters`/`ChannelProgresses` (parallel arrays) + `ActiveSiteIndex` defined Task 1, used Tasks 2/3/4/5/6; `SpawnClimaxMiniBoss()` defined+called Task 3; fallback path keyed on `ChannelCenters.Num()==0` consistently. ✓
- **Risk:** replicated `TArray<FVector>`/`TArray<float>` must register in the fork — Task 1's compile gate confirms; if replication of arrays is unsupported, fall back to non-replicated arrays + keep the single replicated `ChannelCenter`/`ChannelProgress` mirror for the HUD (the gameplay gate is server-side and needs no array replication). Note this in the Task 1 report if it arises.
