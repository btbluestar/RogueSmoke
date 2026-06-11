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

    // Pick-pause watchdog: if someone never picks (disconnect / AFK), auto-apply their hand's
    // first card and resume — the offer is honored, not lost.
    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (IsOfferPending())
        {
            OfferPauseElapsed += DeltaSeconds;
            if (OfferPauseElapsed >= OfferPauseTimeoutSeconds)
                AutoPickRemaining();
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

    // Consolation/filler cards (squad heal, small stat dribbles): pad short hands so a pick
    // screen never shows fewer than OptionsPerOffer cards. Assigned on BP_RaidGamemode (Task 5).
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades")
    TArray<URogueUpgradeDef> UtilityPool;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrades")
    int OptionsPerOffer = 3;

    // --- Shared team XP (UpgradeLoop concept). Curve: level N -> N+1 needs
    // XPBasePerLevel + XPGrowthPerLevel * (N - 1). Kills feed AddTeamXP via HandleEnemyKilled. ---
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|XP")
    float XPBasePerLevel = 50.0;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|XP")
    float XPGrowthPerLevel = 35.0;

    // --- v3 death-path evolutions (D-0020): Toxic Burst cloud + Iron Bulwark shield. ---
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|Evolutions")
    float PoisonBurstRadius = 350.0;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|Evolutions")
    float PoisonBurstDuration = 6.0;

    // Resume safety: if someone never picks (disconnect / AFK), unpause anyway after this long.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades")
    float OfferPauseTimeoutSeconds = 30.0;

    private int OfferCounter = 0;
    private float OfferPauseElapsed = 0.0;
    private int RerollNonce = 0;

    // The weights of the live offer (a reroll re-rolls with the same bias).
    private float LastOfferW1 = 70.0;
    private float LastOfferW2 = 25.0;
    private float LastOfferW3 = 5.0;
    private bool bLastOfferSynergy = false;

    // One queued follow-up offer (e.g. the chest opened while a level pick was on screen).
    private bool bQueuedOffer = false;
    private float QueuedW1 = 0.0;
    private float QueuedW2 = 0.0;
    private float QueuedW3 = 0.0;
    private bool bQueuedSynergy = false;

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

        // Hero-gated cards (taunt/barrage tracks) never appear in the wrong hero's hand.
        if (Def.RequiredHeroClass.Get() != nullptr)
        {
            APawn HeroPawn = ForPlayer != nullptr ? ForPlayer.GetPawn() : nullptr;
            if (HeroPawn == nullptr || !HeroPawn.IsA(Def.RequiredHeroClass.Get()))
                return false;
        }

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

        // v3 behavior evolutions read the corpse's pre-recycle state — OnEnemyKilled fires
        // before Deactivate/ResetHealth (SpawnDirector::HandleEliteDeath), so DoT/Clustered
        // flags are still live here.
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr)
        {
            float BurstDps = GetSquadAttribute(n"PoisonBurstDps");
            if (BurstDps > 0.0 && Enemy.Health != nullptr
                && Enemy.Health.HasActiveDot(ERogueDotType::Poison))
            {
                int Spread = Combat.ApplyDotInSphere(Enemy.GetActorLocation(), PoisonBurstRadius,
                    ERogueDotType::Poison, BurstDps, PoisonBurstDuration, nullptr);
                if (Spread > 0)
                    Print(f"[Evo] toxic burst -> {Spread} enemies", 2.0);
            }

            float ShieldAmount = GetSquadAttribute(n"ClusterKillShieldAmount");
            if (ShieldAmount > 0.0 && Enemy.IsClustered())
                Combat.GrantShieldToSquad(ShieldAmount);
        }

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
        // the bias comes from the level REACHED. Rarity floors+caps per D-0019.
        float W1 = 0.0; float W2 = 0.0; float W3 = 0.0;
        ComputeRarityWeights(GS.TeamLevel, W1, W2, W3);
        Print(f"[XP] team level {GS.TeamLevel} — offer weights {W1}/{W2}/{W3}", 5.0);
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

    // The real offer path: per-player weighted rolls (each player gets THEIR hand — Swarm's
    // model), then PAUSE the raid until every player has picked (or the watchdog auto-picks).
    void OfferUpgradesWeighted(float W1, float W2, float W3, bool bSynergyOnly)
    {
        if (!HasAuthority() || UpgradePool.Num() == 0)
            return;
        if (IsOfferPending())
        {
            // A pick is already on screen — queue ONE follow-up offer, fired on resume.
            bQueuedOffer = true;
            QueuedW1 = W1; QueuedW2 = W2; QueuedW3 = W3; bQueuedSynergy = bSynergyOnly;
            return;
        }

        int OfferSalt = OfferCounter;
        OfferCounter += 1;
        LastOfferW1 = W1; LastOfferW2 = W2; LastOfferW3 = W3;
        bLastOfferSynergy = bSynergyOnly;

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS != nullptr)
            GS.AwaitingPickNames.Empty();

        int Sent = 0;
        TArray<ARaidPlayerController> PCs;
        GetAllActorsOfClass(PCs);
        for (ARaidPlayerController PC : PCs)
        {
            if (PC == nullptr || PC.PlayerState == nullptr)
                continue;
            TArray<URogueUpgradeDef> Options = RollOptionsFor(PC.PlayerState, OptionsPerOffer,
                OfferSalt, W1, W2, W3, bSynergyOnly);
            if (Options.Num() == 0)
                continue;
            int RecIdx = FindOrAddRecord(PC);
            PlayerRecords[RecIdx].PendingHand = Options;
            TArray<int> Stacks;
            for (URogueUpgradeDef Opt : Options)
                Stacks.Add(GetStackCount(PC.PlayerState, Opt));
            PC.Client_OfferUpgrades(Options, Stacks);
            if (GS != nullptr)
                GS.AwaitingPickNames.Add(PC.PlayerState.GetPlayerName());
            Sent += 1;
        }

        if (Sent > 0)
        {
            OfferPauseElapsed = 0.0;
            RogueGame::SetRaidPaused(true);   // URogueGameStatics — 'Statics' suffix stripped
            Print(f"[Upgrades] raid paused for the pick ({Sent} player(s))", 4.0);
        }
    }

    // The ONE authoritative apply path (player pick AND timeout auto-pick). Validates the card
    // was actually offered to this player (client intent never trusted), applies the GE (squad-
    // wide if flagged), records the stack, and resumes once nobody owes a pick.
    void ApplyUpgradeFor(ARaidPlayerController PC, URogueUpgradeDef Upgrade)
    {
        if (!HasAuthority() || PC == nullptr || Upgrade == nullptr)
            return;
        int RecIdx = FindOrAddRecord(PC);
        if (!PlayerRecords[RecIdx].PendingHand.Contains(Upgrade))
            return;   // not offered to this player this round — reject

        if (Upgrade.Effect.Get() != nullptr)
        {
            if (Upgrade.bApplyToSquad)
            {
                TArray<AHeroCharacter> Heroes;
                GetAllActorsOfClass(Heroes);
                for (AHeroCharacter Hero : Heroes)
                {
                    UAngelscriptAbilitySystemComponent ASC =
                        Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
                    if (ASC != nullptr)
                        ASC.ApplyGameplayEffectToTarget(Upgrade.Effect, ASC, 1.0, FGameplayEffectContextHandle());
                }
            }
            else
            {
                AHeroCharacter Hero = Cast<AHeroCharacter>(PC.GetControlledPawn());
                UAngelscriptAbilitySystemComponent ASC =
                    Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
                if (ASC != nullptr)
                    ASC.ApplyGameplayEffectToTarget(Upgrade.Effect, ASC, 1.0, FGameplayEffectContextHandle());
            }
        }

        AddStack(PC.PlayerState, Upgrade);
        PlayerRecords[RecIdx].PendingHand.Empty();

        ARoguePlayerState RPS = Cast<ARoguePlayerState>(PC.PlayerState);
        if (RPS != nullptr)
            RPS.AddUpgradeTaken();

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS != nullptr && PC.PlayerState != nullptr)
            GS.AwaitingPickNames.Remove(PC.PlayerState.GetPlayerName());

        FString PickedName = Upgrade.DisplayName.ToString();
        Print(f"[Upgrades] pick applied: {PickedName}", 4.0);

        if (!IsOfferPending())
            ResumeFromOffer("all players picked");
    }

    // Watchdog path: honor every outstanding hand with its first card, closing remote screens.
    private void AutoPickRemaining()
    {
        for (int i = 0; i < PlayerRecords.Num(); i++)
        {
            if (PlayerRecords[i].PendingHand.Num() == 0)
                continue;
            ARaidPlayerController PC = PlayerRecords[i].PC;
            URogueUpgradeDef First = PlayerRecords[i].PendingHand[0];
            Print("[Upgrades] watchdog auto-pick", 4.0);
            if (PC != nullptr)
            {
                PC.Client_ForceClosePick();
                ApplyUpgradeFor(PC, First);   // resumes after the last outstanding pick
            }
            else
            {
                PlayerRecords[i].PendingHand.Empty();
            }
        }
        if (!IsOfferPending())
            ResumeFromOffer("pick timeout");
    }

    private void ResumeFromOffer(FString Why)
    {
        for (int i = 0; i < PlayerRecords.Num(); i++)
            PlayerRecords[i].PendingHand.Empty();
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS != nullptr)
            GS.AwaitingPickNames.Empty();
        RogueGame::SetRaidPaused(false);
        Print(f"[Upgrades] raid resumed ({Why})", 4.0);

        if (bQueuedOffer)
        {
            bQueuedOffer = false;
            OfferUpgradesWeighted(QueuedW1, QueuedW2, QueuedW3, bQueuedSynergy);
        }
    }

    // Squad reroll: any awaiting player may spend one squad charge to re-roll THEIR hand.
    void RequestReroll(ARaidPlayerController PC)
    {
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (!HasAuthority() || PC == nullptr || PC.PlayerState == nullptr
            || GS == nullptr || GS.SquadRerollsRemaining <= 0)
            return;
        int RecIdx = FindOrAddRecord(PC);
        if (PlayerRecords[RecIdx].PendingHand.Num() == 0)
            return;   // no live offer for this player

        GS.SquadRerollsRemaining -= 1;
        RerollNonce += 1;
        TArray<URogueUpgradeDef> Options = RollOptionsFor(PC.PlayerState, OptionsPerOffer,
            OfferCounter * 977 + RerollNonce, LastOfferW1, LastOfferW2, LastOfferW3, bLastOfferSynergy);
        if (Options.Num() == 0)
            return;
        PlayerRecords[RecIdx].PendingHand = Options;
        TArray<int> Stacks;
        for (URogueUpgradeDef Opt : Options)
            Stacks.Add(GetStackCount(PC.PlayerState, Opt));
        PC.Client_OfferUpgrades(Options, Stacks);
        Print(f"[Upgrades] reroll spent ({GS.SquadRerollsRemaining} left)", 4.0);
    }

    private bool IsOfferPending() const
    {
        for (int i = 0; i < PlayerRecords.Num(); i++)
        {
            if (PlayerRecords[i].PendingHand.Num() > 0)
                return true;
        }
        return false;
    }

    // Stable per-player salt: index in the server's PlayerArray (join order). Only ever read
    // server-side, so replication order can't perturb it (CODING_STANDARDS §5).
    private int GetPlayerSalt(APlayerState PS) const
    {
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS == nullptr)
            return 0;
        for (int i = 0; i < GS.PlayerArray.Num(); i++)
        {
            if (GS.PlayerArray[i] == PS)
                return i;
        }
        return 0;
    }

    // Highest value across hero ASCs. Evolution GEs are bApplyToSquad, so any living hero
    // carries the value; max() behaves sanely if a hero spawned after the pick. ~2 heroes,
    // so the GetAllActorsOfClass per kill is cheap; cache if fodder rates ever make it hot.
    private float GetSquadAttribute(FName AttrName) const
    {
        float Best = 0.0;
        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        for (AHeroCharacter Hero : Heroes)
        {
            if (Hero == nullptr)
                continue;
            UAngelscriptAbilitySystemComponent ASC = Hero.GetRogueAbilitySystem();
            if (ASC != nullptr)
                Best = Math::Max(Best, ASC.GetAttributeCurrentValue(URogueCombatSet, AttrName, 0.0));
        }
        return Best;
    }

    // Per-player roll: guaranteed milestone slots, then the weighted roll over eligible cards,
    // then utility padding so a hand is never short (Swarm's no-dead-screen rule). Fresh stream
    // salted by offer index AND player index — deterministic per seed, distinct per player.
    private TArray<URogueUpgradeDef> RollOptionsFor(APlayerState ForPlayer, int Count, int Salt,
                                                    float W1, float W2, float W3, bool bSynergyOnly)
    {
        TArray<URogueUpgradeDef> Result;

        if (!bSynergyOnly)
        {
            for (URogueUpgradeDef Upgrade : UpgradePool)
            {
                if (Result.Num() >= 2)
                    break;   // at most 2 guaranteed milestone slots — the hand keeps a rolled card
                if (Upgrade == nullptr || !Upgrade.bMilestone || Upgrade.bSynergyUpgrade)
                    continue;
                if (GetStackCount(ForPlayer, Upgrade) > 0)
                    continue;
                if (IsEligible(Upgrade, ForPlayer))
                    Result.Add(Upgrade);
            }
        }

        TArray<URogueUpgradeDef> Available;
        for (URogueUpgradeDef Upgrade : UpgradePool)
        {
            if (Upgrade == nullptr || Upgrade.bSynergyUpgrade != bSynergyOnly)
                continue;
            if (Upgrade.bMilestone && !bSynergyOnly)
                continue;   // milestones never compete in the weighted roll
            if (!IsEligible(Upgrade, ForPlayer))
                continue;
            if (Result.Contains(Upgrade))
                continue;
            Available.Add(Upgrade);
        }

        int BaseSeed = (RunManager != nullptr) ? RunManager.GetStream().GetInitialSeed() : 1;
        FRandomStream Rng(BaseSeed + Salt * 6151 + GetPlayerSalt(ForPlayer) * 389);

        while (Result.Num() < Count && Available.Num() > 0)
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

        // Utility padding (UtilityPool assigned on BP_RaidGamemode in Task 5).
        for (URogueUpgradeDef Util : UtilityPool)
        {
            if (Result.Num() >= Count)
                break;
            if (Util != nullptr && !Result.Contains(Util) && IsEligible(Util, ForPlayer))
                Result.Add(Util);
        }
        return Result;
    }

    // Exec/test hook: roll a hand for a player with the standard weights, explicit salt.
    TArray<URogueUpgradeDef> DebugRollFor(APlayerState ForPlayer, int Salt, bool bSynergyOnly)
    {
        return RollOptionsFor(ForPlayer, OptionsPerOffer, Salt, 70.0, 25.0, 5.0, bSynergyOnly);
    }

    // Rarity pacing (D-0019, Brotato floors+caps): moderate (r2) unlocks at team level 3 and
    // ramps to a 60-weight cap; rare (r3) unlocks at level 6 and ramps to a 25-weight cap.
    // Commons never vanish — they're the substrate milestones and synergies are built from.
    private void ComputeRarityWeights(int Level, float& W1, float& W2, float& W3) const
    {
        W2 = (Level >= 3) ? Math::Min(20.0 + 4.0 * float(Level - 3), 60.0) : 0.0;
        W3 = (Level >= 6) ? Math::Min(5.0 + 2.0 * float(Level - 6), 25.0) : 0.0;
        W1 = Math::Max(100.0 - W2 - W3, 10.0);
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
