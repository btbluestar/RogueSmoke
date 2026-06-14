// LobbyHeroTile.as
// One selectable hero tile in the lobby's hero-select row. Same pattern as UUpgradeCardWidget:
// ONE reusable class, populated at runtime from the RogueHeroes roster entry — no per-hero
// widgets. The whole tile is a button; clicking routes the pick back through the lobby widget.
class ULobbyHeroTile : UUserWidget
{
    const float TileWidth = 220.0;
    const float TileHeight = 230.0;

    private UButton RootButton;
    private UBorder Frame;
    private UTextBlock NameText;
    private UTextBlock RoleText;
    private UTextBlock BlurbText;
    private UTextBlock ClaimText;     // "P2" when someone has claimed this hero
    private bool bBuilt = false;

    private ULobbyWidget OwnerLobby;
    private int HeroIndex = -1;
    private FLinearColor HeroColor;

    UFUNCTION(BlueprintOverride)
    void OnInitialized()
    {
        BuildLayout();
    }

    private void BuildLayout()
    {
        if (bBuilt)
            return;

        RootButton = Cast<UButton>(ConstructWidget(UButton::StaticClass()));
        if (RootButton == nullptr)
            return;
        SetRootWidget(RootButton);
        bBuilt = true;
        RootButton.SetBackgroundColor(FLinearColor(0.0, 0.0, 0.0));
        RootButton.OnClicked.AddUFunction(this, n"HandleClicked");

        USizeBox SizeBox = Cast<USizeBox>(ConstructWidget(USizeBox::StaticClass()));
        SizeBox.SetWidthOverride(TileWidth);
        SizeBox.SetHeightOverride(TileHeight);
        RootButton.AddChild(SizeBox);

        // Outer frame tints to the hero color when selected (selection feedback).
        Frame = RogueUITheme::MakePanel(this, FLinearColor(0.16, 0.18, 0.21), 3.0);
        SizeBox.AddChild(Frame);

        UBorder Face = RogueUITheme::MakePanel(this, RogueUITheme::PanelDark(), 12.0);
        Frame.AddChild(Face);

        UVerticalBox Column = Cast<UVerticalBox>(ConstructWidget(UVerticalBox::StaticClass()));
        Face.AddChild(Column);

        NameText = RogueUITheme::MakeText(this, "", RogueUITheme::TextPrimary(), 1.3);
        UVerticalBoxSlot NameSlot = Column.AddChildToVerticalBox(NameText);
        NameSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        NameSlot.SetPadding(FMargin(0.0, 8.0, 0.0, 6.0));

        RoleText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim(), 1.0);
        UVerticalBoxSlot RoleSlot = Column.AddChildToVerticalBox(RoleText);
        RoleSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        RoleSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 10.0));

        BlurbText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim(), 0.95, true);
        UVerticalBoxSlot BlurbSlot = Column.AddChildToVerticalBox(BlurbText);
        BlurbSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);

        USpacer Spacer = Cast<USpacer>(ConstructWidget(USpacer::StaticClass()));
        UVerticalBoxSlot SpacerSlot = Column.AddChildToVerticalBox(Spacer);
        FSlateChildSize FillSize;
        FillSize.SizeRule = ESlateSizeRule::Fill;
        SpacerSlot.SetSize(FillSize);

        ClaimText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim(), 1.0);
        UVerticalBoxSlot ClaimSlot = Column.AddChildToVerticalBox(ClaimText);
        ClaimSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
    }

    void Populate(int Index, ULobbyWidget Lobby)
    {
        HeroIndex = Index;
        OwnerLobby = Lobby;
        if (!bBuilt || !RogueHeroes::IsValidIndex(Index))
            return;

        FRogueHeroEntry Entry = RogueHeroes::Get(Index);
        HeroColor = Entry.Color;
        NameText.SetText(FText::FromString(Entry.Name));
        NameText.SetColorAndOpacity(FSlateColor(Entry.Color));
        RoleText.SetText(FText::FromString(Entry.Role));
        BlurbText.SetText(FText::FromString(Entry.Blurb));
    }

    // Live state from the lobby's refresh tick: frame lights up for the local pick; the claim
    // line shows who owns the hero (and the tile reads as taken).
    void SetTileState(bool bSelectedByMe, FString ClaimedBy)
    {
        if (!bBuilt)
            return;
        if (bSelectedByMe)
            Frame.SetBrushColor(HeroColor);
        else if (!ClaimedBy.IsEmpty())
            Frame.SetBrushColor(HeroColor * 0.35);
        else
            Frame.SetBrushColor(FLinearColor(0.16, 0.18, 0.21));
        ClaimText.SetText(FText::FromString(ClaimedBy));
    }

    UFUNCTION()
    private void HandleClicked()
    {
        if (OwnerLobby != nullptr)
            OwnerLobby.PickHero(HeroIndex);
    }
}
