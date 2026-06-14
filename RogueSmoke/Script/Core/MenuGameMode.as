// MenuGameMode.as
// Rules object for the front-end menu map (L_MainMenu) — there is no gameplay here, just a
// spectator shell that exists so the menu map boots cleanly and the MenuPlayerController can
// put the main menu on screen. Set as the GameMode Override in L_MainMenu's World Settings.
class AMenuGameMode : AGameModeBase
{
    default PlayerControllerClass = AMenuPlayerController;
    default DefaultPawnClass = ASpectatorPawn;
    // Reuse the run GameState class so anything reading ARaidGameState (player count, phase
    // = None in the menu) stays consistent across menu -> lobby -> raid.
    default GameStateClass = ARaidGameState;
}
