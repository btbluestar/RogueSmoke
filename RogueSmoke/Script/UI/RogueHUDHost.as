// RogueHUDHost.as
// The Game-layer activatable that hosts the HUD. Two CommonUI jobs:
//  1. Stay active for the whole raid so that, when every menu above it pops, the input-config
//     arbitration falls back to THIS widget — which answers "Game mode, capture the mouse".
//     (CommonUI doesn't restore the previous config on deactivation; it asks the new foremost
//     active widget. No persistent Game-layer activatable = stuck in Menu mode forever.)
//  2. Never steal focus or eat back actions (the HUD is not a screen you can close).
//
// The actual HUD (URogueHUDWidget, a plain UUserWidget — its Tick-driven refresh stays as-is)
// is created by the PC from its BP class ref and parented in via SetHUD().
class URogueHUDHost : UCommonActivatableWidget
{
    default bAutoActivate = true;            // active the moment it's pushed, stays active
    default bIsBackHandler = false;          // Esc/B over the HUD is not "close the HUD"
    default bSupportsActivationFocus = false; // the HUD never takes keyboard/gamepad focus

    private UOverlay Root;
    private bool bBuilt = false;

    UFUNCTION(BlueprintOverride)
    void OnInitialized()
    {
        if (bBuilt)
            return;
        Root = Cast<UOverlay>(ConstructWidget(UOverlay::StaticClass()));
        if (Root == nullptr)
            return;
        SetRootWidget(Root);
        bBuilt = true;
    }

    void SetHUD(UUserWidget HUDWidget)
    {
        if (Root == nullptr || HUDWidget == nullptr)
            return;
        UOverlaySlot HUDSlot = Root.AddChildToOverlay(HUDWidget);
        HUDSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
        HUDSlot.SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
    }

    // Shooter baseline: game input, mouse captured.
    UFUNCTION(BlueprintOverride)
    FUIInputConfig GetDesiredInputConfig() const
    {
        FUIInputConfig Config;
        Config.InputMode = ECommonInputMode::Game;
        Config.MouseCaptureMode = EMouseCaptureMode::CapturePermanently;
        Config.bHideCursorDuringViewportCapture = true;
        return Config;
    }
}
