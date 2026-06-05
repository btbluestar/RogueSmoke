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

    // Apply a chosen upgrade server-side (authoritative — upgrades change combat math
    // that runs on the server). Called by UpgradeSelectWidget on the owning client.
    // NOTE: NewObject / Cast / TSubclassOf forms are fork-generated — verify in-editor.
    UFUNCTION(Server)
    void Server_ApplyUpgrade(TSubclassOf<UUpgradeEffect> UpgradeClass)
    {
        if (UpgradeClass.Get() == nullptr)
            return;

        UUpgradeEffect Effect = Cast<UUpgradeEffect>(NewObject(this, UpgradeClass.Get()));
        if (Effect != nullptr)
            Effect.Apply(this);
    }

    // Route a remote client's "call extraction" through a player-owned RPC (clients can't
    // Server-RPC the unowned objective directly). The caller passes the objective it found
    // (e.g. an interaction volume), avoiding a world-wide actor lookup. D-0010.
    UFUNCTION(Server)
    void Server_CallExtraction(ARaidObjective Objective)
    {
        if (Objective != nullptr)
            Objective.CallExtraction();
    }
}
