// RaidPlayerController.as
// Player input, wired once for every hero variant (Controller = the player; Pawn = the body).
// Uses the AngelscriptEnhancedInput pattern: create an EnhancedInputComponent, push it, add
// the mapping context, bind actions, and forward intent to the possessed AHeroCharacter.
//
// SETUP: make a BP_RaidPlayerController child, assign the Input assets, and set it as the
// PlayerControllerClass in BP_RaidGameMode. Then no hero BP needs any input wiring.
class ARaidPlayerController : APlayerController
{
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputMappingContext InputMappingContext;

    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction MoveAction;

    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction PrimaryAbilityAction;

    UEnhancedInputComponent EnhancedInput;

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        EnhancedInput = UEnhancedInputComponent::Create(this);
        PushInputComponent(EnhancedInput);

        UEnhancedInputLocalPlayerSubsystem InputSystem = UEnhancedInputLocalPlayerSubsystem::Get(this);
        if (InputSystem != nullptr && InputMappingContext != nullptr)
            InputSystem.AddMappingContext(InputMappingContext, 0, FModifyContextOptions());

        if (MoveAction != nullptr)
            EnhancedInput.BindAction(MoveAction, ETriggerEvent::Triggered,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleMove"));

        if (PrimaryAbilityAction != nullptr)
            EnhancedInput.BindAction(PrimaryAbilityAction, ETriggerEvent::Started,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandlePrimaryAbility"));
    }

    AHeroCharacter GetHero() const
    {
        return Cast<AHeroCharacter>(GetControlledPawn());
    }

    // Enhanced Input handlers carry 4 params (value, elapsed, triggered, source action).
    UFUNCTION()
    private void HandleMove(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.DoMove(ActionValue.GetAxis2D());
    }

    UFUNCTION()
    private void HandlePrimaryAbility(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.OnPrimaryAbilityPressed();
    }
}
