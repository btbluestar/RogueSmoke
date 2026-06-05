// Upgrade_ChainDetonation.as
// A SYNERGY upgrade (D-0008 / MVP arch §7): deepens the taunt->barrage combo by making
// the barrage hit clustered groups harder AND widening its radius so it catches more of
// the knot the taunt made. This is the kind of mid-run choice that grows a synergy (GDD §6.2).
class UUpgrade_ChainDetonation : UUpgradeEffect
{
    default Rarity = 2;

    void Apply(AHeroCharacter Hero) override
    {
        // Component ::Get accessor is fork-generated — verify the exact form in-editor.
        UBarrageAbilityComponent Barrage = UBarrageAbilityComponent::Get(Hero);
        if (Barrage == nullptr)
            return;

        Barrage.ClusterBonusMultiplier += 1.0;   // bigger payoff on clustered groups
        Barrage.Radius += 150.0;                  // easier to catch the knot the taunt made
    }
}
