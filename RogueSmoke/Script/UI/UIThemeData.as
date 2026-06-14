// UIThemeData.as
// Designer-editable UI palette (D-2026-06-13 "make the UI editable by me"). The whole UI is built
// procedurally in AngelScript (see RogueHUDWidget header), so colors used to be hardcoded consts the
// user could only change by editing code. This DataAsset exposes them as EditAnywhere properties:
// edit ONE asset (DA_UITheme) in the editor and every screen retints — no code, no prompting Claude.
//
// How it's wired: RogueUITheme (UITheme.as) reads colors through here instead of from consts; the
// subsystem below loads DA_UITheme once and caches it. If the asset is missing, it falls back to a
// default-constructed instance, so these property defaults (= the original literals) are the source
// of truth and nothing breaks. Defaults are intentionally identical to the old consts (non-breaking).
//
// Scope: COLORS for now. Sizes/font scales are still consts in the widgets — a documented follow-up
// (see docs/superpowers/plans/2026-06-13-ui-designer-editable.md).
class URogueUIThemeData : UDataAsset
{
    // Core palette (interactive teal / danger red / success green / text / panels).
    UPROPERTY(EditAnywhere, Category = "Palette")
    FLinearColor Accent = FLinearColor(0.27, 0.84, 0.77);

    UPROPERTY(EditAnywhere, Category = "Palette")
    FLinearColor Danger = FLinearColor(0.90, 0.28, 0.30);

    UPROPERTY(EditAnywhere, Category = "Palette")
    FLinearColor Victory = FLinearColor(0.35, 0.90, 0.40);

    UPROPERTY(EditAnywhere, Category = "Palette")
    FLinearColor TextPrimary = FLinearColor(0.92, 0.95, 0.96);

    UPROPERTY(EditAnywhere, Category = "Palette")
    FLinearColor TextDim = FLinearColor(0.55, 0.60, 0.63);

    UPROPERTY(EditAnywhere, Category = "Palette")
    FLinearColor PanelDark = FLinearColor(0.045, 0.055, 0.07);

    UPROPERTY(EditAnywhere, Category = "Palette")
    FLinearColor BackdropDim = FLinearColor(0.0, 0.0, 0.0);

    // Shield bar fill (HUD). Was RogueHUDWidget::ShieldColor.
    UPROPERTY(EditAnywhere, Category = "Palette")
    FLinearColor Shield = FLinearColor(0.35, 0.62, 1.0);

    // Rarity tiers (Hades/loot-tier convention): common grey-white .. legendary orange.
    UPROPERTY(EditAnywhere, Category = "Rarity")
    FLinearColor RarityCommon = FLinearColor(0.72, 0.75, 0.78);

    UPROPERTY(EditAnywhere, Category = "Rarity")
    FLinearColor RarityRare = FLinearColor(0.25, 0.55, 1.0);

    UPROPERTY(EditAnywhere, Category = "Rarity")
    FLinearColor RarityEpic = FLinearColor(0.62, 0.30, 0.90);

    UPROPERTY(EditAnywhere, Category = "Rarity")
    FLinearColor RarityLegendary = FLinearColor(1.0, 0.55, 0.15);

    FLinearColor RarityFor(int Rarity) const
    {
        if (Rarity >= 4)
            return RarityLegendary;
        if (Rarity == 3)
            return RarityEpic;
        if (Rarity == 2)
            return RarityRare;
        return RarityCommon;
    }
}

// Caches the resolved theme for the process. A GameInstance subsystem (like URaidSessionSubsystem)
// lives the whole session on every machine; the cache is a member (allowed — only namespace/global
// vars must be const in this fork), resolved lazily on first read.
class URogueUIThemeSubsystem : UGameInstanceSubsystem
{
    UPROPERTY()
    private URogueUIThemeData CachedTheme = nullptr;

    URogueUIThemeData GetTheme()
    {
        if (CachedTheme == nullptr)
        {
            // Designer-authored override at a fixed path; falls back to class defaults if absent so
            // the UI never depends on the asset existing (it just can't be tweaked until it does).
            CachedTheme = Cast<URogueUIThemeData>(LoadObject(nullptr, "/Game/UI/DA_UITheme.DA_UITheme"));
            if (CachedTheme == nullptr)
                CachedTheme = Cast<URogueUIThemeData>(NewObject(this, URogueUIThemeData));
        }
        return CachedTheme;
    }
}
