// RaidObjective.as
// The single-raid flow (D-0009) with called-in defend-timer extraction (D-0010).
// Server-authoritative; phase + timer replicate so clients can drive objective UI.
// Place ONE per arena. Gameplay flow lives in script (D-0002 / GDD §11.3).
//
// Flow (GDD §3.1): clear the arena's elites -> call extraction -> survive a defend
// timer (final wave) -> extracted = run won. Party wipe = run lost.
//
// MVP scope notes:
//  - Raid goal = "clear every elite in the handcrafted arena" (MVP §12), detected by
//    polling UCombatSubsystem::GetEliteCount() rather than per-elite death wiring.
//  - The defend wave spawns via OnExtractionPhaseStarted -> USpawnDirector::SpawnEliteWave.
//    Caveat: it only spawns if DefendWaveEliteClass is set; it is NOT defaulted in BeginPlay
//    (unlike EliteRoster/BossClass/InjectRoster), so out of the box the defend phase is just
//    continued fodder pressure. See the core-loop-hardening plan, Task 3.
//  - Party-wipe loss IS wired: URogueDownComponent runs down/revive and calls NotifyPartyWiped()
//    when every hero is incapacitated, which ends the run in defeat (D-0010).

enum ERaidPhase
{
    InProgress,        // clearing the arena
    ExtractionReady,   // objective done; players may call extraction
    Extracting,        // defend timer running
    Extracted,         // survived the hold -> WIN
    Failed             // party wiped -> LOSS
}

// How the objective is cleared. ClearElites = legacy (kill the ring). HoldAndChannel = Plan 3
// (stand in the channel radius until the bar fills). Default keeps every existing map unchanged.
enum EObjectiveMode
{
    ClearElites,
    HoldAndChannel
}

class ARaidObjective : AActor
{
    default bReplicates = true;
    // Overriding the Tick event below is what enables ticking in the AngelScript fork
    // (mirrors Blueprint) — there is no settable `default PrimaryActorTick.bCanEverTick`.

    UPROPERTY(EditAnywhere, Category = "Raid")
    float ExtractionDefendSeconds = 30.0;

    // Stand-on-the-pad extraction: once the arena is clear, a living hero standing within this radius
    // of the objective calls extraction in. Gives solo/host play a trigger with no extra input binding
    // (remote clients also keep AHeroCharacter::Server_CallExtraction). GDD §3.1 "call extraction".
    UPROPERTY(EditAnywhere, Category = "Raid")
    float ExtractZoneRadius = 450.0;

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

    // --- Plan C: multi-site channels. ChannelCenter/ChannelProgress above mirror the ACTIVE site
    // (HUD back-compat); these hold every zone. Parallel arrays (same length = zone count). ---
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Raid|Channel")
    TArray<FVector> ChannelCenters;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Raid|Channel")
    TArray<float> ChannelProgresses;

    // Index into ChannelCenters of the site currently being channeled (for director focus). -1 = none yet.
    private int ActiveSiteIndex = -1;

    // The final defend wave spawned when extraction is called (D-0010). Leave the class
    // unset to skip spawning (e.g. while testing the timer alone).
    UPROPERTY(EditAnywhere, Category = "Raid|Defend Wave")
    TSubclassOf<AEliteEnemyBase> DefendWaveEliteClass;

    UPROPERTY(EditAnywhere, Category = "Raid|Defend Wave")
    int DefendWaveCount = 8;

    UPROPERTY(EditAnywhere, Category = "Raid|Defend Wave")
    float DefendWaveRadius = 1200.0;

    // --- Elite roster: the elites placed at raid start. These COUNT toward "clear the arena" (so the raid
    // isn't done until they're dead); the fodder waves below are pressure that does NOT gate the clear. A
    // seeded mix is drawn from EliteRoster, plus one optional boss at the center. Defaults to the bio-horde
    // roster (Carapace/Spitter/Bloater/Lunger + Brood-mother) so a fresh arena works without editor wiring;
    // designers override by filling these on the placed objective. Balance is a tuning call. ---
    UPROPERTY(EditAnywhere, Category = "Raid|Elites")
    TArray<TSubclassOf<AEliteEnemyBase>> EliteRoster;

