// UpgradeCardWidget.as
// ONE reusable upgrade card — there is deliberately no per-upgrade widget class. The select
// screen creates N of these at runtime and Populate() fills each from a URogueUpgradeDef
// (icon, name, value line, rarity color, description, hotkey hint). Runtime-built tree like
// RogueHUDWidget (no UMG asset to maintain).
//
// Card anatomy follows the genre standard (Hades boons / Gunfire Reborn ascensions):
// rarity-colored frame, icon top, name, big concrete value line, detail text, [N] hotkey.
// The whole card is a button; clicking it (or the hotkey, handled by the select screen)
// routes the pick back through UUpgradeSelectWidget.ChooseUpgrade().

class UUpgradeCardWidget : UUserWidget
{
    // Fixed card footprint (research: ~280x420 reads well in a centered row of 3).
    const float CardWidth = 280.0;
    const float CardHeight = 420.0;

    private UButton RootButton;
    private UBorder Frame;          // rarity-colored outer border
    private UImage IconImage;
    private UTextBlock NameText;
    private UTextBlock RarityText;
    private UTextBlock ValueText;
    private UTextBlock DescText;
    private UTextBlock HotkeyText;
    private bool bBuilt = false;

    private UUpgradeSelectWidget OwnerScreen;
    private int OfferIndex = -1;

    UFUNCTION(BlueprintOverride)
    void OnInitialized()
    {
        BuildLayout();
    }

    private void BuildLayout()
    {
        if (bBuilt)
            return;

        // The card IS a button so the entire face is clickable.
        RootButton = Cast<UButton>(ConstructWidget(UButton::StaticClass()));
        if (RootButton == nullptr)
            return;
        SetRootWidget(RootButton);
        bBuilt = true;
        RootButton.SetBackgroundColor(FLinearColor(0.0, 0.0, 0.0));
        RootButton.OnClicked.AddUFunction(this, n"HandleClicked");

        USizeBox SizeBox = Cast<USizeBox>(ConstructWidget(USizeBox::StaticClass()));
        SizeBox.SetWidthOverride(CardWidth);
        SizeBox.SetHeightOverride(CardHeight);
        RootButton.AddChild(SizeBox);

        // Outer border = the rarity frame; inner panel = the dark card face. The 4px gap
        // between them is what renders as the colored frame.
        Frame = RogueUITheme::MakePanel(this, RogueUITheme::RarityColor(1), 4.0);
        SizeBox.AddChild(Frame);

        UBorder Face = RogueUITheme::MakePanel(this, RogueUITheme::PanelDark, 14.0);
        Frame.AddChild(Face);

        UVerticalBox Column = Cast<UVerticalBox>(ConstructWidget(UVerticalBox::StaticClass()));
        Face.AddChild(Column);

        // Icon block, top-center. Real texture when the def has one; rarity-tinted square until then.
        IconImage = Cast<UImage>(ConstructWidget(UImage::StaticClass()));
        IconImage.SetDesiredSizeOverride(FVector2D(96.0, 96.0));
        UVerticalBoxSlot IconSlot = Column.AddChildToVerticalBox(IconImage);
        IconSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        IconSlot.SetPadding(FMargin(0.0, 6.0, 0.0, 14.0));

        NameText = RogueUITheme::MakeText(this, "", RogueUITheme::TextPrimary, 1.35);
        UVerticalBoxSlot NameSlot = Column.AddChildToVerticalBox(NameText);
        NameSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        NameSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 8.0));

        RarityText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim, 0.9);
        UVerticalBoxSlot RaritySlot = Column.AddChildToVerticalBox(RarityText);
        RaritySlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        RaritySlot.SetPadding(FMargin(0.0, 0.0, 0.0, 12.0));

        // The "value": the short concrete stat line, accent-colored — numbers ON the card
        // (the RoR2 icons-only approach is the known anti-pattern).
        ValueText = RogueUITheme::MakeText(this, "", RogueUITheme::Accent, 1.15);
        UVerticalBoxSlot ValueSlot = Column.AddChildToVerticalBox(ValueText);
        ValueSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        ValueSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 12.0));

        DescText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim, 1.0, true);
        UVerticalBoxSlot DescSlot = Column.AddChildToVerticalBox(DescText);
        DescSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);

        // Spacer pushes the hotkey hint to the card's bottom edge.
        USpacer Spacer = Cast<USpacer>(ConstructWidget(USpacer::StaticClass()));
        UVerticalBoxSlot SpacerSlot = Column.AddChildToVerticalBox(Spacer);
        FSlateChildSize FillSize;
        FillSize.SizeRule = ESlateSizeRule::Fill;
        SpacerSlot.SetSize(FillSize);

        HotkeyText = RogueUITheme::MakeText(this, "", RogueUITheme::TextDim, 1.0);
        UVerticalBoxSlot HotkeySlot = Column.AddChildToVerticalBox(HotkeyText);
        HotkeySlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
    }

    // Fill the card from a definition. Index is this card's slot in the offer (drives the
    // hotkey hint and the pick routed back to the screen).
    void Populate(URogueUpgradeDef Def, int Index, UUpgradeSelectWidget Screen)
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
            // Placeholder block: the rarity color at half strength reads as "icon goes here".
            IconImage.SetColorAndOpacity(Rarity * 0.45);
        }

        NameText.SetText(Def.DisplayName);
        RarityText.SetText(FText::FromString(RogueUITheme::RarityName(Def.Rarity)));
        RarityText.SetColorAndOpacity(FSlateColor(Rarity));
        ValueText.SetText(Def.ValueText);
        DescText.SetText(Def.Description);
        HotkeyText.SetText(FText::FromString(f"[ {Index + 1} ]"));
    }

    UFUNCTION()
    private void HandleClicked()
    {
        if (OwnerScreen != nullptr)
            OwnerScreen.ChooseUpgrade(OfferIndex);
    }
}
