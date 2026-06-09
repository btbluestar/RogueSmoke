// RogueHUDWidget.as
// In-game HUD (crosshair / health / shield / ammo / objective). CODING_STANDARDS §6 says "logic in
// script, layout in the asset", but UMG widget-tree authoring isn't reachable from the editor's
// Python/MCP surface here, so this HUD builds its own widget tree at runtime in OnInitialized
// (UUserWidget exposes ConstructWidget + SetRootWidget for exactly this). WBP_HUD is therefore just
// an empty Blueprint child of this class — there's no layout to maintain in the editor.
//
// Driven from RaidPlayerController on the owning client; reads the local pawn's ASC + weapon.
//
// Networking note: weapon ammo/Definition are SERVER-ONLY today (WeaponComponent), so ammo reads
// correctly on the listen-server host but shows "--" on remote clients until weapon state replicates.
// Health/Shield come from the ASC, which replicates to the owning client, so those are correct.
//
// Follow-ups (Design/hud_mockup.html): squad list, ability cooldown slots, off-screen edge indicators.
class URogueHUDWidget : UUserWidget
{
    // Built once in OnInitialized; children are owned by the widget tree (no UPROPERTY needed, same as
    // RaidPlayerController's ActiveUpgradeWidget handle).
    private UCanvasPanel Root;
    private UProgressBar HealthBar;
    private UTextBlock HealthText;
    private UProgressBar ShieldBar;
    private UTextBlock AmmoText;
    private UTextBlock ObjectiveText;

    private AHeroCharacter Hero;
    private bool bBuilt = false;

    // Palette (mirrors the mockup tokens): teal accent, red danger, light-blue shield.
    const FLinearColor Accent = FLinearColor(0.27, 0.84, 0.77);
    const FLinearColor Danger = FLinearColor(0.90, 0.28, 0.30);
    const FLinearColor ShieldColor = FLinearColor(0.35, 0.62, 1.0);

    UFUNCTION(BlueprintOverride)
    void OnInitialized()
    {
        BuildLayout();
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

        // Crosshair: a simple centered '+' marking where fire is aimed (screen-center, D-0014 shooter).
        UTextBlock Crosshair = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        Crosshair.SetText(FText::FromString("+"));
        AddChild(Crosshair, FAnchors(0.5, 0.5), FVector2D(0.5, 0.5), FVector2D(0.0, 0.0), FVector2D(), true);

        // Objective banner: top-center.
        ObjectiveText = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        AddChild(ObjectiveText, FAnchors(0.5, 0.0), FVector2D(0.5, 0.0), FVector2D(0.0, 28.0), FVector2D(), true);

        // Self vitals: bottom-left. Health number above the bar, shield as a thin bar above that.
        HealthText = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        AddChild(HealthText, FAnchors(0.0, 1.0), FVector2D(0.0, 1.0), FVector2D(40.0, -70.0), FVector2D(), true);

        ShieldBar = Cast<UProgressBar>(ConstructWidget(UProgressBar::StaticClass()));
        ShieldBar.SetFillColorAndOpacity(ShieldColor);
        ShieldBar.SetPercent(0.0);
        AddChild(ShieldBar, FAnchors(0.0, 1.0), FVector2D(0.0, 1.0), FVector2D(40.0, -64.0), FVector2D(320.0, 8.0), false);

        HealthBar = Cast<UProgressBar>(ConstructWidget(UProgressBar::StaticClass()));
        HealthBar.SetFillColorAndOpacity(Accent);
        HealthBar.SetPercent(1.0);
        AddChild(HealthBar, FAnchors(0.0, 1.0), FVector2D(0.0, 1.0), FVector2D(40.0, -40.0), FVector2D(320.0, 18.0), false);

        // Ammo: bottom-right.
        AmmoText = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        AddChild(AmmoText, FAnchors(1.0, 1.0), FVector2D(1.0, 1.0), FVector2D(-40.0, -40.0), FVector2D(), true);
    }

