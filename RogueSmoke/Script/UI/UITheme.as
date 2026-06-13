// UITheme.as
// Shared UI design tokens + construction helpers for the runtime-built widget trees
// (RogueHUDWidget pattern: no UMG designer layouts — see that file's header for why).
// Every screen pulls colors from here so an art pass touches ONE place.
//
// The palette now reads from a designer-editable DataAsset (URogueUIThemeData / DA_UITheme) via the
// theme subsystem, so the user retints the whole UI in-editor without touching code. The functions
// fall back to the original literals if the asset/subsystem isn't available, so nothing breaks.
// Rarity colors follow the genre convention (Hades/loot-tier: common grey-white .. legendary orange).
namespace RogueUITheme
{
    // Resolve the cached theme (designer-authored DA_UITheme, else class defaults). May be null only
    // if there's no GameInstance subsystem yet (not a normal UI context) — callers fall back.
    URogueUIThemeData Data()
    {
        URogueUIThemeSubsystem Sub = URogueUIThemeSubsystem::Get();
        return Sub != nullptr ? Sub.GetTheme() : nullptr;
    }

    // Core palette accessors. Were `const FLinearColor` — now functions so the values can come from
    // the editable asset. Fallback literals mirror URogueUIThemeData's defaults.
    FLinearColor Accent()      { URogueUIThemeData T = Data(); return T != nullptr ? T.Accent      : FLinearColor(0.27, 0.84, 0.77); }
    FLinearColor Danger()      { URogueUIThemeData T = Data(); return T != nullptr ? T.Danger      : FLinearColor(0.90, 0.28, 0.30); }
    FLinearColor Victory()     { URogueUIThemeData T = Data(); return T != nullptr ? T.Victory     : FLinearColor(0.35, 0.90, 0.40); }
    FLinearColor TextPrimary() { URogueUIThemeData T = Data(); return T != nullptr ? T.TextPrimary : FLinearColor(0.92, 0.95, 0.96); }
    FLinearColor TextDim()     { URogueUIThemeData T = Data(); return T != nullptr ? T.TextDim     : FLinearColor(0.55, 0.60, 0.63); }
    FLinearColor PanelDark()   { URogueUIThemeData T = Data(); return T != nullptr ? T.PanelDark   : FLinearColor(0.045, 0.055, 0.07); }
    FLinearColor BackdropDim() { URogueUIThemeData T = Data(); return T != nullptr ? T.BackdropDim : FLinearColor(0.0, 0.0, 0.0); }
    FLinearColor Shield()      { URogueUIThemeData T = Data(); return T != nullptr ? T.Shield      : FLinearColor(0.35, 0.62, 1.0); }

    FLinearColor RarityColor(int Rarity)
    {
        URogueUIThemeData T = Data();
        if (T != nullptr)
            return T.RarityFor(Rarity);
        if (Rarity >= 4)
            return FLinearColor(1.0, 0.55, 0.15);    // legendary orange
        if (Rarity == 3)
            return FLinearColor(0.62, 0.30, 0.90);   // epic purple
        if (Rarity == 2)
            return FLinearColor(0.25, 0.55, 1.0);    // rare blue
        return FLinearColor(0.72, 0.75, 0.78);       // common grey-white
    }

    FString RarityName(int Rarity)
    {
        if (Rarity >= 4)
            return "LEGENDARY";
        if (Rarity == 3)
            return "EPIC";
        if (Rarity == 2)
            return "RARE";
        return "COMMON";
    }

    // Construct a styled text block inside Host's widget tree. RenderScale (not font size)
    // is the established sizing mechanism here — keep wrapped body text at scale 1.0 so the
    // wrap width stays correct (scaling happens after layout).
    UTextBlock MakeText(UUserWidget Host, FString Content, FLinearColor Color, float Scale = 1.0, bool bWrap = false)
    {
        UTextBlock Text = Cast<UTextBlock>(Host.ConstructWidget(UTextBlock::StaticClass()));
        if (Text == nullptr)
            return nullptr;
        Text.SetText(FText::FromString(Content));
        Text.SetColorAndOpacity(FSlateColor(Color));
        if (Scale != 1.0)
            Text.SetRenderScale(FVector2D(Scale, Scale));
        if (bWrap)
            Text.AutoWrapText = true;
        return Text;
    }

    // Construct a flat-color border panel (the building block for cards/backdrops).
    UBorder MakePanel(UUserWidget Host, FLinearColor Color, float Padding)
    {
        UBorder Panel = Cast<UBorder>(Host.ConstructWidget(UBorder::StaticClass()));
        if (Panel == nullptr)
            return nullptr;
        Panel.SetBrushColor(Color);
        Panel.SetPadding(FMargin(Padding, Padding, Padding, Padding));
        return Panel;
    }

    // Construct a button with a centered text label, styled flat. The caller binds OnClicked.
    UButton MakeTextButton(UUserWidget Host, FString Label, FLinearColor LabelColor)
    {
        UButton Button = Cast<UButton>(Host.ConstructWidget(UButton::StaticClass()));
        if (Button == nullptr)
            return nullptr;
        Button.SetBackgroundColor(FLinearColor(0.10, 0.12, 0.15));

        UTextBlock Text = MakeText(Host, Label, LabelColor, 1.2);
        if (Text != nullptr)
            Button.AddChild(Text);
        return Button;
    }
}
