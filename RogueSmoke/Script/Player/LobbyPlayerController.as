// LobbyPlayerController.as
// Player's seat in the pre-run lobby: shows the lobby/hero-select UI locally and routes the
// pick + ready intent to the server (client intent -> Server_ RPC -> validate server-side,
// CODING_STANDARDS §4.2). The validated result lands on the replicated ARoguePlayerState,
// which every client's lobby UI reads.
class ALobbyPlayerController : APlayerController
{
    private URogueUILayout Layout;

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        if (!IsLocalController())
            return;

        // CommonUI: layout root + Menu-layer push; the lobby's input config owns the cursor.
        Layout = Cast<URogueUILayout>(WidgetBlueprint::CreateWidget(URogueUILayout, this));
        if (Layout == nullptr)
            return;
        Layout.AddToViewport();
        UCommonActivatableWidget Lobby = Layout.PushToLayer(ERogueUILayer::Menu, ULobbyWidget);
        if (Lobby != nullptr)
            Print("[Lobby] lobby shown", 3.0);
    }

    // Local entry point for the UI: stash the pick for the post-travel respawn (the
    // GameInstance subsystem survives ServerTravel; PlayerStates don't), then ask the server.
    void SelectHero(int HeroIndex)
    {
        URaidSessionSubsystem Session = URaidSessionSubsystem::Get();
        if (Session != nullptr)
            Session.LocalSelectedHeroIndex = HeroIndex;
        Server_SelectHero(HeroIndex);
    }

    UFUNCTION(Server)
    void Server_SelectHero(int HeroIndex)
    {
        ALobbyGameMode GameMode = Cast<ALobbyGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.TrySelectHero(this, HeroIndex);   // validates: real index, not claimed
    }

    UFUNCTION(Server)
    void Server_SetReady(bool bReady)
    {
        ARoguePlayerState PS = Cast<ARoguePlayerState>(PlayerState);
        if (PS == nullptr)
            return;
        // Ready requires a pick — picking IS part of readying up (RoR2-style).
        PS.SetLobbyReady(bReady && PS.SelectedHeroIndex >= 0);
    }

    // Debug (host only): pick Vanguard, ready up, and launch — drives the full lobby -> travel ->
    // hero-embodiment chain from the console / a headless -ExecCmds boot without clicking the UI.
    UFUNCTION(Exec)
    void LobbyForceStart()
    {
        SelectHero(0);
        Server_SetReady(true);
        ALobbyGameMode GameMode = Cast<ALobbyGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.RequestStartRaid();
    }
}
