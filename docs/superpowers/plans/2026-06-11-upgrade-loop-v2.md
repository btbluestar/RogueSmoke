# Upgrade Loop v2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Per-player upgrade hands with stacks/caps/prerequisites, a squad-eligibility synergy framework with new synergy cards, milestone modifier choices, pick-flow polish (reroll, auto-pick, fallbacks, lock-in status), and a rarity/XP pacing pass.

**Architecture:** Everything stays in the existing D-0018 shape: `ARaidGameMode` (server-only, AngelScript) rolls seeded offers and now tracks per-player stack records; picks apply GAS GameplayEffects to the picker's (or squad's) ASC; new card metadata lives on `URogueUpgradeDef` DataAssets authored headlessly via editor python. **No C++ changes.**

**Tech Stack:** UE 5.7 Hazelight AngelScript fork; GAS (AngelscriptGAS); CommonUI runtime-built widgets; editor python commandlets for content.

**Spec:** `docs/superpowers/specs/2026-06-11-upgrade-loop-v2-design.md`

## Hard worker constraints (read before executing anything)

1. **ONE editor.** `as-helper run_code_test`, `Tools\BootLevel.ps1`, `Tools\SmokeTest.ps1`, and python commandlets all contend for one Unreal instance. Execute tasks strictly sequentially; never run two verification commands at once; never dispatch two implementation subagents in parallel.
2. **Verify `.as` changes with `as-helper run_code_test`** (compiles all scripts headlessly, exit 0 = pass). Do NOT launch the editor or run a C++ build for script checks.
3. **Asset saves fail while an interactive editor is open** (share violation). If `mcp__ue-cpp__editor_sessions_list` shows a running editor, `editor_session_end` it before running any python commandlet.
4. **Run script FILES via `Tools\BootLevel.ps1`**, not inline `Start-Process` one-liners (AMSI blocks inline launchers).
5. **Do not touch** `RogueSmoke/Content/Blueprints/GE_Upgrade_ChainDetonation.uasset` (pre-existing local modification, another workstream) and `SUPERPOWERS_HANDOFF.md`. The *DataAsset* `Content/Upgrades/DA_Upgrade_ChainDetonation.uasset` IS ours to edit — only the GE blueprint is off-limits. Never `git add -A`; stage files explicitly.
6. AngelScript gotchas that will bite you: function parameters are `const` (copy to a local to mutate); `Print()` args must not call non-const methods (precompute strings); `FRandomStream` has no `FRand()` — use `RandRange(float64, float64)`; `Gameplay::`/`System::` statics drop the WorldContext param; AS RPCs default to RELIABLE (fine here — offers must arrive).
7. Headless paused-tick caveat: with the game paused under `-nullrhi`, the 30 s watchdog elapses in ~5 s wall-clock. That is expected and the flow-smoke relies on it.

**Paths:** repo root `C:\Users\btblu\Documents\RogueSmoke`; scripts `RogueSmoke\Script\...`; uproject `RogueSmoke\RogueSmoke.uproject`. All `.as` paths below are relative to repo root.

---

### Task 1: Data model — card metadata + per-player stack records

**Files:**
- Modify: `RogueSmoke/Script/Upgrades/RogueUpgradeDef.as`
- Modify: `RogueSmoke/Script/Core/RaidGameMode.as`

- [ ] **Step 1.1: Extend `URogueUpgradeDef`**

Append inside the class body of `RogueSmoke/Script/Upgrades/RogueUpgradeDef.as` (after the `Effect` property):

```angelscript
    // --- Loop v2 (D-0019): stacking, prerequisites, milestone/utility/squad behavior ---

    // How many times ONE player may pick this card. <= 0 means unlimited (utility filler cards).
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade|LoopV2")
    int MaxStacks = 5;

    // Milestone modifier (DRG-overclock style): once eligible and not yet owned, it gets a
    // guaranteed slot in that player's next hand instead of competing in the weighted roll.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade|LoopV2")
    bool bMilestone = false;

    // Apply the Effect to EVERY hero in the squad (team/synergy cards), not just the picker.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade|LoopV2")
    bool bApplyToSquad = false;

    // Prerequisites. bPrereqSelf = true: the PICKING player must hold both (milestone gating).
    // bPrereqSelf = false: A and B must be held by two DIFFERENT players (duo/synergy gating,
    // Hades-style); in a solo squad one player holding both counts, so solo runs still see synergies.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade|Prereqs")
    URogueUpgradeDef PrereqA;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade|Prereqs")
    int PrereqAStacks = 1;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade|Prereqs")
    URogueUpgradeDef PrereqB;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade|Prereqs")
    int PrereqBStacks = 1;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade|Prereqs")
    bool bPrereqSelf = false;
```

- [ ] **Step 1.2: Add stack-record structs + bookkeeping to `RaidGameMode.as`**

At the top of `RogueSmoke/Script/Core/RaidGameMode.as`, before `class ARaidGameMode`:

```angelscript
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
```

Inside `ARaidGameMode`, next to the existing `private int OfferCounter = 0;` block, add:

```angelscript
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
```

- [ ] **Step 1.3: Compile check**

Run `mcp__plugin_ue-as_as-helper__run_code_test` (after `set_project_root` to `C:\Users\btblu\Documents\RogueSmoke\RogueSmoke` if not done this session).
Expected: exit 0, no script errors.

- [ ] **Step 1.4: Commit**

```
git add RogueSmoke/Script/Upgrades/RogueUpgradeDef.as RogueSmoke/Script/Core/RaidGameMode.as
git commit -m "feat(upgrades): card stacks/caps/prereqs data model + per-player records (D-0019)"
```

---

### Task 2: Per-player hands — salted rolls, validated picks, auto-pick, queued offers

