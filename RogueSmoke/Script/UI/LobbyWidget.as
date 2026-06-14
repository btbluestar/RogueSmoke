// LobbyWidget.as
// The pre-run lobby IS the hero select (RoR2 pattern): a row of hero tiles (pick = your ready
// signal's first half; duplicates blocked server-side), a squad panel showing everyone's pick
// + ready state, a READY toggle, and the host's START RAID button (enabled only when the
// whole squad is ready) which kicks the shared launch countdown.
//
// Runtime-built tree (RogueHUDWidget pattern). All state it renders is replicated
// (ARoguePlayerState picks/ready, ARaidGameState.RaidLaunchAt), so host and clients see the
// same lobby. Shown by ALobbyPlayerController on L_Lobby.

class ULobbyWidget : UCommonActivatableWidget
{
    default bAutoActivate = true;

    private UCanvasPanel Root;
    private UHorizontalBox TileRow;
    private TArray<ULobbyHeroTile> Tiles;
    private UVerticalBox SquadList;
    private UButton ReadyButton;
    private UTextBlock ReadyLabel;
    private UButton StartButton;
    private UTextBlock CountdownText;
    private UTextBlock HintText;
    private bool bBuilt = false;

    private float RefreshAccum = 0.0;
    const float RefreshInterval = 0.25;

    UFUNCTION(BlueprintOverride)
    void OnInitialized()
    {
        BuildLayout();
    }

