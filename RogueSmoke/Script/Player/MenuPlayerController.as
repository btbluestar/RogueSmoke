// MenuPlayerController.as
// Local-only UI driver for the front-end menu map: creates the main menu widget, shows the
// cursor, and routes input to UI. No gameplay input, no replication concerns (menu maps are
// standalone until the player hosts/joins).
class AMenuPlayerController : APlayerController
{
    private UMainMenuWidget Menu;

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        if (!IsLocalController())
            return;

        Menu = Cast<UMainMenuWidget>(WidgetBlueprint::CreateWidget(UMainMenuWidget, this));
        if (Menu == nullptr)
            return;
        Menu.AddToViewport();
        bShowMouseCursor = true;
        Print("[Menu] main menu shown", 3.0);
    }
}
