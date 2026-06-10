// ResultsScreenWidget.as
// End-of-run results: big RAID COMPLETE / SQUAD WIPED title, run time + seed, and a per-player
// stat table (DRG/RoR2 pattern: team result first, then everyone's column — damage included,
// the most-missed stat in DRG's end screen). Shown by the HUD a couple of seconds after the
// replicated ERunPhase resolves, so the banner lands first and replication has settled.
//
// CommonUI: an activatable pushed onto the Menu layer stack (input config Menu = cursor on,
// game input off — the run is over). Not a back handler: you leave via its buttons. Built in
// Construct — by then the phase and the replicated ARoguePlayerState stats it renders are known.
class UResultsScreenWidget : UCommonActivatableWidget
{
    default bIsBackHandler = false;

    private UCanvasPanel Root;
    private UButton FirstButton;   // CommonUI focus target (PLAY AGAIN on host, LEAVE on clients)
    private bool bBuilt = false;

    UFUNCTION(BlueprintOverride)
    void Construct()
    {
        BuildLayout();
    }

    UFUNCTION(BlueprintOverride)
    FUIInputConfig GetDesiredInputConfig() const
    {
        FUIInputConfig Config;
        Config.InputMode = ECommonInputMode::Menu;
        Config.MouseCaptureMode = EMouseCaptureMode::NoCapture;
        return Config;
    }

    UFUNCTION(BlueprintOverride)
    UWidget BP_GetDesiredFocusTarget() const
    {
        return FirstButton;
    }

    private void BuildLayout()
    {
        if (bBuilt)
            return;

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        bool bVictory = GS != nullptr && GS.Phase == ERunPhase::Victory;

        Root = Cast<UCanvasPanel>(ConstructWidget(UCanvasPanel::StaticClass()));
        if (Root == nullptr)
            return;
        SetRootWidget(Root);
        bBuilt = true;

        // Heavier dim than the upgrade overlay — the fight is over, focus on the report.
        UBorder Backdrop = RogueUITheme::MakePanel(this, FLinearColor(0.0, 0.0, 0.0), 0.0);
        Backdrop.SetBrushColor(FLinearColor(0.0, 0.0, 0.0, 0.78));
        UCanvasPanelSlot BackdropSlot = Root.AddChildToCanvas(Backdrop);
        BackdropSlot.SetAnchors(FAnchors(0.0, 0.0, 1.0, 1.0));
        BackdropSlot.SetOffsets(FMargin(0.0, 0.0, 0.0, 0.0));

        FLinearColor ResultColor = bVictory ? RogueUITheme::Victory : RogueUITheme::Danger;
        UTextBlock Title = RogueUITheme::MakeText(
            this, bVictory ? "RAID COMPLETE" : "SQUAD WIPED", ResultColor, 3.0);
        UCanvasPanelSlot TitleSlot = Root.AddChildToCanvas(Title);
        TitleSlot.SetAnchors(FAnchors(0.5, 0.16));
        TitleSlot.SetAlignment(FVector2D(0.5, 0.5));
        TitleSlot.SetAutoSize(true);

        UTextBlock Subtitle = RogueUITheme::MakeText(this, BuildSubtitle(GS), RogueUITheme::TextDim, 1.1);
        UCanvasPanelSlot SubSlot = Root.AddChildToCanvas(Subtitle);
        SubSlot.SetAnchors(FAnchors(0.5, 0.25));
        SubSlot.SetAlignment(FVector2D(0.5, 0.5));
        SubSlot.SetAutoSize(true);

        BuildStatsTable(GS);
        BuildButtons();

        // Breadcrumb for the headless smoke path (banner -> panel escalation is timer-driven,
        // so the log line is the cheap proof the panel actually constructed).
        int PlayerCount = (GS != nullptr) ? GS.PlayerArray.Num() : 0;
        Print(f"[Results] screen shown — victory={bVictory} players={PlayerCount}", 5.0);
    }

