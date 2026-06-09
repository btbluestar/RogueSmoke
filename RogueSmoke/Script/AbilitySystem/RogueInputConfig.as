// RogueInputConfig.as
// Maps Enhanced Input actions to gameplay input tags. AngelScript port of Lyra's ULyraInputConfig.
// The controller binds each AbilityInputAction so pressing it activates the granted ability whose
// AbilitySet InputTag matches. NativeInputActions (e.g. Move) are bound directly by the controller.

struct FRogueInputAction
{
    UPROPERTY(EditDefaultsOnly)
    UInputAction InputAction;

    UPROPERTY(EditDefaultsOnly, meta = (Categories = "InputTag"))
    FGameplayTag InputTag;
}

class URogueInputConfig : UDataAsset
{
    // Bound directly by the controller (movement, look, etc.) — not ability-driven.
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    TArray<FRogueInputAction> NativeInputActions;

    // Pressing one of these activates the ability granted with the matching InputTag.
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    TArray<FRogueInputAction> AbilityInputActions;

    UInputAction FindAbilityInputActionForTag(const FGameplayTag InputTag) const
    {
        for (const FRogueInputAction& Action : AbilityInputActions)
        {
            if (Action.InputTag == InputTag)
                return Action.InputAction;
        }
        return nullptr;
    }

    // Reverse lookup used by the controller: which input tag does this ability action carry?
    FGameplayTag FindTagForAction(const UInputAction Action) const
    {
        for (const FRogueInputAction& Entry : AbilityInputActions)
        {
            if (Entry.InputAction == Action)
                return Entry.InputTag;
        }
        return FGameplayTag();
    }
}
