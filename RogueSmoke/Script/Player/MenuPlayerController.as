// MenuPlayerController.as
// Local-only UI driver for the front-end menu map: creates the main menu widget, shows the
// cursor, and routes input to UI. No gameplay input, no replication concerns (menu maps are
// standalone until the player hosts/joins).
class AMenuPlayerController : APlayerController
{
    private URogueUILayout Layout;

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        if (!IsLocalController())
            return;

        // CommonUI: the layout is the only AddToViewport; the menu is pushed onto its Menu
        // layer stack, whose input config (Menu mode) shows the cursor — no manual toggles.
        Layout = Cast<URogueUILayout>(WidgetBlueprint::CreateWidget(URogueUILayout, this));
        if (Layout == nullptr)
            return;
        Layout.AddToViewport();
        UCommonActivatableWidget Menu = Layout.PushToLayer(ERogueUILayer::Menu, UMainMenuWidget);
        if (Menu != nullptr)
            Print("[Menu] main menu shown", 3.0);
    }
}
