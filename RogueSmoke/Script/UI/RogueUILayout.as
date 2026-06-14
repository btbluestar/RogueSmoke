// RogueUILayout.as
// The per-local-player ROOT layout (Lyra's PrimaryGameLayout pattern, reimplemented small).
// This is the ONLY widget that ever calls AddToViewport; every screen is a
// UCommonActivatableWidget pushed onto one of its layer stacks, so CommonUI's action router —
// not ad-hoc bShowMouseCursor toggles — arbitrates input mode, cursor, back actions and focus
// from whichever widget is foremost-active.
//
// Layers (bottom -> top), each a UCommonActivatableWidgetStack:
//   Game     — the persistent HUD host (always active; restores Game input mode when menus pop)
//   GameMenu — in-run overlays the game keeps running under (upgrade pick)
//   Menu     — full-screen/front-of-everything screens (main menu, lobby, results, escape menu)
//
// Owned by the local PlayerController (created in BeginPlay, like the old direct widgets were).

enum ERogueUILayer
{
    Game,
    GameMenu,
    Menu
}

class URogueUILayout : UUserWidget
{
    private UOverlay Root;
    private UCommonActivatableWidgetStack GameStack;
    private UCommonActivatableWidgetStack GameMenuStack;
    private UCommonActivatableWidgetStack MenuStack;
    private bool bBuilt = false;

    UFUNCTION(BlueprintOverride)
    void OnInitialized()
    {
        BuildLayout();
    }

    private void BuildLayout()
    {
        if (bBuilt)
            return;

        Root = Cast<UOverlay>(ConstructWidget(UOverlay::StaticClass()));
        if (Root == nullptr)
            return;
        SetRootWidget(Root);
        bBuilt = true;

        GameStack = MakeLayerStack();
        GameMenuStack = MakeLayerStack();
        MenuStack = MakeLayerStack();
    }

    private UCommonActivatableWidgetStack MakeLayerStack()
    {
        UCommonActivatableWidgetStack Stack = Cast<UCommonActivatableWidgetStack>(
            ConstructWidget(UCommonActivatableWidgetStack::StaticClass()));
        if (Stack == nullptr)
            return nullptr;
        UOverlaySlot LayerSlot = Root.AddChildToOverlay(Stack);
        LayerSlot.SetHorizontalAlignment(EHorizontalAlignment::HAlign_Fill);
        LayerSlot.SetVerticalAlignment(EVerticalAlignment::VAlign_Fill);
        return Stack;
    }

    // Push a screen class onto a layer; the stack creates + activates it and pops it again when
    // it deactivates (so screens close themselves with DeactivateWidget(), never RemoveFromParent).
    UCommonActivatableWidget PushToLayer(ERogueUILayer Layer, TSubclassOf<UCommonActivatableWidget> WidgetClass)
    {
        UCommonActivatableWidgetStack Stack = GetStack(Layer);
        if (Stack == nullptr || WidgetClass.Get() == nullptr)
            return nullptr;
        // Via the C++ shim URogueUIStatics (binds as namespace RogueUI:: — the fork strips the
        // U prefix and "Statics" suffix): the stack's own BP_AddWidget is private, invisible
        // to script.
        return RogueUI::PushWidgetToStack(Stack, WidgetClass);
    }

    private UCommonActivatableWidgetStack GetStack(ERogueUILayer Layer) const
    {
        if (Layer == ERogueUILayer::Game)
            return GameStack;
        if (Layer == ERogueUILayer::GameMenu)
            return GameMenuStack;
        return MenuStack;
    }

    // Re-apply the input config of whatever is topmost-active right now. Needed when a screen
    // CLOSES: CommonUI's own fallback re-apply is silently dropped by an editor-builds-only
    // focus guard (slate focus dangles on the just-destroyed button at that exact frame) and is
    // never retried — see RogueUI::ApplyDesiredInputConfig. Screens OPENING apply normally.
    void ReapplyTopmostConfig()
    {
        RogueUI::ApplyDesiredInputConfig(GetTopmostActiveWidget());
    }

    // Topmost layer first; skips a still-displayed widget that has already deactivated (a
    // closing screen sits in its stack until the pop transition finishes).
    private UCommonActivatableWidget GetTopmostActiveWidget() const
    {
        UCommonActivatableWidget Candidate = MenuStack != nullptr ? MenuStack.GetActiveWidget() : nullptr;
        if (Candidate != nullptr && Candidate.IsActivated())
            return Candidate;
        Candidate = GameMenuStack != nullptr ? GameMenuStack.GetActiveWidget() : nullptr;
        if (Candidate != nullptr && Candidate.IsActivated())
            return Candidate;
        Candidate = GameStack != nullptr ? GameStack.GetActiveWidget() : nullptr;
        if (Candidate != nullptr && Candidate.IsActivated())
            return Candidate;
        return nullptr;
    }
}
