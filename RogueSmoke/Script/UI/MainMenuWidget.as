// MainMenuWidget.as
// Script CONTROLLER for the main menu (CODING_STANDARDS §6: logic in script, UMG layout in
// a BP subclass). The BP widget's Host / Join / Quit buttons call these BlueprintCallable
// functions; the text box passes its string into OnJoinClicked. Mirrors the existing
// UUpgradeSelectWidget pattern.
//
// CommonUI: this is a UCommonActivatableWidget so it can be pushed onto a
// UCommonActivatableWidgetStack and participate in CommonUI focus/back-action routing. Use
// CommonButtonBase for the buttons; the project's Back action is mapped (Esc / FaceButton_Right).
//
// SETUP: make WBP_MainMenu (child of this), lay out the buttons + an IP text box, and wire
// their OnClicked events to these functions. Show it from the menu map's PlayerController.

class UMainMenuWidget : UCommonActivatableWidget
{
    // Activate as soon as it's added to a stack/viewport so focus + input config apply.
    default bAutoActivate = true;

    // The lobby map opened (as a listen server) when hosting. Set on the BP if your map name
    // differs. Just the asset name is fine if it's unique in the project.
    UPROPERTY(EditAnywhere, Category = "Session")
    FName LobbyMap = n"L_Lobby";

    // Pre-filled join target so a local 2-PIE / same-machine test is one click.
    UPROPERTY(EditAnywhere, Category = "Session")
    FString DefaultJoinAddress = "127.0.0.1";

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