    // Pure menu screen: UI input only, cursor free.
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
        return ReadyButton;
    }

    private void BuildLayout()
    {
        if (bBuilt)
            return;

        Root = Cast<UCanvasPanel>(ConstructWidget(UCanvasPanel::StaticClass()));
        if (Root == nullptr)
            return;
        SetRootWidget(Root);
        bBuilt = true;

        UBorder Backdrop = RogueUITheme::MakePanel(this, RogueUITheme::PanelDark(), 0.0);
        UCanvasPanelSlot BackdropSlot = Root.AddChildToCanvas(Backdrop);
        BackdropSlot.SetAnchors(FAnchors(0.0, 0.0, 1.0, 1.0));
        BackdropSlot.SetOffsets(FMargin(0.0, 0.0, 0.0, 0.0));

        UTextBlock Title = RogueUITheme::MakeText(this, "SQUAD LOBBY", RogueUITheme::TextPrimary(), 2.4);
        UCanvasPanelSlot TitleSlot = Root.AddChildToCanvas(Title);
        TitleSlot.SetAnchors(FAnchors(0.5, 0.1));
        TitleSlot.SetAlignment(FVector2D(0.5, 0.5));
        TitleSlot.SetAutoSize(true);

        // Hero tiles, centered. One reusable ULobbyHeroTile per roster entry.
        TileRow = Cast<UHorizontalBox>(ConstructWidget(UHorizontalBox::StaticClass()));
        UCanvasPanelSlot RowSlot = Root.AddChildToCanvas(TileRow);
        RowSlot.SetAnchors(FAnchors(0.5, 0.42));
        RowSlot.SetAlignment(FVector2D(0.5, 0.5));
        RowSlot.SetAutoSize(true);

        for (int i = 0; i < RogueHeroes::Num(); ++i)
        {
            ULobbyHeroTile Tile = Cast<ULobbyHeroTile>(
                WidgetBlueprint::CreateWidget(ULobbyHeroTile, GetOwningPlayer()));
            if (Tile == nullptr)
                continue;
            Tile.Populate(i, this);
            UHorizontalBoxSlot TileSlot = TileRow.AddChildToHorizontalBox(Tile);
            TileSlot.SetPadding(FMargin(12.0, 0.0, 12.0, 0.0));
            Tiles.Add(Tile);
        }

        // Squad panel: everyone's pick + ready, top-right. Rebuilt on the refresh tick.
        UTextBlock SquadHeader = RogueUITheme::MakeText(this, "SQUAD", RogueUITheme::TextDim(), 1.1);
        UCanvasPanelSlot SquadHeaderSlot = Root.AddChildToCanvas(SquadHeader);
        SquadHeaderSlot.SetAnchors(FAnchors(0.97, 0.1));
        SquadHeaderSlot.SetAlignment(FVector2D(1.0, 0.0));
        SquadHeaderSlot.SetAutoSize(true);

        SquadList = Cast<UVerticalBox>(ConstructWidget(UVerticalBox::StaticClass()));
        UCanvasPanelSlot SquadSlot = Root.AddChildToCanvas(SquadList);
        SquadSlot.SetAnchors(FAnchors(0.97, 0.14));
        SquadSlot.SetAlignment(FVector2D(1.0, 0.0));
        SquadSlot.SetAutoSize(true);

        // Bottom controls: READY toggle + START RAID (host) + countdown.
        UHorizontalBox Controls = Cast<UHorizontalBox>(ConstructWidget(UHorizontalBox::StaticClass()));
        UCanvasPanelSlot ControlsSlot = Root.AddChildToCanvas(Controls);
        ControlsSlot.SetAnchors(FAnchors(0.5, 0.78));
        ControlsSlot.SetAlignment(FVector2D(0.5, 0.5));
        ControlsSlot.SetAutoSize(true);

        ReadyButton = RogueUITheme::MakeTextButton(this, "  READY  ", RogueUITheme::Accent());
        ReadyButton.OnClicked.AddUFunction(this, n"HandleReady");
        UHorizontalBoxSlot ReadySlot = Controls.AddChildToHorizontalBox(ReadyButton);
        ReadySlot.SetPadding(FMargin(10.0, 0.0, 10.0, 0.0));

        APlayerController PC = GetOwningPlayer();
        if (PC != nullptr && PC.HasAuthority())
        {
            StartButton = RogueUITheme::MakeTextButton(this, "  START RAID  ", RogueUITheme::Victory());
            StartButton.OnClicked.AddUFunction(this, n"HandleStart");
            StartButton.SetIsEnabled(false);
            UHorizontalBoxSlot StartSlot = Controls.AddChildToHorizontalBox(StartButton);
            StartSlot.SetPadding(FMargin(10.0, 0.0, 10.0, 0.0));
        }

        CountdownText = RogueUITheme::MakeText(this, "", RogueUITheme::Victory(), 1.8);
        UCanvasPanelSlot CountSlot = Root.AddChildToCanvas(CountdownText);
        CountSlot.SetAnchors(FAnchors(0.5, 0.68));
        CountSlot.SetAlignment(FVector2D(0.5, 0.5));
        CountSlot.SetAutoSize(true);

        HintText = RogueUITheme::MakeText(
            this, "Pick a hero, then READY. The host launches when the whole squad is ready.",
            RogueUITheme::TextDim(), 1.0);
        UCanvasPanelSlot HintSlot = Root.AddChildToCanvas(HintText);
        HintSlot.SetAnchors(FAnchors(0.5, 0.88));
        HintSlot.SetAlignment(FVector2D(0.5, 0.5));
        HintSlot.SetAutoSize(true);
    }

    // Tile click: route through the owning lobby PC (stashes locally + Server_ RPC validates).
    void PickHero(int HeroIndex)
    {
        ALobbyPlayerController PC = Cast<ALobbyPlayerController>(GetOwningPlayer());
        if (PC != nullptr)
            PC.SelectHero(HeroIndex);
    }

    UFUNCTION()
    private void HandleReady()
    {
        ALobbyPlayerController PC = Cast<ALobbyPlayerController>(GetOwningPlayer());
        if (PC == nullptr)
            return;
        ARoguePlayerState PS = Cast<ARoguePlayerState>(PC.PlayerState);
        bool bCurrent = PS != nullptr && PS.bLobbyReady;
        PC.Server_SetReady(!bCurrent);
    }

    UFUNCTION()
    private void HandleStart()
    {
        // Host only — the GameMode doesn't exist on clients, so this is a harmless no-op there.
        ALobbyGameMode GameMode = Cast<ALobbyGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.RequestStartRaid();
    }

    UFUNCTION(BlueprintOverride)
    void Tick(FGeometry MyGeometry, float InDeltaTime)
    {
        RefreshAccum += InDeltaTime;
        if (RefreshAccum < RefreshInterval)
            return;
        RefreshAccum = 0.0;
        RefreshTiles();
        RefreshSquad();
        RefreshControls();
    }

    private ARoguePlayerState GetMyPlayerState() const
    {
        APlayerController PC = GetOwningPlayer();
        return PC != nullptr ? Cast<ARoguePlayerState>(PC.PlayerState) : nullptr;
    }

    private void RefreshTiles()
    {
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        ARoguePlayerState MyPS = GetMyPlayerState();
        int MyPick = MyPS != nullptr ? MyPS.SelectedHeroIndex : -1;

        for (int i = 0; i < Tiles.Num(); ++i)
        {
            FString ClaimedBy = "";
            if (GS != nullptr)
            {
                for (APlayerState BasePS : GS.PlayerArray)
                {
                    ARoguePlayerState PS = Cast<ARoguePlayerState>(BasePS);
                    if (PS != nullptr && PS.SelectedHeroIndex == i)
                    {
                        ClaimedBy = PS.GetPlayerName();
                        break;
                    }
                }
            }
            Tiles[i].SetTileState(MyPick == i, ClaimedBy);
        }
    }

    private void RefreshSquad()
    {
        if (SquadList == nullptr)
            return;
        SquadList.ClearChildren();

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS == nullptr)
            return;

        for (APlayerState BasePS : GS.PlayerArray)
        {
            ARoguePlayerState PS = Cast<ARoguePlayerState>(BasePS);
            if (PS == nullptr)
                continue;

            FString HeroName = "picking...";
            FLinearColor RowColor = RogueUITheme::TextDim();
            if (RogueHeroes::IsValidIndex(PS.SelectedHeroIndex))
            {
                FRogueHeroEntry Entry = RogueHeroes::Get(PS.SelectedHeroIndex);
                HeroName = Entry.Name;
                RowColor = Entry.Color;
            }
            FString ReadyMark = PS.bLobbyReady ? "  [READY]" : "";
            FString PlayerLabel = PS.GetPlayerName();

            UTextBlock Row = RogueUITheme::MakeText(this, f"{PlayerLabel} - {HeroName}{ReadyMark}", RowColor, 1.0);
            UVerticalBoxSlot RowSlot = SquadList.AddChildToVerticalBox(Row);
            RowSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 6.0));
            RowSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Right);
        }
    }

    private void RefreshControls()
    {
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());

        // Launch countdown (replicated world time, same count on every machine).
        if (CountdownText != nullptr)
        {
            if (GS != nullptr && GS.RaidLaunchAt > 0.0)
            {
                int Remaining = int(Math::Max(0.0, GS.RaidLaunchAt - Gameplay::GetTimeSeconds())) + 1;
                CountdownText.SetText(FText::FromString(f"LAUNCHING IN {Remaining}"));
            }
            else
            {
                CountdownText.SetText(FText());
            }
        }

        // Host's START RAID gates on the whole squad being ready.
        if (StartButton != nullptr)
        {
            ALobbyGameMode GameMode = Cast<ALobbyGameMode>(Gameplay::GetGameMode());
            StartButton.SetIsEnabled(GameMode != nullptr && GameMode.AreAllPlayersReady());
        }
    }
}
