// HeroCharacter.as
// Base hero: top-down camera (D-0005) + the primary-ability input hook. Subclasses pick
// the ability (Vanguard = taunt, Bombardier = barrage). MVP arch §6.
class AHeroCharacter : ACharacter
{
    default bReplicates = true;

    UPROPERTY(DefaultComponent)
    USpringArmComponent CameraBoom;
    default CameraBoom.TargetArmLength = 1500.0;                  // top-down (GDD §9)
    default CameraBoom.bUsePawnControlRotation = false;
    default CameraBoom.RelativeRotation = FRotator(-60.0, 0.0, 0.0);

    UPROPERTY(DefaultComponent, Attach = CameraBoom)
    UCameraComponent TopDownCamera;

    // Bind this to an Enhanced Input action on the owning client (IA_PrimaryAbility).
    UFUNCTION()
    void OnPrimaryAbilityPressed()
    {
        ActivatePrimary();
    }

    // Overridden per kit.
    void ActivatePrimary() {}
}