    // Add a widget to the root canvas with anchor-relative placement. When bAutoSize, the widget sizes
    // to its content (texts); otherwise Size is applied (bars).
    private void AddChild(UWidget W, FAnchors Anchors, FVector2D Alignment, FVector2D Position, FVector2D Size, bool bAutoSize)
    {
        if (W == nullptr || Root == nullptr)
            return;
        UCanvasPanelSlot Slot = Root.AddChildToCanvas(W);
        if (Slot == nullptr)
            return;
        Slot.SetAnchors(Anchors);
        Slot.SetAlignment(Alignment);
        Slot.SetPosition(Position);
        if (bAutoSize)
            Slot.SetAutoSize(true);
        else
            Slot.SetSize(Size);
    }

    UFUNCTION(BlueprintOverride)
    void Tick(FGeometry MyGeometry, float InDeltaTime)
    {
        RefreshHero();
        RefreshVitals();
        RefreshAmmo();
        RefreshObjective();
    }

    private void RefreshHero()
    {
        if (Hero == nullptr || Hero.GetController() == nullptr)
            Hero = Cast<AHeroCharacter>(GetOwningPlayerPawn());
    }

    private void RefreshVitals()
    {
        if (Hero == nullptr)
            return;

        UAngelscriptAbilitySystemComponent ASC = Hero.GetRogueAbilitySystem();
        if (ASC == nullptr)
            return;

        float Health = ASC.GetAttributeCurrentValue(URogueHealthSet, n"Health", 0.0);
        float MaxHealth = ASC.GetAttributeCurrentValue(URogueHealthSet, n"MaxHealth", 1.0);
        if (MaxHealth <= 0.0)
            MaxHealth = 1.0;
        float Frac = Math::Clamp(Health / MaxHealth, 0.0, 1.0);

        if (HealthBar != nullptr)
        {
            HealthBar.SetPercent(Frac);
            HealthBar.SetFillColorAndOpacity(Frac <= 0.3 ? Danger : Accent);   // low-health warning
        }
        if (HealthText != nullptr)
            HealthText.SetText(FText::FromString(f"{int(Math::Max(0.0, Health))} / {int(MaxHealth)}"));

        if (ShieldBar != nullptr)
        {
            float Shield = ASC.GetAttributeCurrentValue(URogueHealthSet, n"Shield", 0.0);
            float MaxShield = ASC.GetAttributeCurrentValue(URogueHealthSet, n"MaxShield", 0.0);
            ShieldBar.SetPercent(MaxShield > 0.0 ? Math::Clamp(Shield / MaxShield, 0.0, 1.0) : 0.0);
            ShieldBar.SetVisibility(MaxShield > 0.0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
        }
    }

    private void RefreshAmmo()
    {
        if (AmmoText == nullptr)
            return;

        // Weapon ammo/Definition are server-only today: a null Definition (client) => unknown.
        if (Hero != nullptr && Hero.Weapon != nullptr && Hero.Weapon.Definition != nullptr)
        {
            URogueWeaponComponent W = Hero.Weapon;
            AmmoText.SetText(FText::FromString(f"{W.AmmoInMag} / {W.Definition.MagazineSize}"));
        }
        else
        {
            AmmoText.SetText(FText::FromString("-- / --"));
        }
    }

    private void RefreshObjective()
    {
        if (ObjectiveText == nullptr)
            return;

        TArray<ARaidObjective> Objectives;
        GetAllActorsOfClass(Objectives);
        if (Objectives.Num() == 0)
        {
            ObjectiveText.SetText(FText());
            return;
        }

        ARaidObjective Obj = Objectives[0];
        FString Label;
        switch (Obj.Phase)
        {
            case ERaidPhase::InProgress:
                Label = "CLEAR THE ARENA";
                break;
            case ERaidPhase::ExtractionReady:
                Label = "OBJECTIVE COMPLETE - CALL EXTRACTION";
                break;
            case ERaidPhase::Extracting:
                Label = f"DEFEND - {int(Obj.ExtractionSecondsRemaining)}s";
                break;
            case ERaidPhase::Extracted:
                Label = "EXTRACTED";
                break;
            case ERaidPhase::Failed:
                Label = "PARTY WIPED";
                break;
        }
        ObjectiveText.SetText(FText::FromString(Label));
    }
}
