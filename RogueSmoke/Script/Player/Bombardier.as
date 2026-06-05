// Bombardier.as — the PAYOFF hero (D-0008). Carries the barrage that rewards the cluster.
class ABombardier : AHeroCharacter
{
    UPROPERTY(DefaultComponent)
    UBarrageAbilityComponent Barrage;

    void ActivatePrimary() override
    {
        Barrage.TryActivate();
    }
}