**Files:**
- Modify: `RogueSmoke/Script/Core/RaidGameMode.as`
- Modify: `RogueSmoke/Script/Core/RaidGameState.as`
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as` (RPC signature)
- Modify: `RogueSmoke/Script/Player/HeroCharacter.as` (`Server_ApplyUpgrade`)
- Modify: `RogueSmoke/Script/UI/UpgradeSelectWidget.as` (Setup signature — minimal stub here; full UX in Task 4)

- [ ] **Step 2.1: Replicated pick-status + reroll counter on `RaidGameState.as`**

Add after the `XPToNextLevel` property in `RogueSmoke/Script/Core/RaidGameState.as`:

```angelscript
    // --- Loop v2 (D-0019): pick-flow state the card screen renders. Server-written. ---

    // Display names of players who still owe a pick while the raid is pick-paused.
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    TArray<FString> AwaitingPickNames;

    // Squad-shared reroll budget (any player may spend one on their own hand).
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    int SquadRerollsRemaining = 1;
```

- [ ] **Step 2.2: Rewrite the offer/pick/resume core in `RaidGameMode.as`**

Replace the fields `private int PendingPicks = 0;` with queued-offer state (keep `OfferCounter` and `OfferPauseElapsed`):

```angelscript
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
```

Replace `Tick` with:

```angelscript
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
```

Replace `OfferUpgradesWeighted`, `NotifyUpgradePicked`, `ResumeFromOffer`, and `RollOptions` with:

```angelscript
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
```

Add the `UtilityPool` property next to `UpgradePool`:

```angelscript
    // Consolation/filler cards (squad heal, small stat dribbles): pad short hands so a pick
    // screen never shows fewer than OptionsPerOffer cards. Assigned on BP_RaidGamemode (Task 5).
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades")
    TArray<URogueUpgradeDef> UtilityPool;
```

Keep `RarityWeight`, `OfferUpgradesToAll`, `AddTeamXP`, chest functions unchanged (chest still calls `OfferUpgradesWeighted(1.0, 1.0, 1.0, true)`).

- [ ] **Step 2.3: New RPC signatures on `RaidPlayerController.as`**

Replace `Client_OfferUpgrades` and add the two new RPCs (full UX comes in Task 4 — for now `Setup` just gains the stacks param):

```angelscript
    // Server -> owning client: present THIS player's hand (per-player rolls, D-0019).
    // CurrentStacks[i] = how many copies of Options[i] this player already owns (card "Lv n" text).
    UFUNCTION(Client)
    void Client_OfferUpgrades(TArray<URogueUpgradeDef> Options, TArray<int> CurrentStacks)
    {
        if (Layout == nullptr || Options.Num() == 0)
            return;
        if (ActiveUpgradeWidget != nullptr)
        {
            ActiveUpgradeWidget.Refresh(Options, CurrentStacks);   // reroll replaced the hand
            return;
        }
        ActiveUpgradeWidget = Cast<UUpgradeSelectWidget>(
            Layout.PushToLayer(ERogueUILayer::GameMenu, UUpgradeSelectWidget));
        if (ActiveUpgradeWidget == nullptr)
            return;
        ActiveUpgradeWidget.Setup(Options, CurrentStacks);
    }

    // Server -> owning client: watchdog auto-picked for us — drop the screen.
    UFUNCTION(Client)
    void Client_ForceClosePick()
    {
        if (ActiveUpgradeWidget != nullptr)
        {
            ActiveUpgradeWidget.DeactivateWidget();
            ActiveUpgradeWidget = nullptr;
        }
    }

    // Client -> server: spend one squad reroll on my hand (validated server-side).
    UFUNCTION(Server)
    void Server_RequestReroll()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.RequestReroll(this);
    }
```

- [ ] **Step 2.4: Route `Server_ApplyUpgrade` through the GameMode**

In `RogueSmoke/Script/Player/HeroCharacter.as`, replace the body of `Server_ApplyUpgrade` (keep the UFUNCTION and doc comment, update the comment to mention validation):

```angelscript
    // Apply a chosen upgrade server-side. The GameMode validates the card against the player's
    // offered hand (client intent never trusted), applies the GE, and resumes the raid when
    // everyone has picked. D-0010 / D-0019.
    UFUNCTION(Server)
    void Server_ApplyUpgrade(URogueUpgradeDef Upgrade)
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        ARaidPlayerController PC = Cast<ARaidPlayerController>(GetController());
        if (GameMode != nullptr && PC != nullptr)
            GameMode.ApplyUpgradeFor(PC, Upgrade);
    }
```

(The old `NotifyUpgradePicked` no longer exists; `ApplyUpgradeFor` subsumes it — make sure no other call sites remain: `grep -rn "NotifyUpgradePicked" RogueSmoke/Script` must return nothing.)

- [ ] **Step 2.5: Minimal widget signature update (`UpgradeSelectWidget.as`)**

Change `Setup` and add `Refresh` + the stacks array (Task 4 does the visual work; this keeps Task 2 compiling):

```angelscript
    // Stack counts aligned with OfferedUpgrades (how many copies the player already owns).
    UPROPERTY(EditAnywhere, Category = "Upgrades")
    TArray<int> OfferedStacks;

    void Setup(TArray<URogueUpgradeDef> Options, TArray<int> CurrentStacks)
    {
        OfferedUpgrades = Options;
        OfferedStacks = CurrentStacks;
        BuildCards();
    }

    // Reroll path: the server re-rolled our hand while the screen is up — rebuild the cards.
    void Refresh(TArray<URogueUpgradeDef> Options, TArray<int> CurrentStacks)
    {
        OfferedUpgrades = Options;
        OfferedStacks = CurrentStacks;
        if (CardRow != nullptr)
            CardRow.ClearChildren();
        Cards.Empty();
        bCardsBuilt = false;
        BuildCards();
    }