    UPROPERTY(EditAnywhere, Category = "Raid|Elites")
    int InitialEliteCount = 4;

    UPROPERTY(EditAnywhere, Category = "Raid|Elites")
    float EliteSpawnRadius = 1400.0;

    // Optional boss spawned once at the arena center (also counts). Unset (BossClass cleared) to skip.
    UPROPERTY(EditAnywhere, Category = "Raid|Elites")
    TSubclassOf<AEliteEnemyBase> BossClass;

    // --- Fodder waves: continuous swarm pressure during the raid (D-0003 fodder). Server-only.
    // Placement is deterministic per master seed (a fresh FRandomStream salted by wave index), so it
    // never perturbs the run's master stream and reproduces for a given seed (CODING_STANDARDS §5). ---
    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    bool bSpawnFodderWaves = true;

    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    float FodderWaveInterval = 7.0;

    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    int FodderPerWave = 8;

    // Rising tension: each successive wave adds this many fodder over the first, capped by FodderMaxPerWave.
    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    float FodderEscalationPerWave = 0.5;

    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    int FodderMaxPerWave = 32;

    // How far from the targeted player a wave's ring is centered.
    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    float FodderSpawnDistance = 1400.0;

    // Soft cap: skip a wave while this many enemies (elites + fodder) are already alive.
    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    int MaxConcurrentEnemies = 60;

    // --- v3 wave director (D-0020): pressure scales with team level + squad size. ---
    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    float FodderPerTeamLevel = 0.8;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    float WaveIntervalReductionPerLevel = 0.35;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    float MinWaveInterval = 3.5;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    int EliteInjectStartLevel = 4;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    int EliteInjectFastLevel = 8;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    float PlayerCountWaveScale = 0.5;

    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    int MaxConcurrentPerExtraPlayer = 5;

    // Injected wave elites rotate through this roster (defaulted in BeginPlay). They are
    // pressure, NOT clear-gates: SetCountsAsObjectiveTarget(false) at spawn.
    UPROPERTY(EditAnywhere, Category = "Raid|Director")
    TArray<TSubclassOf<AEliteEnemyBase>> InjectRoster;

    UPROPERTY(EditAnywhere, Category = "Debug")
    bool bShowDebug = true;

    // Grace window after start so placed elites register before we test "cleared".
    UPROPERTY(EditAnywhere, Category = "Raid")
    float StartGraceSeconds = 1.0;

    UPROPERTY(Replicated, ReplicatedUsing = OnRep_Phase, BlueprintReadOnly, Category = "Raid")
    ERaidPhase Phase = ERaidPhase::InProgress;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Raid")
    float ExtractionSecondsRemaining = 0.0;

    private bool bSeenElites = false;
    private bool bSpawnedInitialElites = false;
    private float Elapsed = 0.0;
    private float WaveTimer = 0.0;
    private int WaveIndex = 0;
    private bool bSpikeBossSpawned = false;

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        if (!HasAuthority())
            return;

        // Starter bio-horde roster so an arena has elites without editor wiring. Override by filling
        // EliteRoster / BossClass on the placed objective (or clear them to disable auto-spawn).
        if (EliteRoster.Num() == 0)
        {
            EliteRoster.Add(ACarapace);
            EliteRoster.Add(ASpitter);
            EliteRoster.Add(ABloater);
            EliteRoster.Add(ALunger);
        }
        if (BossClass.Get() == nullptr)
            BossClass = ABroodMother;
        if (InjectRoster.Num() == 0)
        {
            InjectRoster.Add(ASpitter);
            InjectRoster.Add(ALunger);
        }

