// LobbyGameMode.as
// Server-only rules for the pre-run lobby + hero select (ARCHITECTURE §3). The host opens the
// lobby map as a listen server; joiners connect by IP (LAN / direct-connect MVP). Everyone
// picks a hero (pick = part of readying up, RoR2-style; duplicates blocked so the synergy
// pairing stays legible), then the host starts a short countdown and the party ServerTravels
// into the raid — KEEPING the connections (unlike OpenLevel, which would drop clients).
//
// Set as the GameMode Override in L_Lobby's World Settings (pure-script class, no BP needed:
// the hero PAWN classes are wired on BP_RaidGamemode, not here — the lobby only deals in
// roster indexes).

class ALobbyGameMode : AGameModeBase
{
    // Reuse the run GameState/PlayerState so lobby picks replicate the same way run state does
    // and the classes stay consistent across the lobby -> raid transition.
    default GameStateClass = ARaidGameState;
    default PlayerStateClass = ARoguePlayerState;
    default PlayerControllerClass = ALobbyPlayerController;
    default DefaultPawnClass = ASpectatorPawn;   // UI-only map; no body needed

    // Where START RAID sends everyone.
    UPROPERTY(EditDefaultsOnly, Category = "Lobby")
    FString RaidMapPath = "/Game/Levels/RaidArena";

    // DRG drop-pod beat: a short shared countdown between START and the actual travel.
    UPROPERTY(EditDefaultsOnly, Category = "Lobby")
    float LaunchCountdownSeconds = 3.0;

    private bool bLaunching = false;

    // A pick is valid if it's a real roster index no OTHER player has claimed (duplicates
    // blocked). Server-side validation for the lobby PC's Server_SelectHero RPC.
    bool TrySelectHero(APlayerController Player, int HeroIndex)
    {
        if (!HasAuthority() || Player == nullptr || !RogueHeroes::IsValidIndex(HeroIndex))
            return false;

        ARoguePlayerState MyPS = Cast<ARoguePlayerState>(Player.PlayerState);
        if (MyPS == nullptr)
            return false;

        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS != nullptr)
        {
            for (APlayerState BasePS : GS.PlayerArray)
            {
                ARoguePlayerState Other = Cast<ARoguePlayerState>(BasePS);
                if (Other != nullptr && Other != MyPS && Other.SelectedHeroIndex == HeroIndex)
                    return false;   // claimed by a teammate
            }
        }

        MyPS.SetSelectedHeroIndex(HeroIndex);
        return true;
    }

    // Everyone connected has picked a hero and toggled ready.
    bool AreAllPlayersReady() const
    {
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS == nullptr || GS.PlayerArray.Num() == 0)
            return false;

        for (APlayerState BasePS : GS.PlayerArray)
        {
            ARoguePlayerState PS = Cast<ARoguePlayerState>(BasePS);
            if (PS == nullptr || !PS.bLobbyReady || PS.SelectedHeroIndex < 0)
                return false;
        }
        return true;
    }

    // Host pressed START RAID: begin the shared countdown, then travel the whole party.
    UFUNCTION(BlueprintCallable, Category = "Lobby")
    void RequestStartRaid()
    {
        if (!HasAuthority() || bLaunching || !AreAllPlayersReady())
            return;

        bLaunching = true;
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS != nullptr)
            GS.RaidLaunchAt = Gameplay::GetTimeSeconds() + LaunchCountdownSeconds;

        System::SetTimer(this, n"TravelToRaid", LaunchCountdownSeconds, false);
        Print(f"[Lobby] launch in {LaunchCountdownSeconds}s", 3.0);
    }

    UFUNCTION()
    private void TravelToRaid()
    {
        UWorld LobbyWorld = GetWorld();
        if (LobbyWorld != nullptr)
            LobbyWorld.ServerTravel(RaidMapPath, true, false);
    }

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
