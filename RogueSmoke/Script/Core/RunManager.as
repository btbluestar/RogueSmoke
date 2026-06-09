// RunManager.as
// Server-only owner of the master seed and the run state machine (ARCHITECTURE §3, §4.1).
// One per raid: created and held by ARaidGameMode. It is the single place that mutates the
// run's authoritative state; it pushes the results into ARaidGameState, which replicates
// them to clients (D-0004, D-0007).
//
// Why a component (not a UObject): components match the project's existing composition
// idiom and get a clean Create() + lifetime tied to the GameMode. The GameMode exists only
// on the server, so this is implicitly server-only.

class URunManager : UActorComponent
{
    // The seeded stream every deterministic generator must draw from (CODING_STANDARDS §5).
    // Never call unseeded global random for world-affecting decisions — draw from this.
    private FRandomStream Stream;
    private bool bRunStarted = false;

    // Optional: force a specific seed for repro/testing. 0 = roll a fresh one.
    UPROPERTY(EditDefaultsOnly, Category = "Run|Debug")
    int DebugForcedSeed = 0;

    // Begin a run: roll (or accept) the master seed, seed the stream, and broadcast the seed
    // through GameState so clients reproduce the same world. Server only.
    void StartRun()
    {
        if (bRunStarted)
            return;

        int Seed = DebugForcedSeed;
        if (Seed == 0)
        {
            // The ONE permitted entropy draw: pick the master seed. Everything downstream is
            // deterministic from it. FRandomStream::GenerateNewSeed seeds from the platform RNG.
            FRandomStream Roller;
            Roller.GenerateNewSeed();
            Seed = Roller.GetCurrentSeed();
            if (Seed == 0)              // GetCurrentSeed never returns 0 in practice, but guard it
                Seed = 1;
        }

        Stream.Initialize(Seed);
        bRunStarted = true;

        ARaidGameState GameState = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GameState != nullptr)
        {
            GameState.MasterSeed = Seed;
            GameState.FloorNumber = 1;
            GameState.Phase = ERunPhase::InProgress;
        }

        Print(f"[RunManager] Run started — master seed {Seed}", 6.0);
    }

    // The seeded stream for deterministic generators (level layout, loot rolls, waves).
    FRandomStream& GetStream()
    {
        return Stream;
    }

    bool IsRunStarted() const { return bRunStarted; }

    // Advance the run to a terminal result and replicate it. Server only.
    void EndRun(bool bVictory)
    {
        ARaidGameState GameState = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GameState != nullptr)
            GameState.Phase = bVictory ? ERunPhase::Victory : ERunPhase::Defeat;
    }
}
