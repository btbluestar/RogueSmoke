// EscapeMenuWidget.as
// In-run escape menu. NOTHING PAUSES (genre convention + listen server — teammates keep
// fighting), so this is just an overlay: RESUME / RETURN TO LOBBY (host) / LEAVE TO MENU /
// QUIT. Opened by RaidPlayerController (Esc or P, or the RaidPause console command).
//
// Runtime-built tree (RogueHUDWidget pattern).
class UEscapeMenuWidget : UUserWidget
{
    private UCanvasPanel Root;
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

        UTextBlock Title = RogueUITheme::MakeText(this, "PAUSED (the raid is not)", RogueUITheme::TextPrimary, 1.8);
        UCanvasPanelSlot TitleSlot = Root.AddChildToCanvas(Title);
        TitleSlot.SetAnchors(FAnchors(0.5, 0.3));
        TitleSlot.SetAlignment(FVector2D(0.5, 0.5));
        TitleSlot.SetAutoSize(true);

        UVerticalBox Stack = Cast<UVerticalBox>(ConstructWidget(UVerticalBox::StaticClass()));
        UCanvasPanelSlot StackSlot = Root.AddChildToCanvas(Stack);
        StackSlot.SetAnchors(FAnchors(0.5, 0.42));
        StackSlot.SetAlignment(FVector2D(0.5, 0.0));
        StackSlot.SetAutoSize(true);

        AddButton(Stack, "  RESUME  ", n"HandleResume", RogueUITheme::Accent);

        APlayerController PC = GetOwningPlayer();
        if (PC != nullptr && PC.HasAuthority())
            AddButton(Stack, "  RETURN TO LOBBY (squad)  ", n"HandleLobby", RogueUITheme::TextPrimary);

        AddButton(Stack, "  LEAVE TO MENU  ", n"HandleLeave", RogueUITheme::TextPrimary);
        AddButton(Stack, "  QUIT  ", n"HandleQuit", RogueUITheme::TextDim);
    }

    private void AddButton(UVerticalBox Stack, FString Label, FName Handler, FLinearColor Color)
    {
        UButton Button = RogueUITheme::MakeTextButton(this, Label, Color);
        if (Button == nullptr)
            return;
        Button.OnClicked.AddUFunction(this, Handler);
        UVerticalBoxSlot ButtonSlot = Stack.AddChildToVerticalBox(Button);
        ButtonSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 10.0));
        ButtonSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Center);
    }

    UFUNCTION()
    private void HandleResume()
    {
        ARaidPlayerController PC = Cast<ARaidPlayerController>(GetOwningPlayer());
        if (PC != nullptr)
            PC.ClosePauseMenu();
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
