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
//  - The defend wave is a HOOK (OnExtractionPhaseStarted) — the spawner isn't built yet.
//  - Party-wipe detection depends on down/revive (MVP §12, not built) — exposed as a hook.

enum ERaidPhase
{
    InProgress,        // clearing the arena
    ExtractionReady,   // objective done; players may call extraction
    Extracting,        // defend timer running
    Extracted,         // survived the hold -> WIN
    Failed             // party wiped -> LOSS
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
    int FodderMaxPerWave = 20;

    // How far from the targeted player a wave's ring is centered.
    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    float FodderSpawnDistance = 1400.0;

    // Soft cap: skip a wave while this many enemies (elites + fodder) are already alive.
    UPROPERTY(EditAnywhere, Category = "Raid|Fodder Waves")
    int MaxConcurrentEnemies = 60;

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

    // Spawn a deterministic fodder wave every FodderWaveInterval, soft-capped by live enemy count.
    private void TickFodderWaves(float DeltaSeconds)
    {
        if (!bSpawnFodderWaves || Elapsed < StartGraceSeconds)
            return;

        WaveTimer += DeltaSeconds;
        if (WaveTimer < FodderWaveInterval)
            return;
        WaveTimer = 0.0;

        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr && Combat.CountEnemiesInSphere(GetActorLocation(), 1000000.0) >= MaxConcurrentEnemies)
            return;     // already enough on the field

        USpawnDirector Director = USpawnDirector::Get();
        if (Director == nullptr)
            return;

        int WaveCount = Math::Min(FodderPerWave + int(WaveIndex * FodderEscalationPerWave), FodderMaxPerWave);
        Director.SpawnFodderWave(PickWaveCenter(WaveIndex), 300.0, WaveCount);
        WaveIndex += 1;
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
            Director.SpawnElite(BossClass, Center, FRotator());
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
                Director.SpawnElite(Cls, Center + Offset, FRotator());
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

        // Place the elites that gate the clear, once, after the grace window (so they register before we
        // first test "cleared").
        if (!bSpawnedInitialElites)
            SpawnInitialElites();

        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
            return;

        int Remaining = Combat.GetEliteCount();
        if (Remaining > 0)
            bSeenElites = true;

        if (bSeenElites && Remaining == 0)
            SetPhase(ERaidPhase::ExtractionReady);
    }

    // Once the arena is clear, any living hero standing inside the extraction zone calls it in.
    private void UpdateExtractionReady()
    {
        const float RadiusSq = ExtractZoneRadius * ExtractZoneRadius;
        const FVector Center = GetActorLocation();

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
            Director.SpawnEliteWave(DefendWaveEliteClass, GetActorLocation(), DefendWaveRadius, DefendWaveCount);
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