```

- [ ] **Step 2.6: Compile check**

`run_code_test` → expected exit 0.

- [ ] **Step 2.7: Behavior check — headless level-up offer round-trip**

```powershell
Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "RaidGiveXP 200" -Grep "Upgrades"
```

Expected in the log: `[XP] team level ...`, `[Upgrades] raid paused for the pick (1 player(s))`, then (watchdog, fast under headless paused-tick) `[Upgrades] watchdog auto-pick`, `[Upgrades] pick applied: ...`, `[Upgrades] raid resumed (pick timeout)` — the auto-pick is NEW behavior proving Task 2 end-to-end.

- [ ] **Step 2.8: Commit**

```
git add RogueSmoke/Script/Core/RaidGameMode.as RogueSmoke/Script/Core/RaidGameState.as RogueSmoke/Script/Player/RaidPlayerController.as RogueSmoke/Script/Player/HeroCharacter.as RogueSmoke/Script/UI/UpgradeSelectWidget.as
git commit -m "feat(upgrades): per-player hands, validated picks, watchdog auto-pick, squad reroll, queued offers (D-0019)"
```

---

### Task 3: Flow-smoke exec battery (server-side filtering proofs)

**Files:**
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as`
- Modify: `Tools/SmokeTest.ps1`

- [ ] **Step 3.1: Add the `UpgradeFlowSmoke` exec**

Append to `RaidPlayerController.as` (next to the other upgrade execs). It fabricates build states via `AddStack`/`DebugRollFor` and ends with a real offer + watchdog round-trip:

```angelscript
    // --- Debug: Loop-v2 flow battery (D-0019). Asserts: cap filtering, milestone guarantee,
    // synergy duo-gating (solo relaxation), utility padding, reroll spend, watchdog auto-pick.
    // Host-only; polls at boot so it works as -ExecCmds. SmokeTest.ps1 asserts the RESULT line.
    private int FlowSmokeRetries = 0;

    UFUNCTION(Exec)
    void UpgradeFlowSmoke()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        AHeroCharacter Hero = GetHero();
        if (GameMode == nullptr || Hero == nullptr || PlayerState == nullptr)
        {
            if (GameMode != nullptr && FlowSmokeRetries < 30)
            {
                FlowSmokeRetries++;
                System::SetTimer(this, n"UpgradeFlowSmoke", 1.0, false);
                return;
            }
            Print("[FlowSmoke] host only, and needs a hero pawn", 5.0);
            return;
        }

        int Pass = 0;
        int Total = 6;

        // 1) Baseline: a hand has OptionsPerOffer cards (utility padding guarantees it
        //    once the UtilityPool is assigned; before Task 5 content, accept >= 1).
        TArray<URogueUpgradeDef> Hand = GameMode.DebugRollFor(PlayerState, 1, false);
        if (Hand.Num() >= 1)
            Pass++;
        else
            Print("[FlowSmoke] FAIL 1: empty baseline hand", 10.0);

        // 2) Cap filtering: max out the first capped card; it must vanish from 20 salted rolls.
        URogueUpgradeDef Capped;
        for (URogueUpgradeDef Def : GameMode.UpgradePool)
        {
            if (Def != nullptr && !Def.bSynergyUpgrade && !Def.bMilestone && Def.MaxStacks > 0)
            {
                Capped = Def;
                break;
            }
        }
        if (Capped != nullptr)
        {
            for (int i = 0; i < Capped.MaxStacks; i++)
                GameMode.AddStack(PlayerState, Capped);
            bool bLeaked = false;
            for (int s = 0; s < 20; s++)
            {
                TArray<URogueUpgradeDef> H = GameMode.DebugRollFor(PlayerState, 100 + s, false);
                if (H.Contains(Capped))
                    bLeaked = true;
            }
            if (!bLeaked)
                Pass++;
            else
                Print("[FlowSmoke] FAIL 2: capped card still offered", 10.0);
        }
        else
        {
            Pass++;   // no capped cards in pool — vacuously true
            Print("[FlowSmoke] note: no capped card to test", 5.0);
        }

        // 3) Milestone guarantee: find a bMilestone card, satisfy its self-prereqs, assert it
        //    appears in the next hand. Skips (vacuous pass) until Task 5 authors one.
        URogueUpgradeDef Milestone;
        for (URogueUpgradeDef Def : GameMode.UpgradePool)
        {
            if (Def != nullptr && Def.bMilestone && Def.bPrereqSelf && Def.PrereqA != nullptr)
            {
                Milestone = Def;
                break;
            }
        }
        if (Milestone != nullptr)
        {
            int Need = Milestone.PrereqAStacks - GameMode.GetStackCount(PlayerState, Milestone.PrereqA);
            for (int i = 0; i < Need; i++)
                GameMode.AddStack(PlayerState, Milestone.PrereqA);
            if (Milestone.PrereqB != nullptr)
            {
                int NeedB = Milestone.PrereqBStacks - GameMode.GetStackCount(PlayerState, Milestone.PrereqB);
                for (int i = 0; i < NeedB; i++)
                    GameMode.AddStack(PlayerState, Milestone.PrereqB);
            }
            TArray<URogueUpgradeDef> MH = GameMode.DebugRollFor(PlayerState, 7, false);
            if (MH.Contains(Milestone))
                Pass++;
            else
                Print("[FlowSmoke] FAIL 3: eligible milestone not guaranteed a slot", 10.0);
        }
        else
        {
            Pass++;
            Print("[FlowSmoke] note: no milestone card to test (pre-Task-5)", 5.0);
        }

        // 4) Synergy duo gate: an un-met synergy card must NOT roll; after satisfying its
        //    prereqs on this (solo) player, it MUST become rollable.
        URogueUpgradeDef Synergy;
        for (URogueUpgradeDef Def : GameMode.UpgradePool)
        {
            if (Def != nullptr && Def.bSynergyUpgrade && Def.PrereqA != nullptr && !Def.bPrereqSelf)
            {
                Synergy = Def;
                break;
            }
        }
        if (Synergy != nullptr)
        {
            bool bEarlyLeak = false;
            if (GameMode.GetStackCount(PlayerState, Synergy.PrereqA) < Synergy.PrereqAStacks)
            {
                for (int s = 0; s < 20; s++)
                {
                    TArray<URogueUpgradeDef> H = GameMode.DebugRollFor(PlayerState, 200 + s, true);
                    if (H.Contains(Synergy))
                        bEarlyLeak = true;
                }
            }
            int NeedA = Synergy.PrereqAStacks - GameMode.GetStackCount(PlayerState, Synergy.PrereqA);
            for (int i = 0; i < NeedA; i++)
                GameMode.AddStack(PlayerState, Synergy.PrereqA);
            if (Synergy.PrereqB != nullptr)
            {
                int NeedB = Synergy.PrereqBStacks - GameMode.GetStackCount(PlayerState, Synergy.PrereqB);
                for (int i = 0; i < NeedB; i++)
                    GameMode.AddStack(PlayerState, Synergy.PrereqB);
            }
            bool bNowOffered = false;
            for (int s = 0; s < 20; s++)
            {
                TArray<URogueUpgradeDef> H = GameMode.DebugRollFor(PlayerState, 300 + s, true);
                if (H.Contains(Synergy))
                    bNowOffered = true;
            }
            if (!bEarlyLeak && bNowOffered)
                Pass++;
            else
                Print(f"[FlowSmoke] FAIL 4: duo gate (earlyLeak={bEarlyLeak} nowOffered={bNowOffered})", 10.0);
        }
        else
        {
            Pass++;
            Print("[FlowSmoke] note: no gated synergy card to test (pre-Task-5)", 5.0);
        }

        // 5) Determinism: same salt twice = identical hand.
        TArray<URogueUpgradeDef> D1 = GameMode.DebugRollFor(PlayerState, 42, false);
        TArray<URogueUpgradeDef> D2 = GameMode.DebugRollFor(PlayerState, 42, false);
        bool bSame = D1.Num() == D2.Num();
        if (bSame)
        {
            for (int i = 0; i < D1.Num(); i++)
            {
                if (D1[i] != D2[i])
                    bSame = false;
            }
        }
        if (bSame && D1.Num() > 0)
            Pass++;
        else
            Print("[FlowSmoke] FAIL 5: same-salt rolls differ", 10.0);

        // 6) Live round-trip: real offer -> reroll spend -> watchdog auto-pick -> resume.
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        int RerollsBefore = GS != nullptr ? GS.SquadRerollsRemaining : -1;
        GameMode.OfferUpgradesWeighted(70.0, 25.0, 5.0, false);
        Server_RequestReroll();
        int RerollsAfter = GS != nullptr ? GS.SquadRerollsRemaining : -1;
        if (RerollsBefore == 1 && RerollsAfter == 0)
            Pass++;
        else
            Print(f"[FlowSmoke] FAIL 6: reroll {RerollsBefore} -> {RerollsAfter}", 10.0);
        // The watchdog now auto-picks and resumes (fast under headless paused-tick); grep
        // "[Upgrades] raid resumed" separately.

        Print(f"[FlowSmoke] RESULT {Pass}/{Total}", 15.0);
    }
```

