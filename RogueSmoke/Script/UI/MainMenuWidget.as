// MainMenuWidget.as
// The front-end main menu. Genre-standard minimal stack (RoR2 shape): HOST GAME / JOIN GAME /
// QUIT, with JOIN expanding an IP row (direct-connect MVP, D net-backend — no server browser).
// Session actions route through URaidSessionSubsystem so the net backend stays swappable.
//
// Layout is runtime-built (RogueHUDWidget pattern — no UMG asset to maintain). Still a
// UCommonActivatableWidget so it can join a CommonUI stack/back-action flow later.
//
// Shown by AMenuPlayerController on the L_MainMenu map.

class UMainMenuWidget : UCommonActivatableWidget
{
    // Activate as soon as it's added to a stack/viewport so focus + input config apply.
    default bAutoActivate = true;

    // The lobby map opened (as a listen server) when hosting.
    UPROPERTY(EditAnywhere, Category = "Session")
    FName LobbyMap = n"L_Lobby";

    // Pre-filled join target so a local 2-PIE / same-machine test is one click.
    UPROPERTY(EditAnywhere, Category = "Session")
    FString DefaultJoinAddress = "127.0.0.1";

    private UCanvasPanel Root;
    private UHorizontalBox JoinRow;       // hidden until JOIN GAME is clicked
    private UEditableTextBox AddressBox;
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

        // Opaque dark backdrop — this IS the screen, not an overlay.
        UBorder Backdrop = RogueUITheme::MakePanel(this, RogueUITheme::PanelDark, 0.0);
        UCanvasPanelSlot BackdropSlot = Root.AddChildToCanvas(Backdrop);
        BackdropSlot.SetAnchors(FAnchors(0.0, 0.0, 1.0, 1.0));
        BackdropSlot.SetOffsets(FMargin(0.0, 0.0, 0.0, 0.0));

        UTextBlock Title = RogueUITheme::MakeText(this, "ROGUESMOKE", RogueUITheme::Accent, 3.4);
        UCanvasPanelSlot TitleSlot = Root.AddChildToCanvas(Title);
        TitleSlot.SetAnchors(FAnchors(0.12, 0.2));
        TitleSlot.SetAlignment(FVector2D(0.0, 0.5));
        TitleSlot.SetAutoSize(true);

        UTextBlock Tagline = RogueUITheme::MakeText(
            this, "co-op raid roguelike  -  pre-alpha", RogueUITheme::TextDim, 1.0);
        UCanvasPanelSlot TagSlot = Root.AddChildToCanvas(Tagline);
        TagSlot.SetAnchors(FAnchors(0.12, 0.27));
        TagSlot.SetAlignment(FVector2D(0.0, 0.5));
        TagSlot.SetAutoSize(true);

        // Button stack.
        UVerticalBox Stack = Cast<UVerticalBox>(ConstructWidget(UVerticalBox::StaticClass()));
        UCanvasPanelSlot StackSlot = Root.AddChildToCanvas(Stack);
        StackSlot.SetAnchors(FAnchors(0.12, 0.42));
        StackSlot.SetAlignment(FVector2D(0.0, 0.0));
        StackSlot.SetAutoSize(true);

        AddMenuButton(Stack, "  HOST GAME  ", n"HandleHost", RogueUITheme::TextPrimary);
        AddMenuButton(Stack, "  JOIN GAME  ", n"HandleToggleJoin", RogueUITheme::TextPrimary);

        // Join row: IP box + CONNECT, collapsed until JOIN GAME is clicked.
        JoinRow = Cast<UHorizontalBox>(ConstructWidget(UHorizontalBox::StaticClass()));
        JoinRow.SetVisibility(ESlateVisibility::Collapsed);
        UVerticalBoxSlot JoinSlot = Stack.AddChildToVerticalBox(JoinRow);
        JoinSlot.SetPadding(FMargin(0.0, 4.0, 0.0, 12.0));

        AddressBox = Cast<UEditableTextBox>(ConstructWidget(UEditableTextBox::StaticClass()));
        AddressBox.SetText(FText::FromString(DefaultJoinAddress));
        UHorizontalBoxSlot BoxSlot = JoinRow.AddChildToHorizontalBox(AddressBox);
        BoxSlot.SetPadding(FMargin(0.0, 0.0, 8.0, 0.0));

        UButton Connect = RogueUITheme::MakeTextButton(this, " CONNECT ", RogueUITheme::Accent);
        Connect.OnClicked.AddUFunction(this, n"HandleConnect");
        JoinRow.AddChildToHorizontalBox(Connect);

        AddMenuButton(Stack, "  QUIT  ", n"HandleQuit", RogueUITheme::TextDim);
    }

    private void AddMenuButton(UVerticalBox Stack, FString Label, FName Handler, FLinearColor Color)
    {
        UButton Button = RogueUITheme::MakeTextButton(this, Label, Color);
        if (Button == nullptr)
            return;
        Button.OnClicked.AddUFunction(this, Handler);
        UVerticalBoxSlot ButtonSlot = Stack.AddChildToVerticalBox(Button);
        ButtonSlot.SetPadding(FMargin(0.0, 0.0, 0.0, 12.0));
        ButtonSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Left);
    }

    UFUNCTION()
    private void HandleHost() { OnHostClicked(); }

    UFUNCTION()
    private void HandleToggleJoin()
    {
        if (JoinRow == nullptr)
            return;
        bool bHidden = JoinRow.GetVisibility() == ESlateVisibility::Collapsed;
        JoinRow.SetVisibility(bHidden ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }

    UFUNCTION()
    private void HandleConnect()
    {
        FString Address = (AddressBox != nullptr) ? AddressBox.GetText().ToString() : "";
        OnJoinClicked(Address);
    }

    UFUNCTION()
    private void HandleQuit() { OnQuitClicked(); }

    // --- Session actions (BlueprintCallable kept so a BP layout could still reuse them) ---

    UFUNCTION(BlueprintCallable, Category = "Session")
    void OnHostClicked()
    {
        URaidSessionSubsystem Session = URaidSessionSubsystem::Get();
        if (Session != nullptr)
            Session.HostListenServer(LobbyMap);
    }

    // Address comes from the menu's text box. Falls back to DefaultJoinAddress if empty.
    UFUNCTION(BlueprintCallable, Category = "Session")
    void OnJoinClicked(FString Address)
    {
        // Params are const in this fork — copy into a local before defaulting.
        FString Target = Address;
        if (Target.IsEmpty())
            Target = DefaultJoinAddress;

        URaidSessionSubsystem Session = URaidSessionSubsystem::Get();
        if (Session != nullptr)
            Session.JoinByAddress(Target);
    }

    UFUNCTION(BlueprintCallable, Category = "Session")
    void OnQuitClicked()
    {
        System::QuitGame(nullptr, EQuitPreference::Quit, false);
    }
}
