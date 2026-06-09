// LobbyWidget.as
// Script CONTROLLER for the pre-run lobby (CODING_STANDARDS §6). The BP widget shows the
// connected-player count and a Start button; only the host's Start does anything. Joiners
// see the count update as players connect (it reads the replicated GameState).
//
// CommonUI: a UCommonActivatableWidget (pushable onto a UCommonActivatableWidgetStack), so the
// menu->lobby flow is just stack pushes and the Back action pops cleanly.
//
// SETUP: make WBP_Lobby (child of this), bind a text block to GetConnectedPlayerCount() and
// the Start button's OnClicked to OnStartClicked(). Optionally hide Start unless
// IsLocalPlayerHost() returns true.

class ULobbyWidget : UCommonActivatableWidget
{
    // Activate as soon as it's added to a stack/viewport so focus + input config apply.
    default bAutoActivate = true;

    // Replicated player count — valid on host AND clients (reads GameState.PlayerArray).
    UFUNCTION(BlueprintPure, Category = "Lobby")
    int GetConnectedPlayerCount() const
    {
        ARaidGameState GameState = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GameState != nullptr)
            return GameState.GetConnectedPlayerCount();
        return 0;
    }

    // True only on the listen-server host. Use it to show/enable the Start button.
    UFUNCTION(BlueprintPure, Category = "Lobby")
    bool IsLocalPlayerHost() const
    {
        APlayerController PC = GetOwningPlayer();
        return PC != nullptr && PC.HasAuthority();
    }

    // Host pressed Start -> travel the whole party into the raid. The GameMode only exists on
    // the server, so a client calling this is a harmless no-op.
    UFUNCTION(BlueprintCallable, Category = "Lobby")
    void OnStartClicked()
    {
        if (!IsLocalPlayerHost())
            return;

        ALobbyGameMode GameMode = Cast<ALobbyGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.StartRaid();
    }
}
