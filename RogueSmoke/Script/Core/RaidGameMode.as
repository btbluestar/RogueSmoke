// RaidGameMode.as
// Server-only run rules for an active raid (ARCHITECTURE §2, §3). Exists only on the host;
// clients never see it. Creates the URunManager, rolls the master seed at level start, and
// owns win/lose progression.
//
// SETUP: make BP_RaidGameMode (child of this), assign PlayerControllerClass =
// BP_RaidPlayerController and DefaultPawnClass = a BP_HeroCharacter, then set it as the
// GameMode Override in the Raid map's World Settings. GameStateClass is wired in script.

// Per-player upgrade bookkeeping (Loop v2, D-0019). Server-only — lives on the GameMode, never
// replicated; the UI gets what it needs in the offer RPC payload instead.
struct FUpgradeStackEntry
{
    UPROPERTY()
    URogueUpgradeDef Def;

    UPROPERTY()
    int Count = 0;
}

struct FPlayerUpgradeRecord
{
    UPROPERTY()
    APlayerState Player;

    UPROPERTY()
    ARaidPlayerController PC;

    UPROPERTY()
    TArray<FUpgradeStackEntry> Stacks;

    // Non-empty while this player owes a pick; the server validates picks against it and
    // auto-applies entry 0 on watchdog timeout.
    UPROPERTY()
    TArray<URogueUpgradeDef> PendingHand;
}

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

        // Upgrade loop (UpgradeLoop concept, 2026-06-11): shared XP from every kill + the
        // mini-boss chest. The pick-pause watchdog below must tick while the game is paused.
        SetTickableWhenPaused(true);
        USpawnDirector Director = USpawnDirector::Get();
        if (Director != nullptr)
            Director.OnEnemyKilled.AddUFunction(this, n"HandleEnemyKilled");
    }

    // Pick-pause watchdog: resume even if a player never picks (disconnect / walked away).
    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (PendingPicks > 0)
        {
            OfferPauseElapsed += DeltaSeconds;
            if (OfferPauseElapsed >= OfferPauseTimeoutSeconds)
                ResumeFromOffer("pick timeout");
        }
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

    // Host action (results / escape menu): take the whole party back to the hero-select lobby.
    // ServerTravel keeps clients connected (same mechanism as the lobby -> raid launch).
    UFUNCTION(BlueprintCallable, Category = "Run")
    void TravelToLobby()
    {
        if (!HasAuthority())
            return;
        UWorld World = GetWorld();
        if (World != nullptr)
            World.ServerTravel("/Game/Levels/L_Lobby", true, false);
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

    // --- Shared team XP (UpgradeLoop concept). Curve: level N -> N+1 needs
    // XPBasePerLevel + XPGrowthPerLevel * (N - 1). Kills feed AddTeamXP via HandleEnemyKilled. ---
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|XP")
    float XPBasePerLevel = 100.0;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|XP")
    float XPGrowthPerLevel = 25.0;

    // Resume safety: if someone never picks (disconnect / AFK), unpause anyway after this long.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades")
    float OfferPauseTimeoutSeconds = 30.0;

    private int OfferCounter = 0;
    private int PendingPicks = 0;
    private float OfferPauseElapsed = 0.0;

    private TArray<FPlayerUpgradeRecord> PlayerRecords;

    private int FindOrAddRecord(ARaidPlayerController PC)
    {
        APlayerState PS = PC.PlayerState;
        for (int i = 0; i < PlayerRecords.Num(); i++)
        {
            if (PlayerRecords[i].Player == PS)
            {
                PlayerRecords[i].PC = PC;
                return i;
            }
        }
        FPlayerUpgradeRecord Rec;
        Rec.Player = PS;
        Rec.PC = PC;
        PlayerRecords.Add(Rec);
        return PlayerRecords.Num() - 1;
    }

    // Public for the flow-smoke exec; gameplay code only calls it from ApplyUpgradeFor.
    int GetStackCount(APlayerState Player, URogueUpgradeDef Def) const
    {
        for (int i = 0; i < PlayerRecords.Num(); i++)
        {
            if (PlayerRecords[i].Player != Player)
                continue;
            for (int j = 0; j < PlayerRecords[i].Stacks.Num(); j++)
            {
                if (PlayerRecords[i].Stacks[j].Def == Def)
                    return PlayerRecords[i].Stacks[j].Count;
            }
            return 0;
        }
        return 0;
    }

    // Public for the flow-smoke exec (it fabricates build states to test filtering).
    void AddStack(APlayerState Player, URogueUpgradeDef Def)
    {
        if (Player == nullptr || Def == nullptr)
            return;
        for (int i = 0; i < PlayerRecords.Num(); i++)
        {
            if (PlayerRecords[i].Player != Player)
                continue;
            for (int j = 0; j < PlayerRecords[i].Stacks.Num(); j++)
            {
                if (PlayerRecords[i].Stacks[j].Def == Def)
                {
                    PlayerRecords[i].Stacks[j].Count += 1;
                    return;
                }
            }
            FUpgradeStackEntry Entry;
            Entry.Def = Def;
            Entry.Count = 1;
            PlayerRecords[i].Stacks.Add(Entry);
            return;
        }
        // No record yet (flow-smoke before any offer): create one without a PC.
        FPlayerUpgradeRecord Rec;
        Rec.Player = Player;
        FUpgradeStackEntry Entry;
        Entry.Def = Def;
        Entry.Count = 1;
        Rec.Stacks.Add(Entry);
        PlayerRecords.Add(Rec);
    }

    // Cap + prerequisite gate. Self-scope prereqs check the candidate player; squad-scope (duo)
    // prereqs need A and B on two different players — except solo, where one player may hold both.
    bool IsEligible(URogueUpgradeDef Def, APlayerState ForPlayer) const
    {
        if (Def == nullptr)
            return false;
        if (Def.MaxStacks > 0 && GetStackCount(ForPlayer, Def) >= Def.MaxStacks)
            return false;

        if (Def.bPrereqSelf)
        {
            if (Def.PrereqA != nullptr && GetStackCount(ForPlayer, Def.PrereqA) < Def.PrereqAStacks)
                return false;
            if (Def.PrereqB != nullptr && GetStackCount(ForPlayer, Def.PrereqB) < Def.PrereqBStacks)
                return false;
            return true;
        }

        if (Def.PrereqA == nullptr)
            return true;   // no squad gate authored

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS == nullptr)
            return false;
        bool bSolo = GS.PlayerArray.Num() <= 1;

        for (int a = 0; a < GS.PlayerArray.Num(); a++)
        {
            APlayerState HolderA = GS.PlayerArray[a];
            if (HolderA == nullptr || GetStackCount(HolderA, Def.PrereqA) < Def.PrereqAStacks)
                continue;
            if (Def.PrereqB == nullptr)
                return true;
            for (int b = 0; b < GS.PlayerArray.Num(); b++)
            {
                APlayerState HolderB = GS.PlayerArray[b];
                if (HolderB == nullptr || GetStackCount(HolderB, Def.PrereqB) < Def.PrereqBStacks)
                    continue;
                if (HolderA != HolderB || bSolo)
                    return true;
            }
        }
        return false;
    }

    // Every pooled enemy death lands here (SpawnDirector.OnEnemyKilled): XP for the team pool,
    // and the mini-boss drops the synergy chest where it fell.
    UFUNCTION()
    private void HandleEnemyKilled(AEliteEnemyBase Enemy)
    {
        if (Enemy == nullptr)
            return;
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS == nullptr || GS.Phase != ERunPhase::InProgress)
            return;     // no XP once the run has resolved (or before it starts)

        if (Enemy.XPValue > 0.0)
            AddTeamXP(Enemy.XPValue);

        if (Cast<ABroodMother>(Enemy) != nullptr)
            SpawnUpgradeChest(Enemy.GetActorLocation() + FVector(0.0, 0.0, 60.0));
    }

    // Add to the shared pool; on level-up, pause and offer a pick with level-biased rarity:
    // default common-leaning, every 5th level boosts moderate, every 10th boosts rare.
    UFUNCTION(BlueprintCallable, Category = "Upgrades")
    void AddTeamXP(float Amount)
    {
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (!HasAuthority() || GS == nullptr || Amount <= 0.0)
            return;

        GS.TeamXP += Amount;
        bool bLeveled = false;
        while (GS.TeamXP >= GS.XPToNextLevel)
        {
            GS.TeamXP -= GS.XPToNextLevel;
            GS.TeamLevel += 1;
            GS.XPToNextLevel = XPBasePerLevel + XPGrowthPerLevel * float(GS.TeamLevel - 1);
            bLeveled = true;
        }
        if (!bLeveled)
            return;

        // One offer per XP burst even if it crossed several levels (no stacked pick screens);
        // the bias comes from the level REACHED. Weights are balance-pass numbers.
        float W1 = 70.0; float W2 = 25.0; float W3 = 5.0;
        FString Tier = "standard";
        if (GS.TeamLevel % 10 == 0)
        {
            W1 = 15.0; W2 = 30.0; W3 = 55.0; Tier = "RARE-boosted";
        }
        else if (GS.TeamLevel % 5 == 0)
        {
            W1 = 30.0; W2 = 55.0; W3 = 15.0; Tier = "moderate-boosted";
        }
        Print(f"[XP] team level {GS.TeamLevel} — {Tier} upgrade offer", 5.0);
        OfferUpgradesWeighted(W1, W2, W3, false);
    }

    // The mini-boss reward (UpgradeLoop right branch): chest where the boss fell; any living
    // player standing next to it opens it -> paused synergy pick for everyone.
    private void SpawnUpgradeChest(FVector Location)
    {
        AUpgradeChest Chest = Cast<AUpgradeChest>(SpawnActor(AUpgradeChest, Location, FRotator()));
        if (Chest == nullptr)
            return;
        Chest.OnOpened.AddUFunction(this, n"HandleChestOpened");
        Print("[Chest] mini-boss chest dropped — stand next to it to open", 6.0);
    }

    UFUNCTION()
    private void HandleChestOpened(AUpgradeChest Chest)
    {
        Print("[Chest] opened — synergy upgrade pick", 5.0);
        OfferUpgradesWeighted(1.0, 1.0, 1.0, true);   // synergy pool; rarity weights moot
    }

    // Roll OptionsPerOffer distinct upgrades (default weights) and present them to every player.
    // Kept for the arena-clear reward path (RaidObjective). Server only.
    UFUNCTION(BlueprintCallable, Category = "Upgrades")
    void OfferUpgradesToAll()
    {
        OfferUpgradesWeighted(70.0, 25.0, 5.0, false);
    }

    // The real offer path: weighted roll, send to every client, then PAUSE the raid until every
    // player has picked (or the watchdog timeout). Synergy-only offers are the chest's.
    void OfferUpgradesWeighted(float W1, float W2, float W3, bool bSynergyOnly)
    {
        if (!HasAuthority() || UpgradePool.Num() == 0)
            return;

        TArray<URogueUpgradeDef> Options = RollOptions(OptionsPerOffer, OfferCounter, W1, W2, W3, bSynergyOnly);
        OfferCounter += 1;
        if (Options.Num() == 0)
            return;

        int Sent = 0;
        TArray<ARaidPlayerController> PCs;
        GetAllActorsOfClass(PCs);
        for (ARaidPlayerController PC : PCs)
        {
            if (PC != nullptr)
            {
                PC.Client_OfferUpgrades(Options);   // Client RPC -> shows the pick screen
                Sent += 1;
            }
        }

        if (Sent > 0)
        {
            PendingPicks = Sent;
            OfferPauseElapsed = 0.0;
            RogueGame::SetRaidPaused(true);   // URogueGameStatics — 'Statics' suffix stripped
            Print(f"[Upgrades] raid paused for the pick ({Sent} player(s))", 4.0);
        }
    }

    // Called by Server_ApplyUpgrade for each pick; the raid resumes once everyone has chosen.
    void NotifyUpgradePicked()
    {
        if (!HasAuthority() || PendingPicks <= 0)
            return;
        PendingPicks -= 1;
        if (PendingPicks == 0)
            ResumeFromOffer("all players picked");
    }

    private void ResumeFromOffer(FString Why)
    {
        PendingPicks = 0;
        RogueGame::SetRaidPaused(false);
        Print(f"[Upgrades] raid resumed ({Why})", 4.0);
    }

    // Distinct weighted picks via a FRESH stream salted by the offer index, so upgrade rolls
    // reproduce per seed yet never perturb the run's master stream (CODING_STANDARDS §5).
    // Weights apply per rarity tier (1 -> W1, 2 -> W2, 3+ -> W3); the synergy flag splits the
    // pool (level offers never roll synergy cards; the chest rolls only them).
    private TArray<URogueUpgradeDef> RollOptions(int Count, int Salt, float W1, float W2, float W3, bool bSynergyOnly)
    {
        TArray<URogueUpgradeDef> Result;
        TArray<URogueUpgradeDef> Available;
        for (URogueUpgradeDef Upgrade : UpgradePool)
        {
            if (Upgrade != nullptr && Upgrade.bSynergyUpgrade == bSynergyOnly)
                Available.Add(Upgrade);
        }

        int BaseSeed = (RunManager != nullptr) ? RunManager.GetStream().GetInitialSeed() : 1;
        FRandomStream Rng(BaseSeed + Salt * 6151);

        int Want = Math::Min(Count, Available.Num());
        for (int i = 0; i < Want; i++)
        {
            float Total = 0.0;
            for (URogueUpgradeDef Upgrade : Available)
                Total += RarityWeight(Upgrade.Rarity, W1, W2, W3);
            if (Total <= 0.0)
                break;

            float Roll = Rng.RandRange(0.0, Total);
            int PickIdx = Available.Num() - 1;
            float Acc = 0.0;
            for (int j = 0; j < Available.Num(); j++)
            {
                Acc += RarityWeight(Available[j].Rarity, W1, W2, W3);
                if (Roll <= Acc)
                {
                    PickIdx = j;
                    break;
                }
            }
            Result.Add(Available[PickIdx]);
            Available.RemoveAt(PickIdx);
        }
        return Result;
    }

    private float RarityWeight(int Rarity, float W1, float W2, float W3) const
    {
        if (Rarity >= 3)
            return W3;
        if (Rarity == 2)
            return W2;
        return W1;
    }
}
