// UpgradeSelectWidget.as
// Choose-1-of-N upgrade screen: dimmed overlay + a centered row of UUpgradeCardWidget cards,
// one per offered URogueUpgradeDef (the cards are populated at runtime — no per-upgrade
// widgets). Pick by clicking a card or pressing its number key. The game does NOT pause
// (genre convention + listen server: teammates are still fighting).
//
// Tree is runtime-built (RogueHUDWidget pattern). Flow: RaidPlayerController creates the
// widget (OnInitialized builds the static frame), sets OfferedUpgrades, then AddToViewport
// (Construct builds the cards from the now-known offer).
class UUpgradeSelectWidget : UUserWidget
{
    // The upgrades offered this pick. Set by Client_OfferUpgrades BEFORE AddToViewport.
    UPROPERTY(EditAnywhere, Category = "Upgrades")
    TArray<URogueUpgradeDef> OfferedUpgrades;

    default bIsFocusable = true;   // so number-key picks reach OnKeyDown

    private UCanvasPanel Root;
    private UHorizontalBox CardRow;
    private bool bBuilt = false;
    private bool bCardsBuilt = false;

    UFUNCTION(BlueprintOverride)
    void OnInitialized()
    {
        BuildFrame();
    }

    UFUNCTION(BlueprintOverride)
    void Construct()
    {
        BuildCards();
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

        UTextBlock Hint = RogueUITheme::MakeText(this, "Click a card or press its number — the raid does not pause", RogueUITheme::TextDim, 1.0);
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

        // Hand input back to gameplay (the PC owns the cursor + active-widget bookkeeping).
        ARaidPlayerController PC = Cast<ARaidPlayerController>(GetOwningPlayer());
        if (PC != nullptr)
            PC.CloseUpgradeScreen();

        RemoveFromParent();
    }
}
