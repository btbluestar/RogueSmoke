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

    // Hero select: players enter as spectators and get their pawn from HandleHeroChoice once
    // their (travel-surviving) pick arrives. Spawning the hero exactly ONCE matters: ability
    // sets grant onto the PlayerState ASC, so a default-pawn-then-swap flow would double-grant
    // kits. The lobby pick is stashed in URaidSessionSubsystem and re-sent by the raid PC's
    // BeginPlay, so this also covers direct PIE play (choice -1 -> first/default hero).
    default bStartPlayersAsSpectators = true;

    // Pawn class per RogueHeroes roster index (0 = Vanguard, 1 = Bombardier). Content
    // references, so they're assigned on BP_RaidGamemode, not here.
    UPROPERTY(EditDefaultsOnly, Category = "Heroes")
    TArray<TSubclassOf<AHeroCharacter>> HeroPawnClasses;

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

    // Spawn the player's chosen hero (server only; called via the raid PC's Server_SetHeroChoice).
    // One-shot per player: ignored if they already have a hero body (see bStartPlayersAsSpectators
    // note above for why there is no default-then-swap).
    void HandleHeroChoice(APlayerController Player, int HeroIndex)
    {
        if (!HasAuthority() || Player == nullptr)
            return;
        if (Cast<AHeroCharacter>(Player.GetControlledPawn()) != nullptr)
            return;   // already embodied

        TSubclassOf<AHeroCharacter> PawnClass;
        if (HeroIndex >= 0 && HeroIndex < HeroPawnClasses.Num())
            PawnClass = HeroPawnClasses[HeroIndex];
        if (PawnClass.Get() == nullptr && HeroPawnClasses.Num() > 0)
            PawnClass = HeroPawnClasses[0];

        AHeroCharacter Hero;
        if (PawnClass.Get() != nullptr)
            Hero = Cast<AHeroCharacter>(SpawnActor(PawnClass, PickHeroSpawnPoint(), FRotator()));
        else
        {
            // Content not wired yet — fall back to the GameMode's DefaultPawnClass (BP_Vanguard).
            UClass FallbackClass = DefaultPawnClass.Get();
            if (FallbackClass != nullptr)
                Hero = Cast<AHeroCharacter>(SpawnActor(FallbackClass, PickHeroSpawnPoint(), FRotator()));
        }
        if (Hero == nullptr)
            return;

        APawn OldPawn = Player.GetControlledPawn();
        Player.Possess(Hero);
        if (OldPawn != nullptr && Cast<AHeroCharacter>(OldPawn) == nullptr)
            OldPawn.DestroyActor();   // drop the spectator shell

        FString HeroLabel = RogueHeroes::IsValidIndex(HeroIndex)
            ? RogueHeroes::Get(HeroIndex).Name : "DEFAULT";
        Print(f"[Raid] hero embodied: choice={HeroIndex} ({HeroLabel})", 4.0);
    }

    // PlayerStarts in placement order, offset per already-spawned hero so a squad doesn't
    // stack inside one start point.
    private FVector PickHeroSpawnPoint() const
    {
        TArray<AHeroCharacter> Existing;
        GetAllActorsOfClass(Existing);

        TArray<APlayerStart> Starts;
        GetAllActorsOfClass(Starts);
        if (Starts.Num() > 0)
        {
            APlayerStart Start = Starts[Existing.Num() % Starts.Num()];
            FVector Base = Start.GetActorLocation();
            int Wrap = Existing.Num() / Starts.Num();
            return Base + FVector(120.0 * Wrap, 0.0, 0.0);
        }
        return FVector(0.0, 120.0 * Existing.Num(), 200.0);
    }

    // --- Roguelike upgrades (D-0013). Designer pool of URogueUpgradeDef (each carries a GameplayEffect);
    // offered on milestones, rolled per master seed. Assign the pool on BP_RaidGamemode. ---
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades")
    TArray<URogueUpgradeDef> UpgradePool;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrades")
    int OptionsPerOffer = 3;

    private int OfferCounter = 0;

    // Roll OptionsPerOffer distinct upgrades and present them to every player's client. Server only.
    UFUNCTION(BlueprintCallable, Category = "Upgrades")
    void OfferUpgradesToAll()
    {
        if (!HasAuthority() || UpgradePool.Num() == 0)
            return;

        TArray<URogueUpgradeDef> Options = RollOptions(OptionsPerOffer, OfferCounter);
        OfferCounter += 1;
        if (Options.Num() == 0)
            return;

        TArray<ARaidPlayerController> PCs;
        GetAllActorsOfClass(PCs);
        for (ARaidPlayerController PC : PCs)
        {
            if (PC != nullptr)
                PC.Client_OfferUpgrades(Options);   // Client RPC -> shows the pick screen
        }
    }

    // Distinct picks via a FRESH stream salted by the offer index, so upgrade rolls reproduce per
    // seed yet never perturb the run's master stream (CODING_STANDARDS §5; same trick as fodder waves).
    private TArray<URogueUpgradeDef> RollOptions(int Count, int Salt)
    {
        TArray<URogueUpgradeDef> Result;
        TArray<URogueUpgradeDef> Available = UpgradePool;

        int BaseSeed = (RunManager != nullptr) ? RunManager.GetStream().GetInitialSeed() : 1;
        FRandomStream Rng(BaseSeed + Salt * 6151);

        int Want = Math::Min(Count, Available.Num());
        for (int i = 0; i < Want; i++)
        {
            int Idx = Rng.RandRange(0, Available.Num() - 1);
            Result.Add(Available[Idx]);
            Available.RemoveAt(Idx);
        }
        return Result;
    }
}
