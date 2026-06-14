// RaidSessionSubsystem.as
// The connection seam: how a machine becomes a host or joins one (ARCHITECTURE §2 — the
// GameInstance owns session/connection handling). Lives for the whole process on every
// machine (a GameInstanceSubsystem auto-instantiates; no custom UGameInstance needed).
//
// MVP backend = LAN / direct IP over the built-in NULL online subsystem (D net-backend).
// Hosting opens the lobby map as a listen server; joining is a direct ClientTravel to an
// address. Centralizing it here means swapping to Steam/EOS later touches ONLY this file.
//
// NOTE (fork idiom): the AngelScript bindings for Gameplay::/System:: library calls OMIT the
// WorldContextObject parameter — the world is taken implicitly from the calling object. So
// these methods need no WorldContext argument of their own.

class URaidSessionSubsystem : UGameInstanceSubsystem
{
    // The LOCAL player's hero pick (index into RogueHeroes; -1 = none). Lives here because a
    // GameInstance subsystem survives ServerTravel on each machine, while PlayerStates are
    // rebuilt — the lobby stashes the pick, and after travel RaidPlayerController re-sends it
    // to the server so the raid GameMode spawns the chosen hero.
    int LocalSelectedHeroIndex = -1;

    // Become the host: open the lobby map as a listen server. Standalone -> listen server is
    // handled by OpenLevel's "listen" option.
    UFUNCTION(BlueprintCallable, Category = "Session")
    void HostListenServer(FName LobbyMap)
    {
        Gameplay::OpenLevel(LobbyMap, true, "listen");
    }

    // Join a host by address ("127.0.0.1", a LAN IP, or "host:port"). Direct connect — no
    // session search needed for LAN. The "open <addr>" console command is the canonical client
    // connect; it routes to the local player and avoids APlayerController::ClientTravel, which
    // has no AngelScript-callable overload in this fork.
    UFUNCTION(BlueprintCallable, Category = "Session")
    void JoinByAddress(FString Address)
    {
        if (Address.IsEmpty())
            return;

        System::ExecuteConsoleCommand("open " + Address);
    }
}
