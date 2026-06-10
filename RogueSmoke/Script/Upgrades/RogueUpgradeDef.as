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

    // The short stat line shown big on the upgrade card ("+25 Max Health"). Description stays
    // the longer flavor/detail text under it.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    FText ValueText;

    // Card icon. Optional — the card falls back to a rarity-tinted block until art exists.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    UTexture2D Icon;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    int Rarity = 1;        // tiers per GDD §6.1

    // Synergy upgrades (cross-player interactions — see GLOSSARY) come ONLY from the mini-boss
    // chest, never from level-up offers (UpgradeLoop concept, 2026-06-11). Level offers roll the
    // non-synergy pool; the chest rolls only entries with this set.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    bool bSynergyUpgrade = false;

    // The effect applied when the player picks this upgrade. Infinite-duration; modifies attributes.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    TSubclassOf<UGameplayEffect> Effect;
}
