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
// Follow-ups (Design/hud_mockup.html): squad list, ability cooldown slots.
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
    private UTextBlock Crosshair;
    private UTextBlock HitMarker;
    private UTextBlock ResultBanner;   // big VICTORY/DEFEAT on run end
    private UTextBlock ResultHint;      // replay hint under the banner
    private UTextBlock TimerText;       // run clock, top-center under the objective

    // Full results panel (stats table + buttons): the PC pushes it onto the Menu layer
    // ResultsDelaySeconds after the phase resolves, so the banner lands first and the
    // replicated stats settle. The HUD only owns the timer + banner handoff.
    private bool bResultsShown = false;
    private float ResultElapsed = 0.0;
    const float ResultsDelaySeconds = 2.5;

    // Join/leave toast: PlayerArray is replicated, so polling its size works on every machine
    // (mandatory listen-server feedback; no RPC needed).
    private UTextBlock ToastText;
    private int LastPlayerCount = -1;
    private float ToastRemaining = 0.0;
    const float ToastSeconds = 4.0;

    const float HitMarkerDuration = 0.12;   // seconds the hitmarker stays lit after a confirmed hit

    private AHeroCharacter Hero;
    private bool bBuilt = false;

    // Off-screen edge indicators: a pool of arrow glyphs reused each tick (teal = teammate, red = elite).
    // They mark where an off-screen ally or threat is, clamped to the screen border and rotated to point at it.
    private TArray<UTextBlock> EdgeMarkers;   // rotating arrow glyphs
    private TArray<UTextBlock> EdgeLabels;    // distance readout, sits just inside each arrow (unrotated)
    const int MaxEdgeMarkers = 8;
    const float EdgeMargin = 56.0;     // layout-space inset from the screen border
    const float EdgeArrowScale = 1.8;  // arrows are tiny at the default font size, so scale them up
    const float EdgeLabelInset = 30.0; // how far inward (toward centre) the distance label sits from the arrow

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

        // Crosshair: a centered '+' marking where fire is aimed (screen-center, D-0014 shooter). Its
        // render scale blooms with hipfire/movement and tightens on focus (RefreshCrosshair).
        Crosshair = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        Crosshair.SetText(FText::FromString("+"));
        AddChild(Crosshair, FAnchors(0.5, 0.5), FVector2D(0.5, 0.5), FVector2D(0.0, 0.0), FVector2D(), true);

        // Hitmarker: a centered 'X' flashed briefly on a confirmed enemy hit. Collapsed until then.
        HitMarker = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        HitMarker.SetText(FText::FromString("X"));
        HitMarker.SetColorAndOpacity(FSlateColor(Danger));
        HitMarker.SetRenderScale(FVector2D(1.4, 1.4));
        HitMarker.SetVisibility(ESlateVisibility::Collapsed);
        AddChild(HitMarker, FAnchors(0.5, 0.5), FVector2D(0.5, 0.5), FVector2D(0.0, 0.0), FVector2D(), true);

        // Objective banner: top-center.
        ObjectiveText = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        AddChild(ObjectiveText, FAnchors(0.5, 0.0), FVector2D(0.5, 0.0), FVector2D(0.0, 28.0), FVector2D(), true);

        // Run result: a big centered VICTORY/DEFEAT shown when the run resolves, with a replay hint just
        // under it. Both collapsed until then (RefreshResultBanner reads the replicated ERunPhase).
        ResultBanner = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        ResultBanner.SetRenderScale(FVector2D(3.2, 3.2));
        ResultBanner.SetVisibility(ESlateVisibility::Collapsed);
        AddChild(ResultBanner, FAnchors(0.5, 0.5), FVector2D(0.5, 0.5), FVector2D(0.0, -60.0), FVector2D(), true);

        ResultHint = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        ResultHint.SetText(FText::FromString("Open the ~ console and type  RaidRestart  to play again"));
        ResultHint.SetVisibility(ESlateVisibility::Collapsed);
        AddChild(ResultHint, FAnchors(0.5, 0.5), FVector2D(0.5, 0.5), FVector2D(0.0, 20.0), FVector2D(), true);

        // Run clock: just under the objective banner, top-center.
        TimerText = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        AddChild(TimerText, FAnchors(0.5, 0.0), FVector2D(0.5, 0.0), FVector2D(0.0, 56.0), FVector2D(), true);

        // Join/leave toast: under the run clock.
        ToastText = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
        ToastText.SetColorAndOpacity(FSlateColor(Accent));
        ToastText.SetVisibility(ESlateVisibility::Collapsed);
        AddChild(ToastText, FAnchors(0.5, 0.0), FVector2D(0.5, 0.0), FVector2D(0.0, 84.0), FVector2D(), true);

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

        // Edge-indicator pool: top-left anchored (absolute layout-space positioning), centred on their slot,
        // collapsed until a target needs one. The glyph points right at 0deg so atan2 maps straight to its angle.
        for (int i = 0; i < MaxEdgeMarkers; ++i)
        {
            UTextBlock Marker = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
            if (Marker == nullptr)
                continue;
            Marker.SetText(FText::FromString("▶"));
            Marker.SetRenderScale(FVector2D(EdgeArrowScale, EdgeArrowScale));
            Marker.SetVisibility(ESlateVisibility::Collapsed);
            AddChild(Marker, FAnchors(0.0, 0.0), FVector2D(0.5, 0.5), FVector2D(), FVector2D(), true);
            EdgeMarkers.Add(Marker);

            // Paired distance label (unrotated, sits inboard of the arrow).
            UTextBlock LabelText = Cast<UTextBlock>(ConstructWidget(UTextBlock::StaticClass()));
            if (LabelText == nullptr)
                continue;
            LabelText.SetVisibility(ESlateVisibility::Collapsed);
            AddChild(LabelText, FAnchors(0.0, 0.0), FVector2D(0.5, 0.5), FVector2D(), FVector2D(), true);
            EdgeLabels.Add(LabelText);
        }
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
        RefreshEdgeIndicators();
        RefreshCrosshair();
        RefreshHitMarker();
        RefreshResultBanner();
        RefreshTimer();
        RefreshToast();
    }

    // "Player joined/left" feedback off the replicated PlayerArray size.
    private void RefreshToast()
    {
        if (ToastText == nullptr)
            return;

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        int Count = (GS != nullptr) ? GS.PlayerArray.Num() : 0;

        if (LastPlayerCount < 0)
        {
            LastPlayerCount = Count;   // first sample: no toast for the initial roster
        }
        else if (Count != LastPlayerCount)
        {
            FString Message = (Count > LastPlayerCount)
                ? f"Player joined — {Count} in squad"
                : f"Player left — {Count} in squad";
            ToastText.SetText(FText::FromString(Message));
            ToastText.SetVisibility(ESlateVisibility::HitTestInvisible);
            ToastRemaining = ToastSeconds;
            LastPlayerCount = Count;
        }

        if (ToastRemaining > 0.0)
        {
            ToastRemaining -= Gameplay::GetWorldDeltaSeconds();
            if (ToastRemaining <= 0.0)
                ToastText.SetVisibility(ESlateVisibility::Collapsed);
        }
    }

    // Run clock, mm:ss. Counts up from RunStartTime; freezes at RunEndTime once the run resolves.
    private void RefreshTimer()
    {
        if (TimerText == nullptr)
            return;

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS == nullptr || GS.RunStartTime <= 0.0)
        {
            TimerText.SetText(FText());
            return;
        }

        float Now = (GS.RunEndTime > 0.0) ? GS.RunEndTime : Gameplay::GetTimeSeconds();
        int Total = int(Math::Max(0.0, Now - GS.RunStartTime));
        int Mins = Total / 60;
        int Secs = Total % 60;
        FString Pad = Secs < 10 ? "0" : "";
        TimerText.SetText(FText::FromString(f"{Mins}:{Pad}{Secs}"));
    }

    // Show a big VICTORY/DEFEAT when the run resolves (the run-level phase set by RunManager::EndRun),
    // plus a replay hint. Collapsed while the run is in progress.
    private void RefreshResultBanner()
    {
        if (ResultBanner == nullptr)
            return;

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        ERunPhase Phase = (GS != nullptr) ? GS.Phase : ERunPhase::None;

        if (Phase == ERunPhase::Victory && !bResultsShown)
            ShowResult("VICTORY", Accent);
        else if (Phase == ERunPhase::Defeat && !bResultsShown)
            ShowResult("DEFEAT", Danger);
        else if (Phase != ERunPhase::Victory && Phase != ERunPhase::Defeat
                 && ResultBanner.GetVisibility() != ESlateVisibility::Collapsed)
        {
            ResultBanner.SetVisibility(ESlateVisibility::Collapsed);
            if (ResultHint != nullptr)
                ResultHint.SetVisibility(ESlateVisibility::Collapsed);
        }

        // Escalate banner -> full results panel once the delay has passed (once per run).
        if (Phase == ERunPhase::Victory || Phase == ERunPhase::Defeat)
        {
            ResultElapsed += Gameplay::GetWorldDeltaSeconds();
            if (!bResultsShown && ResultElapsed >= ResultsDelaySeconds)
            {
                bResultsShown = true;
                ARaidPlayerController PC = Cast<ARaidPlayerController>(GetOwningPlayer());
                if (PC != nullptr)
                    PC.ShowResultsScreen();   // CommonUI: PC pushes it onto the Menu layer
                HideResultBanner();
            }
        }
        else
        {
            ResultElapsed = 0.0;
        }
    }

    // The results panel owns the presentation once it's up — drop the interim banner + hint.
    void HideResultBanner()
    {
        bResultsShown = true;
        if (ResultBanner != nullptr)
            ResultBanner.SetVisibility(ESlateVisibility::Collapsed);
        if (ResultHint != nullptr)
            ResultHint.SetVisibility(ESlateVisibility::Collapsed);
    }

    private void ShowResult(FString Text, FLinearColor Color)
    {
        ResultBanner.SetText(FText::FromString(Text));
        ResultBanner.SetColorAndOpacity(FSlateColor(Color));
        if (ResultBanner.GetVisibility() == ESlateVisibility::Collapsed)
            ResultBanner.SetVisibility(ESlateVisibility::HitTestInvisible);
        if (ResultHint != nullptr && ResultHint.GetVisibility() == ESlateVisibility::Collapsed)
            ResultHint.SetVisibility(ESlateVisibility::HitTestInvisible);
    }

    // Crosshair bloom proxy: tighten when focusing, widen while moving (hip-fire). Full heat-driven bloom
    // needs weapon heat replicated to the owning client (server-only today) — a follow-up; the host reads
    // its own weapon directly, so this is exact for the host and a good approximation for remote clients.
    private void RefreshCrosshair()
    {
        if (Crosshair == nullptr || Hero == nullptr)
            return;

        float Scale = 1.0;
        if (Hero.bFocusing)
            Scale = 0.6;
        else if (Hero.CharacterMovement != nullptr && Hero.CharacterMovement.Velocity.Size() > 50.0)
            Scale = 1.6;
        Crosshair.SetRenderScale(FVector2D(Scale, Scale));
    }

    // Flash the hitmarker for HitMarkerDuration after the hero records a confirmed hit (set locally on the
    // owning client in Multicast_FireFX).
    private void RefreshHitMarker()
    {
        if (HitMarker == nullptr || Hero == nullptr)
            return;

        bool bShow = Hero.LastHitConfirmTime > 0.0
            && (Gameplay::GetTimeSeconds() - Hero.LastHitConfirmTime) < HitMarkerDuration;
        ESlateVisibility Want = bShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed;
        if (HitMarker.GetVisibility() != Want)
            HitMarker.SetVisibility(Want);
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

    // Drive the pooled edge arrows: one per off-screen teammate (teal) then off-screen elite (red), each
    // clamped to the screen border and rotated to point at its target. Everything here is local + cosmetic.
    private void RefreshEdgeIndicators()
    {
        if (EdgeMarkers.Num() == 0)
            return;

        APlayerController PC = GetOwningPlayer();
        // GetViewportSize is in raw pixels; canvas slots live in DPI-scaled layout space, so divide by the
        // DPI scale and use WidgetLayout's DPI-corrected projection to keep both in the same coordinate space.
        FVector2D ViewSize = WidgetLayout::GetViewportSize();
        float DPI = WidgetLayout::GetViewportScale();
        if (PC == nullptr || PC.PlayerCameraManager == nullptr || DPI <= 0.0 || ViewSize.X <= 0.0)
        {
            HideMarkersFrom(0);
            return;
        }

        FVector2D LayoutSize = ViewSize / DPI;
        FVector2D Center = LayoutSize * 0.5;
        float HalfW = Math::Max(16.0, Center.X - EdgeMargin);
        float HalfH = Math::Max(16.0, Center.Y - EdgeMargin);

        FVector CamLoc = PC.PlayerCameraManager.GetCameraLocation();
        FRotator CamRot = PC.PlayerCameraManager.GetCameraRotation();
        // Distance is measured from the pawn (more meaningful than the trailing 3rd-person camera).
        FVector PlayerLoc = (Hero != nullptr) ? Hero.GetActorLocation() : CamLoc;

        int Used = 0;

        // Teammates first (teal): other heroes that are currently off-screen.
        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);
        for (AHeroCharacter H : Heroes)
        {
            if (Used >= EdgeMarkers.Num())
                break;
            if (H == nullptr || H == Hero)
                continue;
            if (PlaceEdgeMarker(Used, H.GetActorLocation(), Accent, Center, HalfW, HalfH, LayoutSize, CamLoc, CamRot, PlayerLoc, PC))
                Used++;
        }

        // Then elites (red): off-screen threats worth turning toward.
        TArray<AEliteEnemyBase> Elites;
        GetAllActorsOfClass(Elites);
        for (AEliteEnemyBase E : Elites)
        {
            if (Used >= EdgeMarkers.Num())
                break;
            if (E == nullptr)
                continue;
            if (PlaceEdgeMarker(Used, E.GetActorLocation(), Danger, Center, HalfW, HalfH, LayoutSize, CamLoc, CamRot, PlayerLoc, PC))
                Used++;
        }

        HideMarkersFrom(Used);
    }

    // Position marker [Index] for a world target. Returns false (and consumes no marker) when the target is
    // already on-screen; true when an edge arrow was placed.
    private bool PlaceEdgeMarker(int Index, FVector TargetLoc, FLinearColor Color,
        FVector2D Center, float HalfW, float HalfH, FVector2D LayoutSize,
        FVector CamLoc, FRotator CamRot, FVector PlayerLoc, APlayerController PC)
    {
        FVector2D ScreenPos;
        bool bProjected = WidgetLayout::ProjectWorldLocationToWidgetPosition(PC, TargetLoc, ScreenPos, false);

        bool bOnScreen = bProjected
            && ScreenPos.X >= EdgeMargin && ScreenPos.X <= LayoutSize.X - EdgeMargin
            && ScreenPos.Y >= EdgeMargin && ScreenPos.Y <= LayoutSize.Y - EdgeMargin;
        if (bOnScreen)
            return false;

        // Screen-space direction from centre toward the target. ProjectWorldLocationToWidgetPosition gives a
        // valid (mirrored) point only when in front; for targets behind the camera, derive the direction from
        // the camera-local offset instead (UnrotateVector: X=forward, Y=right, Z=up).
        FVector2D Dir;
        if (bProjected)
        {
            Dir = ScreenPos - Center;
        }
        else
        {
            FVector Local = CamRot.UnrotateVector(TargetLoc - CamLoc);
            Dir = FVector2D(Local.Y, -Local.Z);   // screen Y points down
        }

        if (Dir.IsNearlyZero())
            Dir = FVector2D(0.0, 1.0);   // directly behind: point straight down
        Dir.Normalize();

        // Scale the unit direction out to whichever inset border it hits first.
        float TX = (Math::Abs(Dir.X) > 0.0001) ? HalfW / Math::Abs(Dir.X) : 1000000.0;
        float TY = (Math::Abs(Dir.Y) > 0.0001) ? HalfH / Math::Abs(Dir.Y) : 1000000.0;
        FVector2D EdgePos = Center + Dir * Math::Min(TX, TY);

        UTextBlock Marker = EdgeMarkers[Index];
        Marker.SetColorAndOpacity(FSlateColor(Color));
        Marker.SetVisibility(ESlateVisibility::HitTestInvisible);
        Marker.SetRenderTransformAngle(Math::Atan2(Dir.Y, Dir.X) * 57.2957795);   // rad -> deg

        UCanvasPanelSlot Slot = WidgetLayout::SlotAsCanvasSlot(Marker);
        if (Slot != nullptr)
            Slot.SetPosition(EdgePos);

        // Distance label, just inboard of the arrow (unrotated so the number stays upright).
        if (Index < EdgeLabels.Num() && EdgeLabels[Index] != nullptr)
        {
            UTextBlock LabelText = EdgeLabels[Index];
            int Meters = int((TargetLoc - PlayerLoc).Size() / 100.0);   // cm -> m
            LabelText.SetText(FText::FromString(f"{Meters}m"));
            LabelText.SetColorAndOpacity(FSlateColor(Color));
            LabelText.SetVisibility(ESlateVisibility::HitTestInvisible);
            UCanvasPanelSlot LabelSlot = WidgetLayout::SlotAsCanvasSlot(LabelText);
            if (LabelSlot != nullptr)
                LabelSlot.SetPosition(EdgePos - Dir * EdgeLabelInset);
        }
        return true;
    }

    private void HideMarkersFrom(int FromIndex)
    {
        for (int i = FromIndex; i < EdgeMarkers.Num(); ++i)
        {
            if (EdgeMarkers[i] != nullptr && EdgeMarkers[i].GetVisibility() != ESlateVisibility::Collapsed)
                EdgeMarkers[i].SetVisibility(ESlateVisibility::Collapsed);
            if (i < EdgeLabels.Num() && EdgeLabels[i] != nullptr && EdgeLabels[i].GetVisibility() != ESlateVisibility::Collapsed)
                EdgeLabels[i].SetVisibility(ESlateVisibility::Collapsed);
        }
    }
}