- [ ] **Step 3.2: Compile check**

`run_code_test` → exit 0.

- [ ] **Step 3.3: Run it headlessly**

```powershell
Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "UpgradeFlowSmoke" -Grep "FlowSmoke"
```

Expected: `[FlowSmoke] RESULT 6/6` (checks 3 and 4 pass vacuously with notes until Task 5 content lands), and later in the log `[Upgrades] raid resumed (pick timeout)`.

- [ ] **Step 3.4: Wire into the regression gate**

Read `Tools/SmokeTest.ps1`, find the `DL_Upgrades` entry, and add `UpgradeFlowSmoke` to its exec list with the grep assertion `\[FlowSmoke\] RESULT 6/6` (follow the file's existing entry format exactly — same retry/timeout parameters as the `UpgradeSmoke` entry). Then run the full gate:

```powershell
Tools\SmokeTest.ps1
```

Expected: PASS on every level (exit 0).

- [ ] **Step 3.5: Commit**

```
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "test(upgrades): UpgradeFlowSmoke battery - caps, milestones, duo gates, determinism, reroll (D-0019)"
```

---

### Task 4: Card-screen UX — stack levels, duo framing, waiting-on, reroll button

**Files:**
- Modify: `RogueSmoke/Script/UI/UpgradeSelectWidget.as`
- Modify: `RogueSmoke/Script/UI/UpgradeCardWidget.as`

- [ ] **Step 4.1: Card shows stack level + prereq (duo) line**

In `UpgradeCardWidget.as`: add a `StackText` and `PrereqText` member next to the other text blocks:

```angelscript
    private UTextBlock StackText;
    private UTextBlock PrereqText;
```

In `BuildLayout()`, insert StackText right after the `RarityText` block and PrereqText right after the `ValueText` block:

```angelscript
        // "Lv 2 -> 3" line: repeat picks read as deepening a track, not duplicates (Loop v2).
        StackText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim, 0.95);
        UVerticalBoxSlot StackSlot = Column.AddChildToVerticalBox(StackText);
        StackSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        StackSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 8.0));
```

```angelscript
        // Duo/prereq line ("Requires: Incendiary Rounds + Heavy Caliber"): synergy legibility.
        PrereqText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim, 0.85, true);
        UVerticalBoxSlot PrereqSlot = Column.AddChildToVerticalBox(PrereqText);
        PrereqSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        PrereqSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 8.0));
```

Change `Populate` to take the stack count and fill both lines:

```angelscript
    void Populate(URogueUpgradeDef Def, int CurrentStacks, int Index, UUpgradeSelectWidget Screen)
    {
        OwnerScreen = Screen;
        OfferIndex = Index;
        if (!bBuilt || Def == nullptr)
            return;

        FLinearColor Rarity = RogueUITheme::RarityColor(Def.Rarity);
        Frame.SetBrushColor(Rarity);

        if (Def.Icon != nullptr)
        {
            IconImage.SetBrushFromTexture(Def.Icon, false);
            IconImage.SetColorAndOpacity(FLinearColor(1.0, 1.0, 1.0));
        }
        else
        {
            IconImage.SetColorAndOpacity(Rarity * 0.45);
        }

        NameText.SetText(Def.DisplayName);
        FString RarityLabel = Def.bSynergyUpgrade
            ? "SYNERGY" : RogueUITheme::RarityName(Def.Rarity);
        RarityText.SetText(FText::FromString(RarityLabel));
        RarityText.SetColorAndOpacity(FSlateColor(Rarity));

        // Stack line: only when the card is repeatable and this isn't the first copy.
        if (CurrentStacks > 0)
        {
            FString CapPart = Def.MaxStacks > 0 ? f"/{Def.MaxStacks}" : "";
            StackText.SetText(FText::FromString(f"Lv {CurrentStacks} -> {CurrentStacks + 1}{CapPart}"));
        }
        else
        {
            StackText.SetText(FText());
        }

        ValueText.SetText(Def.ValueText);
        DescText.SetText(Def.Description);

        // Duo framing: name what unlocked this card (Hades duo-boon legibility).
        if (Def.PrereqA != nullptr && !Def.bPrereqSelf)
        {
            FString Req = f"Requires: {Def.PrereqA.DisplayName.ToString()}";
            if (Def.PrereqB != nullptr)
                Req += f" + {Def.PrereqB.DisplayName.ToString()}";
            PrereqText.SetText(FText::FromString(Req));
        }
        else
        {
            PrereqText.SetText(FText());
        }

        HotkeyText.SetText(FText::FromString(f"[ {Index + 1} ]"));
    }
```

- [ ] **Step 4.2: Select screen — waiting-on line, reroll button, stacks plumb-through**

In `UpgradeSelectWidget.as`:

Add members:

```angelscript
    private UTextBlock WaitingText;
    private UButton RerollButton;
    private UTextBlock RerollLabel;
```

In `BuildFrame()`, after the `Hint` block, add:

```angelscript
        // Lock-in status: who the squad is waiting on (replicated AwaitingPickNames).
        WaitingText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim, 1.0);
        UCanvasPanelSlot WaitingSlot = Root.AddChildToCanvas(WaitingText);
        WaitingSlot.SetAnchors(FAnchors(0.5, 0.88));
        WaitingSlot.SetAlignment(FVector2D(0.5, 0.5));
        WaitingSlot.SetAutoSize(true);

        // Squad reroll: one shared charge; re-rolls THIS player's hand.
        RerollButton = Cast<UButton>(ConstructWidget(UButton::StaticClass()));
        RerollLabel = RogueUITheme::MakeText(this, "REROLL (R)", RogueUITheme::TextPrimary, 1.1);
        RerollButton.AddChild(RerollLabel);
        RerollButton.OnClicked.AddUFunction(this, n"HandleRerollClicked");
        UCanvasPanelSlot RerollSlot = Root.AddChildToCanvas(RerollButton);
        RerollSlot.SetAnchors(FAnchors(0.5, 0.74));
        RerollSlot.SetAlignment(FVector2D(0.5, 0.5));
        RerollSlot.SetAutoSize(true);
```

In `BuildCards()`, change the `Populate` call to pass the stack count:

```angelscript
            int StackCount = (i < OfferedStacks.Num()) ? OfferedStacks[i] : 0;
            Card.Populate(OfferedUpgrades[i], StackCount, i, this);
```

Add the reroll handler, an `R` hotkey case in `OnKeyDown` (before the `return FEventReply::Unhandled();`), and a `Tick` that refreshes the live state (Slate ticks while the game is paused; `-nullrhi` boots never tick widgets — that's why Task 3 asserts server-side):

```angelscript
    UFUNCTION()
    private void HandleRerollClicked()
    {
        ARaidPlayerController PC = Cast<ARaidPlayerController>(GetOwningPlayer());
        if (PC != nullptr)
            PC.Server_RequestReroll();
    }
```

In `OnKeyDown`, after the number-key checks:

```angelscript
        if (Key == EKeys::R)
        {
            HandleRerollClicked();
            return FEventReply::Handled();
        }
```

And the tick:

```angelscript
    UFUNCTION(BlueprintOverride)
    void Tick(FGeometry MyGeometry, float InDeltaTime)
    {
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS == nullptr)
            return;

        if (WaitingText != nullptr)
        {
            FString Line;
            for (int i = 0; i < GS.AwaitingPickNames.Num(); i++)
            {
                if (i > 0)
                    Line += ", ";
                Line += GS.AwaitingPickNames[i];
            }
            WaitingText.SetText(FText::FromString(
                Line.IsEmpty() ? "" : f"Waiting on: {Line}"));
        }

        if (RerollButton != nullptr)
        {
            bool bCanReroll = GS.SquadRerollsRemaining > 0;
            RerollButton.SetIsEnabled(bCanReroll);
            if (RerollLabel != nullptr)
                RerollLabel.SetText(FText::FromString(
                    bCanReroll ? f"REROLL (R) — {GS.SquadRerollsRemaining} left" : "NO REROLLS LEFT"));
        }
    }
```

- [ ] **Step 4.3: Compile check**

`run_code_test` → exit 0.

- [ ] **Step 4.4: Visual sanity headlessly (logs only) + commit**

```powershell
Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "RaidGiveXP 200" -Grep "Upgrades"
```
Expected: same Task-2.7 sequence still passes (UI changes must not break the offer flow). MCP screenshots can't show UMG — defer visual confirmation to the Task 7 PIE pass.

```
git add RogueSmoke/Script/UI/UpgradeSelectWidget.as RogueSmoke/Script/UI/UpgradeCardWidget.as
git commit -m "feat(ui): upgrade cards show stack level + duo prereqs; waiting-on line + squad reroll button (D-0019)"
```

---

### Task 5: Content — milestone, synergy, and utility cards (editor python)

**Files:**
- Create: `Tools/py_make_loop_v2_content.py`
- Content out: `Content/Blueprints/GameplayEffects/LoopV2/GE_*`, `Content/Upgrades/DA_*` (new), edits to existing `DA_Upgrade_*` (MaxStacks/prereqs), `BP_RaidGamemode` (UtilityPool + pool append)

**Card list** (all GEs = INSTANT + ADD_BASE on existing attributes; template pattern from `py_make_weapon_upgrades.py`):

| Card (DA) | Kind | Rarity | GE modifier(s) | Gating |
|---|---|---|---|---|
| DA_Upgrade_FieldDressing | utility (UtilityPool) | 1 | URogueHealthSet `Health` +50 | bApplyToSquad=true, MaxStacks=0 |
| DA_Upgrade_AdrenalSurge | utility (UtilityPool) | 1 | `MoveSpeed` +0.05 | MaxStacks=0 |
| DA_Upgrade_DrillRounds | milestone | 3 | `PierceCount` +2 | bMilestone, bPrereqSelf, PrereqA=DA_Upgrade_PiercingRounds ×3 |
| DA_Upgrade_HeavyPayload | milestone | 3 | `WeaponDamageBonus` +0.40 | bMilestone, bPrereqSelf, PrereqA=DA_Upgrade_PiercingRounds ×3 |
| DA_Upgrade_StormConductor | milestone | 3 | `ChainCount` +3 | bMilestone, bPrereqSelf, PrereqA=DA_Upgrade_ArcConductor ×3 |
| DA_Upgrade_OverchargedArcs | milestone | 3 | `FireRateBonus` +0.40 | bMilestone, bPrereqSelf, PrereqA=DA_Upgrade_ArcConductor ×3 |
| DA_Synergy_Wildfire | synergy | 3 | `BurnChance` +0.25, `WeaponDamageBonus` +0.10 | bSynergyUpgrade, bApplyToSquad, PrereqA=DA_Upgrade_IncendiaryRounds, PrereqB=DA_Upgrade_HeavyCaliber |
| DA_Synergy_VenomCascade | synergy | 3 | `PoisonChance` +0.25, `ChainCount` +1 | bSynergyUpgrade, bApplyToSquad, PrereqA=DA_Upgrade_Neurotoxin, PrereqB=DA_Upgrade_ArcConductor |
| DA_Synergy_Overwhelm | synergy | 3 | `BarrageRadiusBonus` +150, `ChainCount` +1 | bSynergyUpgrade, bApplyToSquad, PrereqA=DA_Upgrade_WideBarrage, PrereqB=DA_Upgrade_ChainShot* |
| DA_Synergy_IronVanguard | synergy | 2 | URogueHealthSet `MaxHealth` +50, `MaxShield` +25 | bSynergyUpgrade, bApplyToSquad, PrereqA=DA_Upgrade_Vitality, PrereqB=DA_Upgrade_Overshield |

*If `DA_Upgrade_ChainShot` doesn't exist as a separate DA (the chain DA is `DA_Upgrade_ArcConductor`), use `DA_Upgrade_ArcConductor` for Overwhelm's PrereqB.

Existing-pool retunes in the same script: `DA_Upgrade_PiercingRounds` MaxStacks=3, `DA_Upgrade_ArcConductor` MaxStacks=3, `DA_Upgrade_IncendiaryRounds` MaxStacks=2, `DA_Upgrade_Neurotoxin` MaxStacks=2, `DA_Upgrade_ChainDetonation` gets PrereqA=`DA_Upgrade_WideBarrage` PrereqB=`DA_Upgrade_Power` (duo, squad-scope), `bApplyToSquad`=true. **Do NOT load/save `GE_Upgrade_ChainDetonation`** (workstream rule) — only its DA.

- [ ] **Step 5.1: Write `Tools/py_make_loop_v2_content.py`**

Follow `Tools/py_make_weapon_upgrades.py` exactly (template-GE export_text/import_text for modifiers; idempotent re-runs; CDO edit → `compile_blueprint` → `save_asset` for the GameMode BP). Differences from that script:
- Multi-modifier GEs (the synergy cards): build a `modifiers` list of two imported `GameplayModifierInfo`s. Health-set attributes use `GE_Upgrade_MaxHealth_T1` (`/Game/Blueprints/GameplayEffects/Tier_1/GE_Upgrade_MaxHealth_T1`) as the export template (replace the `MaxHealth` token); combat-set attributes keep `GE_Upgrade_MoveSpeed_T1`.
- New DA fields are plain types: `da.set_editor_property('MaxStacks', 3)`, `('bMilestone', True)`, `('bApplyToSquad', True)`, `('bPrereqSelf', True)`, `('PrereqA', other_da_object)`, `('PrereqAStacks', 3)`, `('bSynergyUpgrade', True)`.
- GE assets go to `/Game/Blueprints/GameplayEffects/LoopV2/`; new DAs to `/Game/Upgrades/`.
- Append milestone+synergy DAs to `BP_RaidGamemode.UpgradePool`; set the two utility DAs as `UtilityPool` (a separate `set_editor_property('UtilityPool', [...])` on the same CDO).
- Log `L2C-DONE ge=<n> da=<n> retuned=<n> pool=<n> utility=<n>` as the success marker; `raise` on any failure.

- [ ] **Step 5.2: Close any interactive editor, then run the commandlet**

Check `mcp__ue-cpp__editor_sessions_list`; if an editor is up, `editor_session_end` it. Then:

```powershell
& "F:\UEAS\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\RogueSmoke.uproject" -run=pythonscript -script="C:/Users/btblu/Documents/RogueSmoke/Tools/py_make_loop_v2_content.py" -stdout -unattended -abslog="C:/Users/btblu/Documents/RogueSmoke/Tools/loop_v2_content.log"
```

Expected: `L2C-DONE` in `Tools/loop_v2_content.log`, no `-ERR`/`-FATAL` lines. (Commandlet `print` is lost — only `unreal.log` lines land; check the log file, not stdout.)

- [ ] **Step 5.3: Verify the content through the real loop**

```powershell
Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "ListUpgrades" -Grep "Upgrades"
Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "UpgradeSmoke" -Grep "UpgradeSmoke"
Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "UpgradeFlowSmoke" -Grep "FlowSmoke"
```

Expected: `ListUpgrades` shows the new cards; `[UpgradeSmoke] RESULT n/n` with n grown by the new pool cards (every new GE moves at least one attribute); `[FlowSmoke] RESULT 6/6` — and checks 3 and 4 now run for REAL (milestone + synergy cards exist; no more vacuous-pass notes in the log).

- [ ] **Step 5.4: Commit (content + script; stage explicitly, never `-A`)**

```
git add Tools/py_make_loop_v2_content.py "RogueSmoke/Content/Blueprints/GameplayEffects/LoopV2" "RogueSmoke/Content/Upgrades" "RogueSmoke/Content/Blueprints/BP_RaidGamemode.uasset"
git status   # confirm GE_Upgrade_ChainDetonation.uasset and SUPERPOWERS_HANDOFF.md are NOT staged
git commit -m "feat(content): milestone/synergy/utility cards + caps and duo prereqs on existing pool (D-0019)"
```

---

### Task 6: Pacing & rarity — floors+caps weights, front-loaded XP curve

**Files:**
- Modify: `RogueSmoke/Script/Core/RaidGameMode.as`
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as` (RaidXPReport exec)
- Possibly modify: `Tools/SmokeTest.ps1` (if any grep depended on old curve numbers)

- [ ] **Step 6.1: Replace the spike weights with floors+caps in `AddTeamXP`**

In `RaidGameMode.as`, replace the weight block inside `AddTeamXP` (the `float W1 = 70.0; ...` through the `Print`) with:

```angelscript
        float W1 = 0.0; float W2 = 0.0; float W3 = 0.0;
        ComputeRarityWeights(GS.TeamLevel, W1, W2, W3);
        Print(f"[XP] team level {GS.TeamLevel} — offer weights {W1}/{W2}/{W3}", 5.0);
```

And add the method (Brotato model — hard floors, capped ceilings, smooth ramp between):

```angelscript
    // Rarity pacing (D-0019, Brotato floors+caps): moderate (r2) unlocks at team level 3 and
    // ramps to a 60-weight cap; rare (r3) unlocks at level 6 and ramps to a 25-weight cap.
    // Commons never vanish — they're the substrate milestones and synergies are built from.
    private void ComputeRarityWeights(int Level, float& W1, float& W2, float& W3) const
    {
        W2 = (Level >= 3) ? Math::Min(20.0 + 4.0 * float(Level - 3), 60.0) : 0.0;
        W3 = (Level >= 6) ? Math::Min(5.0 + 2.0 * float(Level - 6), 25.0) : 0.0;
        W1 = Math::Max(100.0 - W2 - W3, 10.0);
    }
```

- [ ] **Step 6.2: Front-load the XP curve**

Change the two defaults in `RaidGameMode.as`:

```angelscript
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|XP")
    float XPBasePerLevel = 50.0;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrades|XP")
    float XPGrowthPerLevel = 35.0;
```

(Targets ~8–12 levels per ~15-min raid at current kill rates: cumulative XP to level 12 = 50×11 + 35×(0+1+…+10) = 550 + 1925 = 2475; fodder 5 / elites 25 / boss 150 per D-0018. First three levels cost 50/85/120 — reachable in the opening waves. **Check whether `BP_RaidGamemode` overrides these two properties** — if the BP CDO has its own values, update them via python or note it; class defaults only apply where the BP didn't override.)

Also update `ARaidGameState.XPToNextLevel`'s initial value to match:

```angelscript
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Run")
    float XPToNextLevel = 50.0;
```

- [ ] **Step 6.3: `RaidXPReport` exec**

Append to `RaidPlayerController.as`:

```angelscript
    // --- Debug: print the XP curve table + live team state (pacing tuning, D-0019). ---
    UFUNCTION(Exec)
    void RaidXPReport()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GameMode == nullptr || GS == nullptr)
        {
            Print("[XPReport] host only", 4.0);
            return;
        }
        float Cumulative = 0.0;
        for (int Lv = 1; Lv <= 12; Lv++)
        {
            float Step = GameMode.XPBasePerLevel + GameMode.XPGrowthPerLevel * float(Lv - 1);
            Cumulative += Step;
            Print(f"[XPReport] L{Lv}->L{Lv + 1}: {Step} (cumulative {Cumulative})", 12.0);
        }
        Print(f"[XPReport] live: level {GS.TeamLevel}, {GS.TeamXP}/{GS.XPToNextLevel}", 12.0);
    }
