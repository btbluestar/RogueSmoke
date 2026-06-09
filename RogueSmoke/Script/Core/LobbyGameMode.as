// LobbyGameMode.as
// Server-only rules for the pre-run lobby (ARCHITECTURE §3). The host opens the lobby map as
// a listen server; joiners connect by IP (LAN / direct-connect for the MVP — D net-backend).
// When the host starts, we ServerTravel the whole party into the raid map, which KEEPS the
// existing connections (unlike OpenLevel, which would drop clients).
//
// SETUP: make BP_LobbyGameMode (child of this), assign PlayerControllerClass =
// BP_RaidPlayerController (or a lobby-specific PC), set it as the GameMode Override in the
// Lobby map's World Settings.

class ALobbyGameMode : AGameModeBase
{
    // Reuse the run GameState so the lobby UI can read GetConnectedPlayerCount() and so the
    // class is consistent across the lobby->raid transition.
    default GameStateClass = ARaidGameState;

    // Where "Start" sends everyone. Set to the raid map's path on the BP.
    UPROPERTY(EditDefaultsOnly, Category = "Lobby")
    FString RaidMapPath = "/Game/Maps/L_Raid";

    // Host pressed Start. Authority-gated travel that carries connected clients along.
    UFUNCTION(BlueprintCallable, Category = "Lobby")
    void StartRaid()
    {
        if (!HasAuthority())
            return;

        UWorld LobbyWorld = GetWorld();
        if (LobbyWorld != nullptr)
            LobbyWorld.ServerTravel(RaidMapPath, true, false);
    }

    // Player joined/left — hook for updating lobby UI or enforcing a max party size.
    // (Max party size is still an open decision; see DECISIONS.md "Still open".)
    UFUNCTION(BlueprintOverride)
    void OnPostLogin(APlayerController NewPlayer)
    {
        // Compute outside Print(): Print is a development-only call that compiles out in
        // shipping, and the fork forbids non-const calls inside its arguments.
        int Connected = GetNumPlayers();
        Print(f"[Lobby] Player joined ({Connected} connected)", 4.0);
    }

    UFUNCTION(BlueprintOverride)
    void OnLogout(AController ExitingController)
    {
        Print("[Lobby] Player left", 4.0);
    }
}
