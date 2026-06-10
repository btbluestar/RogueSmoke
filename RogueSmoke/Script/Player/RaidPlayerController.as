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

    // Light ADS / focus (hold RMB): zoom in + tighten spread (D-0014).
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction FocusAction;

    // Escape menu (Esc / P). Note: Esc also stops PIE in-editor — P is the PIE-friendly key.
    UPROPERTY(EditDefaultsOnly, Category = "Input")
    UInputAction PauseAction;

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

    // In-game HUD (crosshair / health / ammo / objective). A child of URogueHUDWidget; assign
    // WBP_HUD on BP_RaidPlayerController. Created once for the local player in BeginPlay.
    UPROPERTY(EditDefaultsOnly, Category = "UI")
    TSubclassOf<URogueHUDWidget> HUDWidgetClass;

    private URogueHUDWidget HUDWidget;

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
        // HUD belongs to the local player only (the owning client / the host's own controller).
        if (IsLocalController() && HUDWidgetClass.Get() != nullptr && HUDWidget == nullptr)
        {
            HUDWidget = Cast<URogueHUDWidget>(WidgetBlueprint::CreateWidget(HUDWidgetClass, this));
            if (HUDWidget != nullptr)
                HUDWidget.AddToViewport();
        }

        // Hero select: players start as spectators; send the lobby pick (stashed in the
        // GameInstance subsystem, which survives ServerTravel) so the GameMode embodies us.
        // -1 (no lobby / direct PIE) falls back to the first/default hero server-side.
        if (IsLocalController())
        {
            URaidSessionSubsystem Session = URaidSessionSubsystem::Get();
            int HeroChoice = (Session != nullptr) ? Session.LocalSelectedHeroIndex : -1;
            Server_SetHeroChoice(HeroChoice);
        }

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

        if (FocusAction != nullptr)
        {
            EnhancedInput.BindAction(FocusAction, ETriggerEvent::Started,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleFocusOn"));
            EnhancedInput.BindAction(FocusAction, ETriggerEvent::Completed,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandleFocusOff"));
        }

        if (PauseAction != nullptr)
            EnhancedInput.BindAction(PauseAction, ETriggerEvent::Started,
                FEnhancedInputActionHandlerDynamicSignature(this, n"HandlePause"));

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

    // Client -> server: my hero pick (RogueHeroes index; -1 = none). The GameMode validates and
    // spawns the body — clients never decide outcomes (CODING_STANDARDS §4.2).
    UFUNCTION(Server)
    void Server_SetHeroChoice(int HeroIndex)
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.HandleHeroChoice(this, HeroIndex);
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
    private void HandleFocusOn(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.SetFocus(true);
    }

    UFUNCTION()
    private void HandleFocusOff(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
            Hero.SetFocus(false);
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

    // --- Debug camera: type `RaidDebugCam` in the ~ console during play to toggle ------------------
    // A top-down overhead view of the whole arena so you can watch the loop from above — objective
    // phase ring/label, elite telegraph spheres, fodder waves — then back to the over-shoulder pawn
    // camera. Local/cosmetic on the owning client only; no replication, no gameplay effect.
    private bool bDebugCamActive = false;
    private ACameraActor DebugCam;

    UFUNCTION(Exec)
    void RaidDebugCam()
    {
        if (bDebugCamActive)
        {
            SetViewTargetWithBlend(GetControlledPawn(), 0.25);
            bDebugCamActive = false;
            return;
        }

        if (DebugCam == nullptr)
            DebugCam = Cast<ACameraActor>(SpawnActor(ACameraActor, FVector(0.0, 0.0, 5000.0), FRotator(-90.0, 0.0, 0.0)));

        if (DebugCam != nullptr)
        {
            SetViewTargetWithBlend(DebugCam, 0.25);
            bDebugCamActive = true;
        }
    }

    // --- Debug: type `RaidKillElites` in the ~ console to clear the arena instantly ----------------
    // Nukes every registered enemy (elites + fodder share the combat registry) so you can skip the
    // fight and test the back half of the loop — clear -> upgrade offer -> walk to the pad -> defend
    // timer -> EXTRACTED. Goes through the seam; server-authoritative, so it only does work on the
    // host/listen-server (a remote client typing it is a no-op).
    UFUNCTION(Exec)
    void RaidKillElites()
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
            return;
        AHeroCharacter Hero = GetHero();
        FVector Center = Hero != nullptr ? Hero.GetActorLocation() : FVector();
        float Dealt = Combat.ApplyRadialDamage(Center, 1000000.0, 999999.0, 1.0, Hero);
        Print(f"[Debug] RaidKillElites — dealt {Dealt}", 3.0);
    }

    // --- Replay: type `RaidRestart` in the ~ console to reload the current level (fresh run + seed) ----
    // Shown as the hint under the VICTORY/DEFEAT banner so a finished run is replayable without leaving PIE.
    UFUNCTION(Exec)
    void RaidRestart()
    {
        FString Level = Gameplay::GetCurrentLevelName();
        if (!Level.IsEmpty())
        {
            Print(f"[Debug] RaidRestart — reloading {Level}", 2.0);
            Gameplay::OpenLevel(FName(Level));
        }
    }

    // Debug: force the run result so you can check the VICTORY/DEFEAT banner + replay flow instantly,
    // without clearing the arena. Host/authority only (sets the replicated run phase).
    UFUNCTION(Exec)
    void RaidWin() { ForceRunPhase(ERunPhase::Victory); }

    UFUNCTION(Exec)
    void RaidLose() { ForceRunPhase(ERunPhase::Defeat); }

    // Debug: pop the end-of-run results panel immediately (normally the HUD shows it ~2.5s after
    // the phase resolves). Pair with RaidWin/RaidLose to preview either report.
    UFUNCTION(Exec)
    void RaidResults()
    {
        if (HUDWidget != nullptr)
            HUDWidget.ShowResultsScreen();
    }

    // --- Escape menu (Esc / P, or this console command). An overlay — nothing pauses. ---
    private UEscapeMenuWidget PauseMenu;

    UFUNCTION(Exec)
    void RaidPause() { TogglePauseMenu(); }

    UFUNCTION()
    private void HandlePause(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        TogglePauseMenu();
    }

    void TogglePauseMenu()
    {
        if (PauseMenu != nullptr)
        {
            ClosePauseMenu();
            return;
        }
        PauseMenu = Cast<UEscapeMenuWidget>(WidgetBlueprint::CreateWidget(UEscapeMenuWidget, this));
        if (PauseMenu == nullptr)
            return;
        PauseMenu.AddToViewport(20);   // above HUD + results
        bShowMouseCursor = true;
        Print("[Menu] escape menu shown", 2.0);
    }

    void ClosePauseMenu()
    {
        if (PauseMenu == nullptr)
            return;
        PauseMenu.RemoveFromParent();
        PauseMenu = nullptr;
        // Restore cursor only if no other screen needs it (upgrade pick keeps it on).
        bShowMouseCursor = ActiveUpgradeWidget != nullptr;
    }

    private void ForceRunPhase(ERunPhase NewPhase)
    {
        if (!HasAuthority())
            return;
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GS != nullptr)
        {
            GS.Phase = NewPhase;
            Print("[Debug] Forced run phase (check the VICTORY/DEFEAT banner)", 3.0);
        }
    }
}