```

Note: `XPBasePerLevel`/`XPGrowthPerLevel` are `UPROPERTY(EditDefaultsOnly)` — readable cross-class within script. If the compiler rejects the access, change both to `UPROPERTY(EditDefaultsOnly, BlueprintReadOnly)` in `RaidGameMode.as`.

- [ ] **Step 6.4: Compile + behavior check**

`run_code_test` → exit 0. Then:

```powershell
Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "RaidXPReport" -Grep "XPReport"
Tools\BootLevel.ps1 -Map /Game/Levels/DebuggingLevels/DL_Upgrades -Exec "RaidGiveXP 135" -Grep "XP"
```

Expected: the curve table; and `RaidGiveXP 135` crosses exactly levels 2 AND 3 (50+85=135) with ONE offer — log shows `team level 3` and one `raid paused`. Then check `Tools/SmokeTest.ps1` for any grep that assumed `RaidGiveXP 100` = exactly one level (base was 100) and update that entry's exec amount/greps to the new curve. Run `Tools\SmokeTest.ps1` → all PASS.

- [ ] **Step 6.5: Commit**

```
git add RogueSmoke/Script/Core/RaidGameMode.as RogueSmoke/Script/Core/RaidGameState.as RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "feat(upgrades): rarity floors+caps by team level; front-loaded XP curve; RaidXPReport (D-0019)"
```

---

### Task 7: Final verification + docs (D-0019)

**Files:**
- Modify: `DECISIONS.md`, `GLOSSARY.md`, `startup.md`

- [ ] **Step 7.1: Full regression gate**

```powershell
Tools\SmokeTest.ps1
```
Expected: PASS table, exit 0. Fix anything that fails before proceeding.

- [ ] **Step 7.2: 2-player listen-server PIE check (the one thing headless can't prove)**

Start the editor (`mcp__ue-cpp__editor_session_start`), then via `unreal-test-mcp`: load `/Game/Levels/DebuggingLevels/DL_Upgrades`, start PIE with 2 players / listen server if the MCP supports the parameters (otherwise report to the user that this step needs a manual PIE run and list what to check). Verify, via logs + `console_exec RaidGiveXP 200` on the host:
- Both players' screens open; the two hands are DIFFERENT (per-player salt) — grep each client's `[Upgrades]` lines or inspect `AwaitingPickNames` shrinking one name at a time as each picks.
- Reroll on one client decrements the replicated `SquadRerollsRemaining` for both.
- After both pick: `raid resumed (all players picked)`.
- Chest path: `RaidKillElites` (or scripted Brood-mother kill) → `RaidGoToChest` → synergy offer happens (with prereqs unmet it should offer utility padding only — confirm no ineligible synergy card shows).
Remember: PIE under MCP free-runs in real time; assert end-state, not frame counts.

- [ ] **Step 7.3: Write D-0019 in `DECISIONS.md`**

Append after D-0018, following the existing entry format:

```markdown
### D-0019 — Upgrade loop v2: per-player hands, stacks/prereqs, squad eligibility

