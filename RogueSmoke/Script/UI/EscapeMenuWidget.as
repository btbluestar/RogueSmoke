// EscapeMenuWidget.as
// In-run escape menu. NOTHING PAUSES (genre convention + listen server — teammates keep
// fighting), so this is just an overlay: RESUME / RETURN TO LOBBY (host) / LEAVE TO MENU /
// QUIT. Opened by RaidPlayerController (Esc or P, or the RaidPause console command), which
// pushes it onto the Menu layer stack.
//
// CommonUI: an activatable, back-handling screen — Esc/gamepad-B (the configured back action)
// pops it via the default back behavior (deactivate -> the stack removes it), and its input
// config (Menu) is what shows the cursor; the HUD host underneath restores Game mode when this
// deactivates. No manual cursor code anywhere.
class UEscapeMenuWidget : UCommonActivatableWidget
{
    default bIsBackHandler = true;   // Esc/B = RESUME (default back behavior deactivates)

    private UCanvasPanel Root;
    private UButton ResumeButton;
    private bool bBuilt = false;

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

        UBorder Backdrop = RogueUITheme::MakePanel(this, FLinearColor(0.0, 0.0, 0.0), 0.0);
        Backdrop.SetBrushColor(FLinearColor(0.0, 0.0, 0.0, 0.62));
        UCanvasPanelSlot BackdropSlot = Root.AddChildToCanvas(Backdrop);
        BackdropSlot.SetAnchors(FAnchors(0.0, 0.0, 1.0, 1.0));
        BackdropSlot.SetOffsets(FMargin(0.0, 0.0, 0.0, 0.0));

        UTextBlock Title = RogueUITheme::MakeText(this, "PAUSED (the raid is not)", RogueUITheme::TextPrimary(), 1.8);
        UCanvasPanelSlot TitleSlot = Root.AddChildToCanvas(Title);
        TitleSlot.SetAnchors(FAnchors(0.5, 0.3));
        TitleSlot.SetAlignment(FVector2D(0.5, 0.5));
        TitleSlot.SetAutoSize(true);

        UVerticalBox Stack = Cast<UVerticalBox>(ConstructWidget(UVerticalBox::StaticClass()));
        UCanvasPanelSlot StackSlot = Root.AddChildToCanvas(Stack);
        StackSlot.SetAnchors(FAnchors(0.5, 0.42));
        StackSlot.SetAlignment(FVector2D(0.5, 0.0));
        StackSlot.SetAutoSize(true);

        ResumeButton = AddButton(Stack, "  RESUME  ", n"HandleResume", RogueUITheme::Accent());

        APlayerController PC = GetOwningPlayer();
        if (PC != nullptr && PC.HasAuthority())
            AddButton(Stack, "  RETURN TO LOBBY (squad)  ", n"HandleLobby", RogueUITheme::TextPrimary());

        AddButton(Stack, "  LEAVE TO MENU  ", n"HandleLeave", RogueUITheme::TextPrimary());
        AddButton(Stack, "  QUIT  ", n"HandleQuit", RogueUITheme::TextDim());
    }

    private UButton AddButton(UVerticalBox Stack, FString Label, FName Handler, FLinearColor Color)
    {
        UButton Button = RogueUITheme::MakeTextButton(this, Label, Color);
        if (Button == nullptr)
            return nullptr;
        Button.OnClicked.AddUFunction(this, Handler);
        UVerticalBoxSlot ButtonSlot = Stack.AddChildToVerticalBox(Button);
        ButtonSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 10.0));
        ButtonSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
        return Button;
    }

    // Menus own the cursor; the HUD host underneath restores Game capture on close.
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
        return ResumeButton;
    }

    // Any close path (RESUME click, Esc/B back action, travel) lands here — tell the PC so its
    // toggle bookkeeping stays correct.
    UFUNCTION(BlueprintOverride)
    void OnDeactivated()
    {
        ARaidPlayerController PC = Cast<ARaidPlayerController>(GetOwningPlayer());
        if (PC != nullptr)
        {
            PC.NotifyPauseMenuClosed();
            // CommonUI's own restore of the underlying widget's input config is eaten by an
            // editor-only focus guard at this exact frame (focus is on our dying button);
            // deterministically re-apply whatever is topmost now.
            PC.RestoreTopmostInputConfig();
        }
    }

    UFUNCTION()
    private void HandleResume()
    {
        DeactivateWidget();   // the stack pops us; OnDeactivated notifies the PC
    }

    // Host: take the whole squad back to hero select (ServerTravel keeps clients connected).
    UFUNCTION()
    private void HandleLobby()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.TravelToLobby();
    }

    // Local: open the front-end map. On a remote client this disconnects from the host; the
    // host doing it ends the session for everyone (it IS the server) — acceptable, explicit.
    UFUNCTION()
    private void HandleLeave()
    {
        Gameplay::OpenLevel(n"L_MainMenu");
    }

    UFUNCTION()
    private void HandleQuit()
    {
        System::QuitGame(nullptr, EQuitPreference::Quit, false);
    }
}
