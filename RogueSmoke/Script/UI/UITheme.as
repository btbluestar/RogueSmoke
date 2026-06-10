// UITheme.as
// Shared UI design tokens + construction helpers for the runtime-built widget trees
// (RogueHUDWidget pattern: no UMG designer layouts — see that file's header for why).
// Every screen pulls colors/sizes from here so a later art pass touches ONE place.
//
// Rarity colors follow the genre convention players already know (Hades/loot-tier
// standard): common grey-white, rare blue, epic purple, legendary orange.

namespace RogueUITheme
{
    // Core palette (mirrors RogueHUDWidget / the HUD mockup tokens).
    const FLinearColor Accent = FLinearColor(0.27, 0.84, 0.77);      // teal — interactive / positive
    const FLinearColor Danger = FLinearColor(0.90, 0.28, 0.30);      // red — damage / defeat
    const FLinearColor Victory = FLinearColor(0.35, 0.90, 0.40);     // green — success
    const FLinearColor TextPrimary = FLinearColor(0.92, 0.95, 0.96);
    const FLinearColor TextDim = FLinearColor(0.55, 0.60, 0.63);
    const FLinearColor PanelDark = FLinearColor(0.045, 0.055, 0.07); // card / panel fill
    const FLinearColor BackdropDim = FLinearColor(0.0, 0.0, 0.0);    // full-screen dim (use alpha)

    FLinearColor RarityColor(int Rarity)
    {
        if (Rarity >= 4)
            return FLinearColor(1.0, 0.55, 0.15);   // legendary orange
        if (Rarity == 3)
            return FLinearColor(0.62, 0.30, 0.90);  // epic purple
        if (Rarity == 2)
            return FLinearColor(0.25, 0.55, 1.0);   // rare blue
        return FLinearColor(0.72, 0.75, 0.78);      // common grey-white
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
