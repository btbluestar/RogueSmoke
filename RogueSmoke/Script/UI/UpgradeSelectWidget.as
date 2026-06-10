// UpgradeSelectWidget.as
// Choose-1-of-N upgrade screen: dimmed overlay + a centered row of UUpgradeCardWidget cards,
// one per offered URogueUpgradeDef (the cards are populated at runtime — no per-upgrade
// widgets). Pick by clicking a card or pressing its number key. The raid PAUSES while picks
// are open (UpgradeLoop concept, 2026-06-11): the GameMode pauses on offer and resumes once
// every player has picked (or its watchdog times out). UI input works fine while paused.
//
// CommonUI: an activatable pushed onto the GameMenu layer stack by RaidPlayerController, which
// then calls Setup(Options) to build the cards. Its input config is All — menu input for the
// cards AND game input underneath, because the raid keeps running while you choose. NOT a back
// handler: you commit to a pick. Closes itself with DeactivateWidget() (the stack pops it; the
// HUD host restores Game capture).
class UUpgradeSelectWidget : UCommonActivatableWidget
{
    // The upgrades offered this pick. Set via Setup() right after the stack push.
    UPROPERTY(EditAnywhere, Category = "Upgrades")
    TArray<URogueUpgradeDef> OfferedUpgrades;

    default bIsFocusable = true;     // so number-key picks reach OnKeyDown
    default bIsBackHandler = false;  // no backing out of the pick

    private UCanvasPanel Root;
    private UHorizontalBox CardRow;
    private TArray<UUpgradeCardWidget> Cards;
    private bool bBuilt = false;
    private bool bCardsBuilt = false;

    UFUNCTION(BlueprintOverride)
    void OnInitialized()
    {
        BuildFrame();
    }

    // Called by the PC right after pushing this onto the GameMenu stack (the stack constructs
    // the widget itself, so the offer can't be set before creation like the old flow did).
    void Setup(TArray<URogueUpgradeDef> Options)
    {
        OfferedUpgrades = Options;
        BuildCards();
    }

    // Game keeps running under the overlay: route input to BOTH, cursor free for the cards.
    UFUNCTION(BlueprintOverride)
    FUIInputConfig GetDesiredInputConfig() const
    {
        FUIInputConfig Config;
        Config.InputMode = ECommonInputMode::All;
        Config.MouseCaptureMode = EMouseCaptureMode::NoCapture;
        return Config;
    }

    UFUNCTION(BlueprintOverride)
    UWidget BP_GetDesiredFocusTarget() const
    {
        return Cards.Num() > 0 ? Cards[0].GetFocusWidget() : nullptr;
    }

    private void BuildFrame()
    {
        if (bBuilt)
            return;

        Root = Cast<UCanvasPanel>(ConstructWidget(UCanvasPanel::StaticClass()));
        if (Root == nullptr)
            return;
        SetRootWidget(Root);
        bBuilt = true;

        // Full-screen dim so the cards pop while the world stays visible behind them.
        UBorder Backdrop = RogueUITheme::MakePanel(this, FLinearColor(0.0, 0.0, 0.0), 0.0);
        Backdrop.SetBrushColor(FLinearColor(0.0, 0.0, 0.0, 0.55));
        UCanvasPanelSlot BackdropSlot = Root.AddChildToCanvas(Backdrop);
        BackdropSlot.SetAnchors(FAnchors(0.0, 0.0, 1.0, 1.0));
        BackdropSlot.SetOffsets(FMargin(0.0, 0.0, 0.0, 0.0));

        UTextBlock Title = RogueUITheme::MakeText(this, "CHOOSE AN UPGRADE", RogueUITheme::TextPrimary, 2.2);
        UCanvasPanelSlot TitleSlot = Root.AddChildToCanvas(Title);
        TitleSlot.SetAnchors(FAnchors(0.5, 0.18));
        TitleSlot.SetAlignment(FVector2D(0.5, 0.5));
        TitleSlot.SetAutoSize(true);

        CardRow = Cast<UHorizontalBox>(ConstructWidget(UHorizontalBox::StaticClass()));
        UCanvasPanelSlot RowSlot = Root.AddChildToCanvas(CardRow);
        RowSlot.SetAnchors(FAnchors(0.5, 0.5));
        RowSlot.SetAlignment(FVector2D(0.5, 0.45));
        RowSlot.SetAutoSize(true);

        UTextBlock Hint = RogueUITheme::MakeText(this, "Click a card or press its number — the raid is paused until everyone picks", RogueUITheme::TextDim, 1.0);
        UCanvasPanelSlot HintSlot = Root.AddChildToCanvas(Hint);
        HintSlot.SetAnchors(FAnchors(0.5, 0.82));
        HintSlot.SetAlignment(FVector2D(0.5, 0.5));
        HintSlot.SetAutoSize(true);
    }

    private void BuildCards()
    {
        if (bCardsBuilt || CardRow == nullptr)
            return;
        bCardsBuilt = true;

        for (int i = 0; i < OfferedUpgrades.Num(); ++i)
        {
            UUpgradeCardWidget Card = Cast<UUpgradeCardWidget>(
                WidgetBlueprint::CreateWidget(UUpgradeCardWidget, GetOwningPlayer()));
            if (Card == nullptr)
                continue;
            Card.Populate(OfferedUpgrades[i], i, this);
            UHorizontalBoxSlot CardSlot = CardRow.AddChildToHorizontalBox(Card);
            CardSlot.SetPadding(FMargin(12.0, 0.0, 12.0, 0.0));
            Cards.Add(Card);
        }

        SetKeyboardFocus();
    }

    // Number-key picks: 1..N selects that card (mirrors the on-card [N] hint).
    UFUNCTION(BlueprintOverride)
    FEventReply OnKeyDown(FGeometry MyGeometry, FKeyEvent InKeyEvent)
    {
        FKey Key = InKeyEvent.GetKey();
        int Picked = -1;
        if (Key == EKeys::One)
            Picked = 0;
        else if (Key == EKeys::Two)
            Picked = 1;
        else if (Key == EKeys::Three)
            Picked = 2;
        else if (Key == EKeys::Four)
            Picked = 3;

        if (Picked >= 0 && Picked < OfferedUpgrades.Num())
        {
            ChooseUpgrade(Picked);
            return FEventReply::Handled();
        }
        return FEventReply::Unhandled();
    }

    // Routes the choice through the owning pawn's server RPC so the upgrade's GameplayEffect
    // applies authoritatively. Called by the cards (click) or OnKeyDown (hotkey).
    UFUNCTION(BlueprintCallable)
    void ChooseUpgrade(int Index)
    {
        if (Index < 0 || Index >= OfferedUpgrades.Num())
            return;

        AHeroCharacter Hero = Cast<AHeroCharacter>(GetOwningPlayerPawn());
        if (Hero != nullptr)
            Hero.Server_ApplyUpgrade(OfferedUpgrades[Index]);

        ARaidPlayerController PC = Cast<ARaidPlayerController>(GetOwningPlayer());
        if (PC != nullptr)
            PC.CloseUpgradeScreen();   // bookkeeping only; input/cursor is CommonUI's job now

        DeactivateWidget();   // the GameMenu stack pops us
    }
}
