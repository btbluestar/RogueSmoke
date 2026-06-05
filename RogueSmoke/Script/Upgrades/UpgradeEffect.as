// UpgradeEffect.as
// Base for roguelike upgrades (MVP arch §7). Applied when a player picks the upgrade.
// For the slice these are plain UObject subclasses in a pool; promote to UDataAsset once
// the pool grows so designers author upgrades as assets (ARCHITECTURE §4.3 / §5).
class UUpgradeEffect : UObject
{
    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    FText DisplayName;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    FText Description;

    UPROPERTY(EditDefaultsOnly, Category = "Upgrade")
    int Rarity = 1;        // tiers per GDD §6.1

    // Applied server-side when the player picks this upgrade. Override per upgrade.
    void Apply(AHeroCharacter Hero) {}
}
