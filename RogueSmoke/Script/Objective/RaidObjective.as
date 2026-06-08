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

    // The final defend wave spawned when extraction is called (D-0010). Leave the class
    // unset to skip spawning (e.g. while testing the timer alone).
    UPROPERTY(EditAnywhere, Category = "Raid|Defend Wave")
    TSubclassOf<AEliteEnemyBase> DefendWaveEliteClass;

    UPROPERTY(EditAnywhere, Category = "Raid|Defend Wave")
    int DefendWaveCount = 8;

    UPROPERTY(EditAnywhere, Category = "Raid|Defend Wave")
    float DefendWaveRadius = 1200.0;

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
    private float Elapsed = 0.0;

    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (bShowDebug)
            DrawDebug();

        if (!HasAuthority())
            return;

        Elapsed += DeltaSeconds;

        if (Phase == ERaidPhase::InProgress)
            UpdateObjective();
        else if (Phase == ERaidPhase::Extracting)
            UpdateExtraction(DeltaSeconds);
    }

    private void UpdateObjective()
    {
        if (Elapsed < StartGraceSeconds)
            return;

        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
            return;

        int Remaining = Combat.GetEliteCount();
        if (Remaining > 0)
            bSeenElites = true;

        if (bSeenElites && Remaining == 0)
            SetPhase(ERaidPhase::ExtractionReady);
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
            Print("OBJECTIVE COMPLETE - call extraction", 5.0);
        else if (NewPhase == ERaidPhase::Extracted)
            Print("EXTRACTED - raid won!", 8.0);
        else if (NewPhase == ERaidPhase::Failed)
            Print("PARTY WIPED - raid failed", 8.0);
    }
}
