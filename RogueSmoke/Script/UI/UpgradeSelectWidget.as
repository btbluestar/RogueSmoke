// UpgradeSelectWidget.as
// Minimal choose-1-of-N upgrade screen (MVP arch §5 / §9). This is the script CONTROLLER;
// the visual widget stays a BP asset that subclasses this and calls ChooseUpgrade() from
// its buttons (CODING_STANDARDS §6: logic in script, UMG layout in Blueprint).
class UUpgradeSelectWidget : UUserWidget
{
    // The upgrades offered this pick. Populated by the run/upgrade flow (or BP defaults for now).
    // Each is a URogueUpgradeDef carrying the GameplayEffect applied on selection.
    UPROPERTY(EditAnywhere, Category = "Upgrades")
    TArray<URogueUpgradeDef> OfferedUpgrades;

    // Called from the BP widget's buttons. Routes the choice through the owning pawn's
    // server RPC so the upgrade's GameplayEffect applies authoritatively.
    UFUNCTION(BlueprintCallable)
    void ChooseUpgrade(int Index)
    {
        if (Index < 0 || Index >= OfferedUpgrades.Num())
            return;

        AHeroCharacter Hero = Cast<AHeroCharacter>(GetOwningPlayerPawn());
        if (Hero != nullptr)
            Hero.Server_ApplyUpgrade(OfferedUpgrades[Index]);

        RemoveFromParent();
    }
}
