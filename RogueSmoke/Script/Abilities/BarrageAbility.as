// BarrageAbility.as
// The PAYOFF half of the signature synergy (D-0008). Radial damage that rewards density:
// more enemies = more total damage, and Clustered enemies (set by Taunt) eat a bonus
// multiplier on top. Neither ability requires the other, but together they're far stronger.
class UBarrageAbilityComponent : UAbilityComponent
{
    default Cooldown = 6.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float Radius = 600.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float BaseDamage = 40.0;

    // The synergy payoff: clustered enemies take this multiple of damage.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float ClusterBonusMultiplier = 2.0;

    // MVP: land on the owner. Real impl: override GetActivationLocation() to return the aim reticle hit.
    void ExecuteOnServer(FVector Location) override
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
            return;

        int HitCount = Combat.CountEnemiesInSphere(Location, Radius);
        float Dealt = Combat.ApplyRadialDamage(
            Location, Radius, BaseDamage, ClusterBonusMultiplier, GetOwner());

        Print(f"BARRAGE hit {HitCount} enemies for {Dealt} total", 2.0);
    }

    void PlayCosmetics(FVector Location) override
    {
        Print("BARRAGE: AoE detonation", 2.0);
    }

    float GetDebugRadius() const override { return Radius; }
}
