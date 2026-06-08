// HeroCharacter.as
// Base hero: top-down camera (D-0005) + self-contained Enhanced Input. Subclasses pick the
// ability (Vanguard = taunt, Bombardier = barrage). MVP arch §6.
//
// SETUP — in the hero BP (Class Defaults > Input) assign:
//   InputMappingContext  = IMC_Default          (must contain a key->PrimaryAbilityAction mapping)
//   MoveAction           = IA_Move
//   PrimaryAbilityAction = IA_PrimaryAbility     (create this asset; add it to IMC_Default)
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

    // ---- Input assets (assign in the hero BP) ----
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputMappingContext InputMappingContext;

    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction MoveAction;

    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction PrimaryAbilityAction;

    // Register the mapping context for the locally-controlled player.
    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        APlayerController PC = Cast<APlayerController>(GetController());
        if (PC != nullptr && InputMappingContext != nullptr)
        {
            UEnhancedInputLocalPlayerSubsystem InputSystem = UEnhancedInputLocalPlayerSubsystem::Get(PC.GetLocalPlayer());
            if (InputSystem != nullptr)
                InputSystem.AddMappingContext(InputMappingContext, 0);
        }
    }

    // Bind actions. Called automatically when the pawn is possessed by a local player.
    UFUNCTION(BlueprintOverride)
    void SetupPlayerInputComponent(UInputComponent InputComponent)
    {
        UEnhancedInputComponent EnhancedInput = Cast<UEnhancedInputComponent>(InputComponent);
        if (EnhancedInput == nullptr)
            return;

        if (MoveAction != nullptr)
            EnhancedInput.BindAction(MoveAction, ETriggerEvent::Triggered, FEnhancedInputActionHandlerDynamicSignature(this, n"HandleMove"));

        if (PrimaryAbilityAction != nullptr)
            EnhancedInput.BindAction(PrimaryAbilityAction, ETriggerEvent::Started, FEnhancedInputActionHandlerDynamicSignature(this, n"HandlePrimaryAbility"));
    }

    UFUNCTION()
    private void HandleMove(FInputActionValue ActionValue)
    {
        FVector2D Axis = ActionValue.GetAxis2D();
        AddMovementInput(FVector(1.0, 0.0, 0.0), Axis.Y);   // top-down: world +X = "up"
        AddMovementInput(FVector(0.0, 1.0, 0.0), Axis.X);
    }

    UFUNCTION()
    private void HandlePrimaryAbility(FInputActionValue ActionValue)
    {
        OnPrimaryAbilityPressed();
    }

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