    private FString BuildSubtitle(ARaidGameState GS)
    {
        if (GS == nullptr)
            return "";
        float End = (GS.RunEndTime > 0.0) ? GS.RunEndTime : Gameplay::GetTimeSeconds();
        int Total = int(Math::Max(0.0, End - GS.RunStartTime));
        int Mins = Total / 60;
        int Secs = Total % 60;
        FString Pad = Secs < 10 ? "0" : "";
        return f"TIME {Mins}:{Pad}{Secs}      SEED {GS.MasterSeed}";
    }

    // One label column + one column per player + a TEAM totals column.
    private void BuildStatsTable(ARaidGameState GS)
    {
        UHorizontalBox Table = Cast<UHorizontalBox>(ConstructWidget(UHorizontalBox::StaticClass()));
        UCanvasPanelSlot TableSlot = Root.AddChildToCanvas(Table);
        TableSlot.SetAnchors(FAnchors(0.5, 0.5));
        TableSlot.SetAlignment(FVector2D(0.5, 0.5));
        TableSlot.SetAutoSize(true);

        TArray<FString> Labels;
        Labels.Add("KILLS");
        Labels.Add("DAMAGE DEALT");
        Labels.Add("DAMAGE TAKEN");
        Labels.Add("DOWNS");
        Labels.Add("REVIVES");
        Labels.Add("UPGRADES");
        AddColumn(Table, "", RogueUITheme::TextDim, Labels, EHorizontalAlignment::HAlign_Right);

        int TotalKills = 0;
        int TotalDealt = 0;
        int TotalTaken = 0;
        int TotalDowns = 0;
        int TotalRevives = 0;
        int TotalUpgrades = 0;

        if (GS != nullptr)
        {
            for (APlayerState BasePS : GS.PlayerArray)
            {
                ARoguePlayerState PS = Cast<ARoguePlayerState>(BasePS);
                if (PS == nullptr)
                    continue;

                TArray<FString> Values;
                Values.Add(f"{PS.Kills}");
                Values.Add(f"{int(PS.DamageDealt)}");
                Values.Add(f"{int(PS.DamageTaken)}");
                Values.Add(f"{PS.TimesDowned}");
                Values.Add(f"{PS.Revives}");
                Values.Add(f"{PS.UpgradesTaken}");
                AddColumn(Table, PS.GetPlayerName(), RogueUITheme::Accent, Values,
                          EHorizontalAlignment::HAlign_Center);

                TotalKills += PS.Kills;
                TotalDealt += int(PS.DamageDealt);
                TotalTaken += int(PS.DamageTaken);
                TotalDowns += PS.TimesDowned;
                TotalRevives += PS.Revives;
                TotalUpgrades += PS.UpgradesTaken;
            }
        }

        TArray<FString> Totals;
        Totals.Add(f"{TotalKills}");
        Totals.Add(f"{TotalDealt}");
        Totals.Add(f"{TotalTaken}");
        Totals.Add(f"{TotalDowns}");
        Totals.Add(f"{TotalRevives}");
        Totals.Add(f"{TotalUpgrades}");
        AddColumn(Table, "TEAM", RogueUITheme::TextPrimary, Totals, EHorizontalAlignment::HAlign_Center);
    }

