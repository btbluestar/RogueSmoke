// RaidPlayerController.as
// Player input, wired once for every hero variant (Controller = the player; Pawn = the body).
// Uses the AngelscriptEnhancedInput pattern: create an EnhancedInputComponent, push it, add the
// mapping context, bind actions. Movement forwards to the pawn; ability inputs are resolved to
// gameplay tags (via URogueInputConfig) and routed through the pawn's Server_ActivateAbilityInput
// so GAS activates the matching granted ability authoritatively.
//
// SETUP: make a BP_RaidPlayerController child, assign the mapping context, move action, look action,
// and input config, and set it as the PlayerControllerClass in BP_RaidGameMode.
class ARaidPlayerController : APlayerController
{
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputMappingContext InputMappingContext;

    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction MoveAction;

    // Mouse / right-stick aim. Drives the control rotation, which the third-person boom follows
    // (HeroCharacter bUsePawnControlRotation = true, D-0014).
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction LookAction;

    // Primary fire (LMB). Press/release so full-auto weapons can hold to fire (D-0014 shooter).
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction FireAction;

    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction ReloadAction;

    // Movement (D-0015): jump/double-jump (Space), hold-to-sprint (Shift), crouch/slide (Ctrl).
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction JumpAction;

    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction SprintAction;

    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction CrouchAction;

    // Maps ability input actions -> gameplay input tags (Lyra-style). Each ability action's tag is
    // matched against the abilities the hero was granted from its AbilitySet.
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    URogueInputConfig InputConfig;

    UEnhancedInputComponent EnhancedInput;

    // Upgrade selection: the BP widget (a child of UUpgradeSelectWidget) shown when the server offers
    // a choice. Assign WBP_UpgradeSelect on BP_RaidPlayerController.
    UPROPERTY(EditDefaultsOnly, Category = "Upgrades")
    TSubclassOf<UUpgradeSelectWidget> UpgradeWidgetClass;

    private UUpgradeSelectWidget ActiveUpgradeWidget;

    // Server -> owning client: present a choose-1-of-N upgrade screen with the rolled options.
    UFUNCTION(Client)
    void Client_OfferUpgrades(TArray<URogueUpgradeDef> Options)
    {
        if (UpgradeWidgetClass.Get() == nullptr || Options.Num() == 0 || ActiveUpgradeWidget != nullptr)
            return;

        ActiveUpgradeWidget = Cast<UUpgradeSelectWidget>(WidgetBlueprint::CreateWidget(UpgradeWidgetClass, this));
        if (ActiveUpgradeWidget == nullptr)
            return;

        ActiveUpgradeWidget.OfferedUpgrades = Options;
        ActiveUpgradeWidget.AddToViewport();
        bShowMouseCursor = true;
    }

    // Called by the widget after a pick (it grants + removes itself): return to gameplay.
    void CloseUpgradeScreen()
    {
        ActiveUpgradeWidget = nullptr;
        bShowMouseCursor = false;
    }

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

        if (LookAction != nullptr)
            EnhancedInput.BindAction(LookAction, ETriggerEvent::Triggered,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleLook"));

        if (FireAction != nullptr)
        {
            EnhancedInput.BindAction(FireAction, ETriggerEvent::Started,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleFireStarted"));
            EnhancedInput.BindAction(FireAction, ETriggerEvent::Completed,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleFireStopped"));
        }

        if (ReloadAction != nullptr)
            EnhancedInput.BindAction(ReloadAction, ETriggerEvent::Started,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleReload"));

        if (JumpAction != nullptr)
        {
            EnhancedInput.BindAction(JumpAction, ETriggerEvent::Started,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleJump"));
            EnhancedInput.BindAction(JumpAction, ETriggerEvent::Completed,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleStopJump"));
        }

        if (SprintAction != nullptr)
        {
            EnhancedInput.BindAction(SprintAction, ETriggerEvent::Started,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleSprintOn"));
            EnhancedInput.BindAction(SprintAction, ETriggerEvent::Completed,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleSprintOff"));
        }

        if (CrouchAction != nullptr)
        {
            EnhancedInput.BindAction(CrouchAction, ETriggerEvent::Started,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleCrouchPressed"));
            EnhancedInput.BindAction(CrouchAction, ETriggerEvent::Completed,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleCrouchReleased"));
        }

        // Bind every ability action; the shared handler resolves which by the source action's tag.
        if (InputConfig != nullptr)
        {
            for (const FRogueInputAction& Entry : InputConfig.AbilityInputActions)
            {
                if (Entry.InputAction != nullptr)
                    EnhancedInput.BindAction(Entry.InputAction, ETriggerEvent::Started,
                        FEnhancedInputActionHandlerDynamicSignature(this, n"HandleAbilityInput"));
            }
        }
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

    // Aim look: feed the control rotation on the pawn. The boom follows it (over-the-shoulder).
    // Sign/inversion (e.g. mouse Y) is handled by Negate modifiers in the IMC, Lyra/template-style.
    UFUNCTION()
    private void HandleLook(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero == nullptr)
            return;

        FVector2D LookAxis = ActionValue.GetAxis2D();
        Hero.AddControllerYawInput(LookAxis.X);
        Hero.AddControllerPitchInput(LookAxis.Y);
    }

    UFUNCTION()
    private void HandleFireStarted(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.Server_SetWantsToFire(true);
    }

    UFUNCTION()
    private void HandleFireStopped(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.Server_SetWantsToFire(false);
    }

    UFUNCTION()
    private void HandleReload(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.Server_RequestReload();
    }

    UFUNCTION()
    private void HandleJump(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.DoJump();
    }

    UFUNCTION()
    private void HandleStopJump(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.DoStopJump();
    }

    UFUNCTION()
    private void HandleSprintOn(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.SetSprint(true);
    }

    UFUNCTION()
    private void HandleSprintOff(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.SetSprint(false);
    }

    UFUNCTION()
    private void HandleCrouchPressed(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.CrouchPressed();
    }

    UFUNCTION()
    private void HandleCrouchReleased(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.CrouchReleased();
    }

    UFUNCTION()
    private void HandleAbilityInput(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero == nullptr || InputConfig == nullptr)
            return;

        FGameplayTag InputTag = InputConfig.FindTagForAction(SourceAction);
        if (InputTag.IsValid())
            Hero.Server_ActivateAbilityInput(InputTag);
    }
}
