// RaidGameState.as
// Replicated run state — the single source clients read to reproduce deterministic
// decisions (ARCHITECTURE §2 table, §4.1). Server-authored via URunManager; clients
// never write it. Lives on server + every client.
//
// ERunPhase is the RUN-LIFECYCLE phase (menu -> lobby -> run -> results). It is distinct
// from RaidObjective.as's ERaidPhase, which is the WITHIN-RAID objective phase
// (clearing -> extraction-ready -> extracting -> ...). Both are intentional and separate.

enum ERunPhase
{
    None,           // not in a run yet (menu / lobby)
    Generating,     // server is laying out the floor from the seed
    InProgress,     // floor loop is live
    Victory,        // run won (extracted)
    Defeat          // run lost (party wipe)
}

class ARaidGameState : AGameStateBase
{
    // The master seed for the whole run (D-0007). Server rolls it once; everyone else
    // reads this replicated copy so generation reproduces identically across machines.
    UPROPERTY(Replicated, ReplicatedUsing = OnRep_MasterSeed, BlueprintReadOnly, Category = "Run")
    int MasterSeed = 0;

    // Plan 3: set true by the GameMode when this raid uses a generated arena, so clients stamp on OnRep.
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    bool bGeneratedArena = false;

    UPROPERTY(Replicated, ReplicatedUsing = OnRep_Phase, BlueprintReadOnly, Category = "Run")
    ERunPhase Phase = ERunPhase::None;

    // Which floor of the raid we're on (D-0009: one raid for the MVP, but the field is
    // here so the floor loop has somewhere to count).
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    int FloorNumber = 0;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    int SharedScore = 0;

    // Run clock (world seconds). Start is stamped when the run begins; End is 0 until it resolves, then
    // stamped so the HUD freezes the final time. Replicated so clients show the same clock.
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    float RunStartTime = 0.0;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    float RunEndTime = 0.0;

    // --- Shared team XP (UpgradeLoop concept, 2026-06-11): every kill feeds ONE team pool; each
    // level-up pauses the raid and offers an upgrade pick to all players. Server-written by the
    // GameMode; replicated so every HUD shows the same level/bar. ---
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    int TeamLevel = 1;

    // Progress INTO the current level (resets each level-up), not lifetime XP.
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    float TeamXP = 0.0;

    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    float XPToNextLevel = 50.0;

    // --- Loop v2 (D-0019): pick-flow state the card screen renders. Server-written. ---

    // Display names of players who still owe a pick while the raid is pick-paused.
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    TArray<FString> AwaitingPickNames;

    // Squad-shared reroll budget (any player may spend one on their own hand).
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    int SquadRerollsRemaining = 1;

    // Lobby launch countdown: 0 until the host hits START RAID, then the world time the travel
    // fires at. Replicated so every lobby UI shows the same "Launching in N..." count.
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Lobby")
    float RaidLaunchAt = 0.0;

    // True once the server has rolled a real seed (0 means "not started yet").
    UFUNCTION(BlueprintPure, Category = "Run")
    bool HasValidSeed() const
    {
        return MasterSeed != 0;
    }

    // How many players are connected — replicated via PlayerArray, so valid on clients too.
    // The lobby UI reads this to show "2/4 ready".
    UFUNCTION(BlueprintPure, Category = "Run")
    int GetConnectedPlayerCount() const
    {
        return PlayerArray.Num();
    }

    // Clients: drive run-level UI (loading spinner, results screen) off the new phase.
    // No gameplay logic here — the server already decided.
    UFUNCTION()
    void OnRep_Phase()
    {
    }

    // Clients re-stamp the generated arena locally once the seed arrives (server stamped in
    // GameMode::BeginPlay). Deterministic from the replicated seed — no geometry is replicated.
    UFUNCTION()
    void OnRep_MasterSeed()
    {
        if (bGeneratedArena && MasterSeed != 0)
            RaidArena::BuildFromSeed(MasterSeed);
    }
}
