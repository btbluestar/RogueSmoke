// RogueUpgradeDef.as
// A roguelike upgrade, now expressed the GAS way: a designer-authored definition that carries a
// GameplayEffect. Picking the upgrade applies that effect (infinite-duration) to the player's ASC,
// modifying attributes (e.g. Chain Detonation adds to BarrageRadiusBonus / BarrageClusterBonus on
// URogueCombatSet). Replaces the retired UUpgradeEffect.Apply() UObject pattern.
//
// The GameplayEffect itself is content (authored in the editor, no logic) — see CODING_STANDARDS:
// logic in script, content in assets. This data asset is the script-visible handle to it.
class URogueUpgradeDef : UPrimaryDataAsset
{
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    FText DisplayName;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    FText Description;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    int Rarity = 1;        // tiers per GDD §6.1

    // The effect applied when the player picks this upgrade. Infinite-duration; modifies attributes.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    TSubclassOf<UGameplayEffect> Effect;
}
