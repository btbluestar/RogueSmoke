// GA_Barrage.as
// PAYOFF half of the signature synergy (D-0008), now a GAS ability. Radial damage that rewards
// density: Clustered enemies (set by Taunt) eat a bonus multiplier. Upgrade-tunable magnitudes
// (Chain Detonation) come from URogueCombatSet attributes so a GameplayEffect can grow them.
class UGA_Barrage : UGA_RogueAbility
{
    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float Radius = 600.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float BaseDamage = 40.0;

    // The synergy payoff: clustered enemies take this multiple of damage.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float ClusterBonusMultiplier = 2.0;

    UFUNCTION(BlueprintOverride)
    void ActivateAbility()
    {
        if (!CommitAbility())
        {
            EndAbility();
            return;
        }

        if (HasAuthority())
        {
            UCombatSubsystem Combat = UCombatSubsystem::Get();
            if (Combat != nullptr)
            {
                FVector Location = GetActivationLocation();

                // Upgrades (e.g. Chain Detonation GE) add to these via URogueCombatSet.
                float EffectiveRadius = Radius + GetCombatAttribute(n"BarrageRadiusBonus");
                float EffectiveCluster = ClusterBonusMultiplier + GetCombatAttribute(n"BarrageClusterBonus");

                int HitCount = Combat.CountEnemiesInSphere(Location, EffectiveRadius);
                float Dealt = Combat.ApplyRadialDamage(
                    Location, EffectiveRadius, BaseDamage, EffectiveCluster, GetAvatarActorFromActorInfo());

                Print(f"BARRAGE hit {HitCount} enemies for {Dealt} total", 2.0);
            }
        }

        EndAbility();
    }
}
