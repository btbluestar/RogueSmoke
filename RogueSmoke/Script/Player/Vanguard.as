// Vanguard.as — the SETUP hero (D-0008). Carries the taunt that pulls + marks Clustered.
class AVanguard : AHeroCharacter
{
    UPROPERTY(DefaultComponent)
    UTauntAbilityComponent Taunt;

    void ActivatePrimary() override
    {
        Taunt.TryActivate();
    }
}
