// HeroCharacter.as
// Base hero: top-down camera (D-0005). Subclasses pick the ability (Vanguard = taunt,
// Bombardier = barrage). MVP arch §6.
//
// INPUT: this fork build does not expose Enhanced Input *binding* to script (only the
// legacy BindAction), so wire input in the hero BP and call the BlueprintCallable entry
// points below:
//   - IA_PrimaryAbility (Started)  -> OnPrimaryAbilityPressed()
//   - IA_Move (Triggered)          -> DoMove(ActionValue.Axis2D)
// IMPORTANT: also add the Input Mapping Context (IMC_Default) to the player on BeginPlay
// in the BP — if that step is missing, NO input reaches the pawn (the usual "nothing
// happens" cause).
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

    // Tunable stat block (health/armor/speed/etc). Self-applies MoveSpeed on BeginPlay and
    // replicates current health/shield for HUDs. Inherited by Vanguard/Bombardier.
    UPROPERTY(DefaultComponent)
    UStatsComponent Stats;

    // Call from the BP's IA_PrimaryAbility (Started/Triggered) event.
    UFUNCTION(BlueprintCallable)
    void OnPrimaryAbilityPressed()
    {
        ActivatePrimary();
    }

    // Call from the BP's IA_Move (Triggered) event, passing the action value's 2D axis.
    UFUNCTION(BlueprintCallable)
    void DoMove(FVector2D Axis)
    {
        AddMovementInput(FVector(1.0, 0.0, 0.0), Axis.Y);        // top-down: world +X = "up"
        AddMovementInput(FVector(0.0, 1.0, 0.0), Axis.X);
    }

    // Overridden per kit.
    void ActivatePrimary() {}

    // Apply a chosen upgrade server-side (authoritative — upgrades change combat math
    // that runs on the server). Called by UpgradeSelectWidget on the owning client.
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
    // Server-RPC the unowned objective directly). D-0010.
    UFUNCTION(Server)
    void Server_CallExtraction(ARaidObjective Objective)
    {
        if (Objective != nullptr)
            Objective.CallExtraction();
    }
}