    private void AddColumn(UHorizontalBox Table, FString Header, FLinearColor HeaderColor,
                           TArray<FString> Values, EHorizontalAlignment Align)
    {
        UVerticalBox Column = Cast<UVerticalBox>(ConstructWidget(UVerticalBox::StaticClass()));

        UTextBlock HeaderText = RogueUITheme::MakeText(this, Header, HeaderColor, 1.2);
        UVerticalBoxSlot HeaderSlot = Column.AddChildToVerticalBox(HeaderText);
        HeaderSlot.SetHorizontalAlignment(Align);
        HeaderSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 14.0));

        for (FString Value : Values)
        {
            UTextBlock ValueText = RogueUITheme::MakeText(this, Value, RogueUITheme::TextPrimary, 1.0);
            UVerticalBoxSlot ValueSlot = Column.AddChildToVerticalBox(ValueText);
            ValueSlot.SetHorizontalAlignment(Align);
            ValueSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 8.0));
        }

        UHorizontalBoxSlot ColSlot = Table.AddChildToHorizontalBox(Column);
        ColSlot.SetPadding(FMargin(22.0, 0.0, 22.0, 0.0));
    }

    private void BuildButtons()
    {
        UHorizontalBox ButtonRow = Cast<UHorizontalBox>(ConstructWidget(UHorizontalBox::StaticClass()));
        UCanvasPanelSlot RowSlot = Root.AddChildToCanvas(ButtonRow);
        RowSlot.SetAnchors(FAnchors(0.5, 0.8));
        RowSlot.SetAlignment(FVector2D(0.5, 0.5));
        RowSlot.SetAutoSize(true);

        APlayerController PC = GetOwningPlayer();
        bool bHost = PC != nullptr && PC.HasAuthority();
        if (bHost)
        {
            UButton PlayAgain = RogueUITheme::MakeTextButton(this, "  PLAY AGAIN  ", RogueUITheme::Accent);
            PlayAgain.OnClicked.AddUFunction(this, n"HandlePlayAgain");
            FirstButton = PlayAgain;
            UHorizontalBoxSlot PlaySlot = ButtonRow.AddChildToHorizontalBox(PlayAgain);
            PlaySlot.SetPadding(FMargin(10.0, 0.0, 10.0, 0.0));

            UButton Lobby = RogueUITheme::MakeTextButton(this, "  RETURN TO LOBBY  ", RogueUITheme::TextPrimary);
            Lobby.OnClicked.AddUFunction(this, n"HandleLobby");
            UHorizontalBoxSlot LobbySlot = ButtonRow.AddChildToHorizontalBox(Lobby);
            LobbySlot.SetPadding(FMargin(10.0, 0.0, 10.0, 0.0));
        }
        else
        {
            UTextBlock Waiting = RogueUITheme::MakeText(
                this, "Waiting for the host...", RogueUITheme::TextDim, 1.1);
            UHorizontalBoxSlot WaitSlot = ButtonRow.AddChildToHorizontalBox(Waiting);
            WaitSlot.SetPadding(FMargin(10.0, 8.0, 10.0, 0.0));

            UButton Leave = RogueUITheme::MakeTextButton(this, "  LEAVE TO MENU  ", RogueUITheme::TextPrimary);
            Leave.OnClicked.AddUFunction(this, n"HandleLeave");
            FirstButton = Leave;
            UHorizontalBoxSlot LeaveSlot = ButtonRow.AddChildToHorizontalBox(Leave);
            LeaveSlot.SetPadding(FMargin(10.0, 0.0, 10.0, 0.0));
        }

        UButton Quit = RogueUITheme::MakeTextButton(this, "  QUIT  ", RogueUITheme::TextDim);
        Quit.OnClicked.AddUFunction(this, n"HandleQuit");
        UHorizontalBoxSlot QuitSlot = ButtonRow.AddChildToHorizontalBox(Quit);
        QuitSlot.SetPadding(FMargin(10.0, 0.0, 10.0, 0.0));
    }

    // Host: whole squad back to hero select (ServerTravel keeps clients connected).
    UFUNCTION()
    private void HandleLobby()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.TravelToLobby();
    }

    // Client: back to the local front-end (disconnects from the host).
    UFUNCTION()
    private void HandleLeave()
    {
        Gameplay::OpenLevel(n"L_MainMenu");
    }

    // Host: reload the level for a fresh run + seed (same path as the RaidRestart console command).
    // MVP caveat: OpenLevel drops remote clients (they rejoin by IP); the lobby flow will switch
    // this to ServerTravel so the party stays connected.
    UFUNCTION()
    private void HandlePlayAgain()
    {
        APlayerController PC = GetOwningPlayer();
        if (PC == nullptr || !PC.HasAuthority())
            return;
        FName Current = FName(Gameplay::GetCurrentLevelName());
        Gameplay::OpenLevel(Current);
    }

    UFUNCTION()
    private void HandleQuit()
    {
        System::QuitGame(nullptr, EQuitPreference::Quit, false);
    }
}
