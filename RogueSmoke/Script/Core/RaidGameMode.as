// RaidGameMode.as
// Server-only run rules for an active raid (ARCHITECTURE §2, §3). Exists only on the host;
// clients never see it. Creates the URunManager, rolls the master seed at level start, and
// owns win/lose progression.
//
// SETUP: make BP_RaidGameMode (child of this), assign PlayerControllerClass =
// BP_RaidPlayerController and DefaultPawnClass = a BP_HeroCharacter, then set it as the
// GameMode Override in the Raid map's World Settings. GameStateClass is wired in script.

class ARaidGameMode : AGameModeBase
{
    // Pure-script class, no asset needed, so we can wire it here (the rest stay BP — they
    // reference assets with input/mesh assigned).
    default GameStateClass = ARaidGameState;

    // GAS lives on the PlayerState (Lyra-style; survives pawn respawn). ARoguePlayerState (C++)
    // owns the AngelScript-driven AbilitySystemComponent. No asset needed, so wire it here.
    default PlayerStateClass = ARoguePlayerState;

    // Server-only owner of the seed + run state machine. Created on BeginPlay.
    UPROPERTY(BlueprintReadOnly, Category = "Run")
    URunManager RunManager;

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        // GameMode only ever exists on the server, but assert the invariant for clarity.
        if (!HasAuthority())
            return;

        RunManager = URunManager::Create(this);
        RunManager.StartRun();      // roll + replicate the master seed (D-0007)
    }

    // Convenience for ability/objective code: reach the seeded stream from anywhere on the
    // server. Returns null on clients (no GameMode there) — callers must null-check.
    UFUNCTION(BlueprintCallable, Category = "Run")
    URunManager GetRunManager()
    {
        return RunManager;
    }
}