- **Status:** Decided — spec `docs/superpowers/specs/2026-06-11-upgrade-loop-v2-design.md`
  (research: LoL Swarm primary; VS / RoR2 / Brotato / Hades survey). Builds on D-0018.
- **Decision:** (1) **Per-player hands** — each player's 3 cards roll from a fresh stream salted
  by offer index AND PlayerArray index; picks validated against the offered hand server-side.
  (2) **Stacks/caps/prereqs on `URogueUpgradeDef`** — `MaxStacks` (≤0 = unlimited), self-scope
  prereqs gate **milestone** modifier cards (guaranteed hand slots once eligible), squad-scope
  duo prereqs (A and B on two different players; relaxed to one player solo) gate **synergy**
  cards. (3) **Pick-flow**: watchdog now AUTO-PICKS card 0 (offer honored, not lost); 1
  squad-shared reroll per raid; short hands pad from a `UtilityPool` (squad heal / filler);
  offers requested mid-pick queue (one deep). (4) **Rarity floors+caps** replace the %5/%10
  spikes: r2 from team level 3 (cap 60), r3 from level 6 (cap 25), commons floored at 10.
  XP curve front-loaded (base 50, growth 35; ~8–12 levels per raid).
- **Mechanics:** all per-player state is server-only `FPlayerUpgradeRecord`s on the GameMode;
  the UI gets stack counts in the offer RPC payload; `AwaitingPickNames`/`SquadRerollsRemaining`
  replicate on `ARaidGameState`. `bApplyToSquad` applies a card's GE to every hero's ASC
  (synergy/team cards). Verification: `UpgradeFlowSmoke` exec battery in SmokeTest.ps1.