        // Plan 3: anchor the channel at the generated objective node and extraction at the separate
        // Extraction node. Move the actor to the channel point so the legacy GetActorLocation()-based
        // helpers (elite ring, fodder centers) follow it; extraction uses ExtractionCenter instead.
        ChannelCenter = GetActorLocation();
        ExtractionCenter = GetActorLocation();
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
    }

    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (bShowDebug)
            DrawDebug();

        if (!HasAuthority())
            return;

        Elapsed += DeltaSeconds;

        if (Phase == ERaidPhase::InProgress)
        {
            UpdateObjective();
            TickFodderWaves(DeltaSeconds);    // swarm pressure while clearing
        }
        else if (Phase == ERaidPhase::ExtractionReady)
        {
            UpdateExtractionReady();          // a living hero on the pad calls extraction
        }
        else if (Phase == ERaidPhase::Extracting)
        {
            UpdateExtraction(DeltaSeconds);
            TickFodderWaves(DeltaSeconds);    // keep the heat on during the defend timer
        }
    }

    // Spawn a director-planned fodder wave; the plan scales with team level, wave index, and
    // squad size (RaidWaveDirector.as), soft-capped by live enemy count.
    private void TickFodderWaves(float DeltaSeconds)
    {
        if (!bSpawnFodderWaves || Elapsed < StartGraceSeconds)
            return;

        int TeamLevel = GetTeamLevel();
        int NumPlayers = GetNumPlayers();
        // The time-spine drives only the generated hold-and-channel loop; the legacy ClearElites map
        // keeps its team-level director untouched (pass 0 elapsed so spikes never fire there).
        float DirectorElapsed = (Mode == EObjectiveMode::HoldAndChannel) ? Elapsed : 0.0;
        FWavePlan Plan = RaidDirector::ComputeWavePlan(TeamLevel, WaveIndex, NumPlayers, MakeTunables(), DirectorElapsed);

        WaveTimer += DeltaSeconds;
        if (WaveTimer < Plan.Interval)
            return;
        WaveTimer = 0.0;

        int ConcurrentCap = MaxConcurrentEnemies + MaxConcurrentPerExtraPlayer * Math::Max(NumPlayers - 1, 0);
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr && Combat.CountEnemiesInSphere(GetActorLocation(), 1000000.0) >= ConcurrentCap)
            return;     // already enough on the field

        USpawnDirector Director = USpawnDirector::Get();
        if (Director == nullptr)
            return;

        FVector Center = PickWaveCenter(WaveIndex);
        Director.SpawnFodderWave(Center, 300.0, Plan.FodderCount);

        if (Plan.EliteInjectIndex >= 0 && InjectRoster.Num() > 0)
        {
            TSubclassOf<AEliteEnemyBase> Cls = InjectRoster[Plan.EliteInjectIndex % InjectRoster.Num()];
            if (Cls.Get() != nullptr)
            {
                AEliteEnemyBase Injected = Director.SpawnElite(Cls, Center + FVector(0.0, 0.0, 40.0), FRotator());
                if (Injected != nullptr)
                {
                    Injected.SetCountsAsObjectiveTarget(false);   // pressure, not a clear-gate
                    Print(f"[Director] wave {WaveIndex}: injected elite (L{TeamLevel})", 3.0);
                }
            }
        }

        if (Plan.bSpawnMiniBoss && !bSpikeBossSpawned)
        {
            bSpikeBossSpawned = true;   // fire once, even if no BossClass is set
            if (BossClass.Get() != nullptr)
            {
                AEliteEnemyBase SpikeBoss = Director.SpawnElite(BossClass, Center + FVector(0.0, 0.0, 40.0), FRotator());
                if (SpikeBoss != nullptr)
                {
                    SpikeBoss.SetCountsAsObjectiveTarget(false);   // pressure spike, not a clear-gate
                    Print("[Director] mini-boss spike", 4.0);
                }
            }
        }
        WaveIndex += 1;
    }

    private FDirectorTunables MakeTunables() const
    {
        FDirectorTunables T;
        T.BaseInterval = FodderWaveInterval;
        T.BasePerWave = FodderPerWave;
        T.EscalationPerWave = FodderEscalationPerWave;
        T.MaxPerWave = FodderMaxPerWave;
        T.FodderPerTeamLevel = FodderPerTeamLevel;
        T.IntervalReductionPerLevel = WaveIntervalReductionPerLevel;
        T.MinInterval = MinWaveInterval;
        T.EliteInjectStartLevel = EliteInjectStartLevel;
        T.EliteInjectFastLevel = EliteInjectFastLevel;
        T.PlayerCountWaveScale = PlayerCountWaveScale;
        return T;
    }

    private int GetTeamLevel() const
    {
        ARaidGameState GameState = Cast<ARaidGameState>(Gameplay::GetGameState());
        return GameState != nullptr ? GameState.TeamLevel : 1;
    }

    private int GetNumPlayers() const
    {
        ARaidGameState GameState = Cast<ARaidGameState>(Gameplay::GetGameState());
        return GameState != nullptr ? Math::Max(GameState.PlayerArray.Num(), 1) : 1;
    }

    // Deterministic per-seed wave center: a player (cycled by wave index), offset by a seeded
    // ground direction. A fresh stream salted by the wave index keeps the master stream untouched.
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

        FRandomStream WaveRng(GetMasterSeed() + Index * 7919);
        FVector Dir = WaveRng.GetUnitVector();
        Dir.Z = 0.0;
        if (Dir.SizeSquared() < 0.01)
            Dir = FVector(1.0, 0.0, 0.0);
        Dir = Dir.GetSafeNormal();

        // Ring sits FodderSpawnDistance out, a touch above the floor so the bodies read.
        return Base + Dir * FodderSpawnDistance + FVector(0.0, 0.0, -40.0);
    }

    private int GetMasterSeed() const
    {
        ARaidGameState GameState = Cast<ARaidGameState>(Gameplay::GetGameState());
        return GameState != nullptr ? GameState.MasterSeed : 1;
    }

    // Spawn the gating elites once: an optional boss at the center plus a seeded mix from EliteRoster on a
    // ring. Deterministic per master seed (a fresh stream salted off it, never touching the master stream).
    private void SpawnInitialElites()
    {
        bSpawnedInitialElites = true;

        USpawnDirector Director = USpawnDirector::Get();
        if (Director == nullptr)
            return;

        FVector Center = GetActorLocation();
        int Spawned = 0;
        bool bBoss = false;

        if (BossClass.Get() != nullptr)
        {
            AEliteEnemyBase Boss = Director.SpawnElite(BossClass, Center, FRotator());
            if (Boss != nullptr)
                Boss.SetCountsAsObjectiveTarget(true);
            bBoss = true;
        }

        if (EliteRoster.Num() > 0 && InitialEliteCount > 0)
        {
            FRandomStream Rng(GetMasterSeed() + 104729);
            for (int i = 0; i < InitialEliteCount; i++)
            {
                TSubclassOf<AEliteEnemyBase> Cls = EliteRoster[Rng.RandRange(0, EliteRoster.Num() - 1)];
                if (Cls.Get() == nullptr)
                    continue;
                float Angle = (2.0 * 3.14159265 * i) / InitialEliteCount;
                FVector Offset = FVector(Math::Cos(Angle), Math::Sin(Angle), 0.0) * EliteSpawnRadius;
                AEliteEnemyBase Ring = Director.SpawnElite(Cls, Center + Offset, FRotator());
                if (Ring != nullptr)
                    Ring.SetCountsAsObjectiveTarget(true);
                Spawned += 1;
            }
        }

        // Loop-debug breadcrumb: confirms the gating elites placed (and how many gate the clear).
        FString BossNote = bBoss ? " + boss" : "";
        Print(f"[Raid] spawned {Spawned} ring elites{BossNote} — clear them to open extraction", 5.0);
    }

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

    // Once the arena is clear, any living hero standing inside the extraction zone calls it in.
    private void UpdateExtractionReady()
    {
        const float RadiusSq = ExtractZoneRadius * ExtractZoneRadius;
        const FVector Center = ExtractionCenter;

        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        for (AHeroCharacter H : Heroes)
        {
            if (H == nullptr || H.IsIncapacitated())
                continue;
            if ((H.GetActorLocation() - Center).SizeSquared() <= RadiusSq)
            {
                CallExtraction();
                return;
            }
        }
    }

    // Authority entry point. Host calls directly; remote clients route via
    // AHeroCharacter::Server_CallExtraction (clients can't RPC this unowned actor).
    UFUNCTION(BlueprintCallable, Category = "Raid")
    void CallExtraction()
    {
        if (!HasAuthority() || Phase != ERaidPhase::ExtractionReady)
            return;

        ExtractionSecondsRemaining = ExtractionDefendSeconds;
        SetPhase(ERaidPhase::Extracting);
        OnExtractionPhaseStarted();      // hook: spawn the final defend wave
    }

    private void UpdateExtraction(float DeltaSeconds)
    {
        ExtractionSecondsRemaining = Math::Max(0.0, ExtractionSecondsRemaining - DeltaSeconds);
        if (ExtractionSecondsRemaining <= 0.0)
            SetPhase(ERaidPhase::Extracted);     // survived the hold -> win
    }

    // Call when the whole party is down. Depends on down/revive (MVP §12, not yet built).
    UFUNCTION(BlueprintCallable, Category = "Raid")
    void NotifyPartyWiped()
    {
        if (!HasAuthority())
            return;
        if (Phase == ERaidPhase::Extracted || Phase == ERaidPhase::Failed)
            return;

        SetPhase(ERaidPhase::Failed);
    }

    private void SetPhase(ERaidPhase NewPhase)
    {
        Phase = NewPhase;
        OnPhaseChanged(NewPhase);        // server-side reaction
    }

    // White = clearing · Green = extraction ready · Red = defending · Blue = extracted.
    private void DrawDebug()
    {
        if (!RaidDebug::bEnabled)
            return;

        FLinearColor Color = FLinearColor::White;
        FString Label = "RAID: clearing";
        if (Phase == ERaidPhase::ExtractionReady)
        {
            Color = FLinearColor::Green;
            Label = "RAID: extraction ready";
        }
        else if (Phase == ERaidPhase::Extracting)
        {
            Color = FLinearColor::Red;
            Label = f"DEFEND: {int(ExtractionSecondsRemaining)}s";
        }
        else if (Phase == ERaidPhase::Extracted)
        {
            Color = FLinearColor::Blue;
            Label = "EXTRACTED";
        }
        else if (Phase == ERaidPhase::Failed)
        {
            Label = "FAILED";
        }

        System::DrawDebugSphere(GetActorLocation(), DefendWaveRadius, 32, Color, 0.0, 3.0);
        System::DrawDebugString(GetActorLocation() + FVector(0.0, 0.0, 120.0), Label, nullptr, Color, 0.0);
    }

    UFUNCTION()
    void OnRep_Phase()
    {
        // Clients: drive objective UI off the new phase. (No gameplay logic here.)
    }

    // ---- Hooks (override in a script/BP subclass; replace Prints with real cues) ----
    void OnExtractionPhaseStarted()
    {
        Print("EXTRACTION CALLED - defend the zone!", 5.0);

        // Spawn the final wave through the spawn seam (pooled elites now, Mass fodder later).
        USpawnDirector Director = USpawnDirector::Get();
        if (Director != nullptr)
            Director.SpawnEliteWave(DefendWaveEliteClass, ExtractionCenter, DefendWaveRadius, DefendWaveCount);
    }

    void OnPhaseChanged(ERaidPhase NewPhase)
    {
        if (NewPhase == ERaidPhase::ExtractionReady)
        {
            Print("OBJECTIVE COMPLETE - call extraction", 5.0);

            // Reward clearing the arena with an upgrade choice (GDD §6.1, D-0013).
            ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
            if (GameMode != nullptr)
                GameMode.OfferUpgradesToAll();
        }
        else if (NewPhase == ERaidPhase::Extracted)
        {
            Print("EXTRACTED - raid won!", 8.0);
            EndRunResult(true);              // resolve the RUN-level phase -> Victory
        }
        else if (NewPhase == ERaidPhase::Failed)
        {
            Print("PARTY WIPED - raid failed", 8.0);
            EndRunResult(false);             // resolve the RUN-level phase -> Defeat
        }
    }

    // Bridge the in-raid outcome to the run state machine: the RunManager flips ARaidGameState.Phase
    // to Victory/Defeat, which replicates so every client's results UI can react. Without this the
    // run-level phase would sit on InProgress forever even though the raid is over.
    private void EndRunResult(bool bVictory)
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (GameMode == nullptr)
            return;
        URunManager RunManager = GameMode.GetRunManager();
        if (RunManager != nullptr)
            RunManager.EndRun(bVictory);
    }
}