- **Consequences:** synergy pool now has 5 duo-gated cards; milestone pairs exist for the
  pierce and chain tracks; full Swarm-style behavior evolutions (new weapon mechanics) remain
  future work; balance numbers are first-pass.
```

- [ ] **Step 7.4: `GLOSSARY.md` additions**

Add under "Roguelike systems":

```markdown
- **Stack** — one owned copy of an upgrade card; repeat picks deepen it ("Lv 2 → 3") up to the
  card's `MaxStacks` cap (≤0 = unlimited, used by utility filler).
- **Milestone upgrade** — a modifier card unlocked by stacking a track to its prerequisite count
  (e.g. 3× Piercing Rounds); once eligible it's guaranteed a slot in that player's next hand.
- **Eligibility** — whether a card may be offered: under its stack cap AND prerequisites met.
  Synergy cards use squad-scope (duo) eligibility: prereq A and B held by two different players
  (one player may hold both when playing solo).
- **Utility card** — consolation filler (squad heal, small stat dribble) padding a hand that
  eligibility filtering left short; a pick screen never shows a dead/short hand.
- **Squad reroll** — one shared charge per raid; any player may spend it to re-roll their own hand.
```

- [ ] **Step 7.5: Refresh `startup.md` §5 thin spots**

Update the "Known thin spots" bullets: synergy pool now 5 cards (remove "only one synergy card exists"); add the new execs (`UpgradeFlowSmoke`, `RaidXPReport`) to the §3 console-exec list; note per-player hands + auto-pick + reroll in the D-0018/0019 description of the upgrade loop bullet.

- [ ] **Step 7.6: Final commit**

```
git add DECISIONS.md GLOSSARY.md startup.md
git commit -m "docs: D-0019 upgrade loop v2 - per-player hands, stacks/prereqs, squad eligibility"
```

---

## Self-review notes (done at plan time)

- **Spec coverage:** A (per-player hands 2.2, lock-in 2.1+4.2, auto-pick 2.2, fallback 2.2 RollOptionsFor padding + 5.x utility cards, reroll 2.2/2.3/4.2) / B (squad eligibility 1.2 IsEligible, chest unchanged-but-filtered via RollOptionsFor's eligibility, 5.x synergy cards, duo framing 4.1) / C (MaxStacks+records Task 1, "Lv n→n+1" 4.1, milestones 1.1+2.2+5.x) / D (Task 6). Spec's "milestone choice of two" is realized as two competing milestone cards per track both eligible at the same stack — they occupy the two guaranteed slots, so the player chooses between them on the level-up screen.
- **Known judgment calls an executor must NOT "fix":** picks while paused rely on the existing D-0018 pause plumbing (RPCs work while paused); `AwaitingPickNames` as FStrings is deliberate (display-only); records key on `APlayerState` so they survive pawn respawn.
- **Type consistency check:** `Client_OfferUpgrades(TArray<URogueUpgradeDef>, TArray<int>)` matches Setup/Refresh; `ApplyUpgradeFor(ARaidPlayerController, URogueUpgradeDef)` called from HeroCharacter and AutoPickRemaining; `DebugRollFor(APlayerState, int, bool)` used by flow-smoke. `Populate(Def, CurrentStacks, Index, Screen)` matches the BuildCards call.
- **Risk register:** AS struct-in-array mutation semantics (`PlayerRecords[i].X = ...`) — if the compiler complains, copy-modify-assign the record; `float&` out-params in `ComputeRarityWeights` — if rejected, return an `FVector` of weights instead; python setting AS-defined properties by name (`MaxStacks` etc.) is the same mechanism `py_make_weapon_upgrades.py` already uses for `Rarity`/`Effect`.
