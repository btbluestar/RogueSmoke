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

    // The CommonUI root layout (per local player). Screens push onto its layer stacks; nothing
    // else calls AddToViewport.
    private URogueUILayout Layout;

    // Server -> owning client: present THIS player's hand (per-player rolls, D-0019).
    // CurrentStacks[i] = how many copies of Options[i] this player already owns (card "Lv n" text).
    UFUNCTION(Client)
    void Client_OfferUpgrades(TArray<URogueUpgradeDef> Options, TArray<int> CurrentStacks)
    {
        if (Layout == nullptr || Options.Num() == 0)
            return;
        if (ActiveUpgradeWidget != nullptr)
        {
            ActiveUpgradeWidget.Refresh(Options, CurrentStacks);   // reroll replaced the hand
            return;
        }
        ActiveUpgradeWidget = Cast<UUpgradeSelectWidget>(
            Layout.PushToLayer(ERogueUILayer::GameMenu, UUpgradeSelectWidget));
        if (ActiveUpgradeWidget == nullptr)
            return;
        ActiveUpgradeWidget.Setup(Options, CurrentStacks);
    }

    // Server -> owning client: watchdog auto-picked for us — drop the screen.
    UFUNCTION(Client)
    void Client_ForceClosePick()
    {
        if (ActiveUpgradeWidget != nullptr)
        {
            ActiveUpgradeWidget.DeactivateWidget();
            ActiveUpgradeWidget = nullptr;
        }
    }

    // Client -> server: spend one squad reroll on my hand (validated server-side).
    UFUNCTION(Server)
    void Server_RequestReroll()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (GameMode != nullptr)
            GameMode.RequestReroll(this);
    }

    // Called by the widget after a pick (it deactivates itself; the stack pops it).
    void CloseUpgradeScreen()
    {
        ActiveUpgradeWidget = nullptr;
    }

    // Called by closing GameMenu/Menu screens: re-apply the input config of whatever is now
    // topmost. CommonUI's own fallback apply is silently dropped by an editor-builds-only
    // focus guard at the exact frame a screen pops (see RogueUI::ApplyDesiredInputConfig).
    void RestoreTopmostInputConfig()
    {
        if (Layout != nullptr)
            Layout.ReapplyTopmostConfig();
    }

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        // UI root (local player only): the CommonUI layout is the single AddToViewport; the HUD
        // rides the Game layer inside an always-active host whose input config (Game mode,
        // capture) is what menus fall back to when they pop.
        if (IsLocalController() && Layout == nullptr)
        {
            Layout = Cast<URogueUILayout>(WidgetBlueprint::CreateWidget(URogueUILayout, this));
            if (Layout != nullptr)
            {
                Layout.AddToViewport();
                URogueHUDHost HUDHost = Cast<URogueHUDHost>(
                    Layout.PushToLayer(ERogueUILayer::Game, URogueHUDHost));
                if (HUDHost != nullptr && HUDWidgetClass.Get() != nullptr && HUDWidget == nullptr)
                {
                    HUDWidget = Cast<URogueHUDWidget>(WidgetBlueprint::CreateWidget(HUDWidgetClass, this));
                    if (HUDWidget != nullptr)
                        HUDHost.SetHUD(HUDWidget);
                }
            }
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
        {
            Hero.SetFireHeldForFacing(true);   // owner-side mirror: facing must not wait for the RPC
            Hero.Server_SetWantsToFire(true);
        }
    }

    UFUNCTION()
    private void HandleFireStopped(FInputActionValue ActionValue, float32 ElapsedTime, float32 TriggeredTime, UInputAction SourceAction)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero != nullptr)
        {
            Hero.SetFireHeldForFacing(false);
            Hero.Server_SetWantsToFire(false);
        }
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
    private int KillElitesRetries = 0;

    UFUNCTION(Exec)
    void RaidKillElites()
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
            return;
        AHeroCharacter Hero = GetHero();
        FVector Center = Hero != nullptr ? Hero.GetActorLocation() : FVector();
        int Live = Combat.CountEnemiesInSphere(Center, 1000000.0);
        if (Live == 0 && KillElitesRetries < 15)
        {
            // Boot-time friendliness: -ExecCmds fires before the arena populates — poll briefly.
            KillElitesRetries++;
            System::SetTimer(this, n"RaidKillElites", 1.0, false);
            return;
        }
        KillElitesRetries = 15;   // armed once: typing it later never re-polls into a future wave
        float Dealt = Combat.ApplyRadialDamage(Center, 1000000.0, 999999.0, 1.0, Hero);
        Print(f"[Debug] RaidKillElites — {Live} live, dealt {Dealt}", 3.0);
    }

    // --- Debug: weapon-upgrade seam smoke (WEAPON_UPGRADES_PLAN.md). Fires one synthetic upgraded
    // shot (pierce 1, chain 2, guaranteed burn+poison) from the hero's muzzle at the nearest live
    // enemy, prints the seam result, then re-reads the victim 2s later to prove the DoTs ticked.
    // Host-only (the seam no-ops on clients). ---
    private AEliteEnemyBase SmokeTarget;
    private float SmokeStartHealth = 0.0;
    private int SmokeRetries = 0;

    UFUNCTION(Exec)
    void WeaponSmoke()
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        AHeroCharacter Hero = GetHero();
        if (Combat == nullptr || Hero == nullptr)
        {
            RetrySmoke("waiting for hero");
            return;
        }

        TArray<AEliteEnemyBase> Enemies;
        GetAllActorsOfClass(Enemies);
        AEliteEnemyBase Target = nullptr;
        float BestDistSq = 1.0e18;
        for (AEliteEnemyBase Enemy : Enemies)
        {
            if (Enemy == nullptr || Enemy.Health == nullptr || Enemy.Health.IsDead())
                continue;
            float DistSq = Enemy.GetActorLocation().DistSquared(Hero.GetActorLocation());
            if (DistSq < BestDistSq)
            {
                BestDistSq = DistSq;
                Target = Enemy;
            }
        }
        if (Target == nullptr)
        {
            RetrySmoke("waiting for enemies");
            return;
        }

        SmokeTarget = Target;
        SmokeStartHealth = Target.Health.Health;

        FWeaponShotParams Shot;
        Shot.Damage = 10.0;
        Shot.PierceCount = 1;
        Shot.ChainCount = 2;
        Shot.ChainRadius = 100000.0;   // arena-wide so the arcs always find the ring elites
        Shot.BurnChance = 1.0;         // guaranteed procs so the check is deterministic
        Shot.PoisonChance = 1.0;

        FVector From = Hero.GetMuzzleLocation();
        FVector Dir = (Target.GetActorLocation() - From).GetSafeNormal();
        FHitscanResult Result = Combat.FireWeaponShot(From, From + Dir * 20000.0, Shot, Hero);
        Print(f"[WeaponSmoke] shot: hitEnemy={Result.bHitEnemy} dealt={Result.DamageDealt} extraEnemies={Result.ExtraEnemiesHit} startHP={SmokeStartHealth}", 8.0);

        System::SetTimer(this, n"WeaponSmokeReport", 2.0, false);
    }

    // Boot-time friendliness: -ExecCmds fires before the hero/enemies spawn, so poll for up to 30s.
    private void RetrySmoke(FString Why)
    {
        if (SmokeRetries >= 30)
        {
            Print(f"[WeaponSmoke] gave up: {Why}", 8.0);
            return;
        }
        SmokeRetries++;
        System::SetTimer(this, n"WeaponSmoke", 1.0, false);
    }

    UFUNCTION()
    private void WeaponSmokeReport()
    {
        if (SmokeTarget == nullptr || SmokeTarget.Health == nullptr)
        {
            Print("[WeaponSmoke] dot check: target gone (died/level change)", 5.0);
            return;
        }
        float HealthNow = SmokeTarget.Health.Health;
        bool bBurn = SmokeTarget.Health.HasActiveDot(ERogueDotType::Burn);
        bool bPoison = SmokeTarget.Health.HasActiveDot(ERogueDotType::Poison);
        Print(f"[WeaponSmoke] dot check: health {SmokeStartHealth} -> {HealthNow} burnActive={bBurn} poisonActive={bPoison}", 8.0);
    }

    // --- Debug: live movement-feel tuning (the convergence tool for the feel pass). -------------
    // `MoveTune`                  — dump every knob as a paste-ready defaults block.
    // `MoveTune <Param> <Value>`  — set one knob live (host PIE; names per the dump output).
    // Host-only: on a listen server host = authority + local, so one apply covers prediction
    // and simulation. A remote client typing it would desync its prediction — refused.
    // --- Movement debug overlay (local screen only, default ON): anim Direction/speed plus the
    // movement keys currently held. Toggle with the `MoveDebug` console command. ---
    private bool bMoveDebugOverlay = true;

    UFUNCTION(Exec)
    void MoveDebug()
    {
        bMoveDebugOverlay = !bMoveDebugOverlay;
        Print(bMoveDebugOverlay ? "[MoveDebug] overlay ON" : "[MoveDebug] overlay OFF", 3.0);
    }

    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (!bMoveDebugOverlay || !IsLocalController())
            return;
        AHeroCharacter Hero = GetHero();
        if (Hero == nullptr || Hero.Mesh == nullptr)
            return;

        URogueHeroAnimInstance Anim = Cast<URogueHeroAnimInstance>(Hero.Mesh.GetAnimInstance());
        FString Line1 = "[MoveDebug] no URogueHeroAnimInstance on the mesh";
        if (Anim != nullptr)
        {
            Line1 = f"[MoveDebug] Direction {int(Anim.Direction)} deg | Speed {int(Anim.GroundSpeed)}"
                  + f" | StrafeY {int(Anim.StrafeSpeed)} | Rate x{Anim.PlayRate}";
        }

        FString Keys = "";
        if (IsInputKeyDown(FKey(n"W")))                Keys += "W ";
        if (IsInputKeyDown(FKey(n"A")))                Keys += "A ";
        if (IsInputKeyDown(FKey(n"S")))                Keys += "S ";
        if (IsInputKeyDown(FKey(n"D")))                Keys += "D ";
        if (IsInputKeyDown(FKey(n"SpaceBar")))         Keys += "Space ";
        if (IsInputKeyDown(FKey(n"LeftShift")))        Keys += "Shift ";
        if (IsInputKeyDown(FKey(n"LeftControl")))      Keys += "Ctrl ";
        if (IsInputKeyDown(FKey(n"LeftMouseButton")))  Keys += "LMB ";
        if (IsInputKeyDown(FKey(n"RightMouseButton"))) Keys += "RMB ";
        if (Keys.IsEmpty())
            Keys = "(none)";

        // Duration 0 = this frame only; ticking every frame keeps two steady lines on screen.
        PrintToScreen(Line1, 0.0);
        PrintToScreen(f"[MoveDebug] Keys: {Keys}", 0.0);
    }

    UFUNCTION(Exec)
    void MoveTune(FString Param = "", float Value = 0.0)
    {
        AHeroCharacter Hero = GetHero();
        if (Hero == nullptr || Hero.Locomotion == nullptr)
        {
            Print("[MoveTune] needs a hero pawn", 5.0);
            return;
        }
        if (!HasAuthority())
        {
            Print("[MoveTune] host only — tuning on a remote client would desync its prediction", 8.0);
            return;
        }
        URogueLocomotionComponent L = Hero.Locomotion;
        URogueCameraFeelComponent C = Hero.CameraFeel;

        FString Key = Param.ToLower();
        if (Key.IsEmpty() || Key == "dump")
        {
            Print("[MoveTune] current values (paste-ready LocomotionComponent defaults):", 20.0);
            Print(f"[MoveTune] MaxAcceleration = {L.MaxAcceleration}; BrakingDecelerationWalking = {L.BrakingDecelerationWalking}; GroundFriction = {L.GroundFriction};", 20.0);
            Print(f"[MoveTune] BrakingFriction = {L.BrakingFriction}; BrakingFrictionFactor = {L.BrakingFrictionFactor}; GravityScale = {L.GravityScale};", 20.0);
            Print(f"[MoveTune] JumpZVelocity = {L.JumpZVelocity}; DoubleJumpZVelocity = {L.DoubleJumpZVelocity}; JumpMaxHoldTime = {L.JumpMaxHoldTime};", 20.0);
            Print(f"[MoveTune] AirControl = {L.AirControl}; FallingLateralFriction = {L.FallingLateralFriction}; MaxJumpCount = {L.MaxJumpCount};", 20.0);
            Print(f"[MoveTune] SprintSpeedMultiplier = {L.SprintSpeedMultiplier}; CrouchSpeed = {L.CrouchSpeed}; FocusMoveMultiplier = {L.FocusMoveMultiplier};", 20.0);
            Print(f"[MoveTune] SlideBoostMultiplier = {L.SlideBoostMultiplier}; SlideSpeedCapMultiplier = {L.SlideSpeedCapMultiplier}; SlideBoostArmMultiplier = {L.SlideBoostArmMultiplier};", 20.0);
            Print(f"[MoveTune] SlideEntryMinFraction = {L.SlideEntryMinFraction}; SlideExitSpeedFraction = {L.SlideExitSpeedFraction}; SlideGroundFriction = {L.SlideGroundFriction};", 20.0);
            Print(f"[MoveTune] SlideBraking = {L.SlideBraking}; SlideDownhillAccel = {L.SlideDownhillAccel}; SlideMaxDuration = {L.SlideMaxDuration};", 20.0);
            Print(f"[MoveTune] SlideBoostCooldown = {L.SlideBoostCooldown}; SlideSustainMinSlope = {L.SlideSustainMinSlope}; bRequireSprintToSlide = {L.bRequireSprintToSlide};", 20.0);
            Print("[MoveTune] current values (paste-ready CameraFeelComponent.as defaults):", 20.0);
            Print(f"[MoveTune] SprintFOVBonus = {C.SprintFOVBonus}; SlideFOVBonus = {C.SlideFOVBonus}; FOVBlendSpeed = {C.FOVBlendSpeed};", 20.0);
            Print(f"[MoveTune] FireKickPitch = {C.FireKickPitch}; FireKickYawRange = {C.FireKickYawRange}; FireKickMaxOffset = {C.FireKickMaxOffset};", 20.0);
            Print(f"[MoveTune] KickSpringStiffness = {C.KickSpringStiffness}; KickSpringDamping = {C.KickSpringDamping};", 20.0);
            Print(f"[MoveTune] LandDipPerFallSpeed = {C.LandDipPerFallSpeed}; LandDipMax = {C.LandDipMax};", 20.0);
            Print(f"[MoveTune] DipSpringStiffness = {C.DipSpringStiffness}; DipSpringDamping = {C.DipSpringDamping};", 20.0);
            Print(f"[MoveTune] SlideRollMax = {C.SlideRollMax}; SlideRollBlendSpeed = {C.SlideRollBlendSpeed};", 20.0);
            Print("[MoveTune] current values (paste-ready HeroCharacter.as facing defaults):", 20.0);
            Print(f"[MoveTune] IdleFreeLookYawLimit = {Hero.IdleFreeLookYawLimit}; IdleAlignYawRate = {Hero.IdleAlignYawRate};", 20.0);
            // Stamina pips (D-0023): live count plus the two regen tunables on the hero.
            Print(f"[MoveTune] Stamina = {Hero.GetStamina()}/{Hero.GetMaxStamina()}; StaminaRegenSeconds = {Hero.StaminaRegenSeconds}; StaminaRegenDelay = {Hero.StaminaRegenDelay};", 20.0);
            // Weapon spread/heat knobs (tunes the shared definition ASSET in memory; host-only live tuning, not persisted).
            if (Hero.Weapon != nullptr && Hero.Weapon.Definition != nullptr)
            {
                URogueWeaponDefinition WD = Hero.Weapon.Definition;
                Print("[MoveTune] current values (weapon spread/heat; tunes shared DA in memory):", 20.0);
                Print(f"[MoveTune] HeatPerShot = {WD.HeatPerShot}; HeatCooldownPerSecond = {WD.HeatCooldownPerSecond}; SpreadRecoveryDelay = {WD.SpreadRecoveryDelay};", 20.0);
            }
            return;
        }

        if (Key == "maxacceleration")                   L.MaxAcceleration = Value;
        else if (Key == "brakingdecelerationwalking")   L.BrakingDecelerationWalking = Value;
        else if (Key == "groundfriction")               L.GroundFriction = Value;
        else if (Key == "brakingfriction")              L.BrakingFriction = Value;
        else if (Key == "brakingfrictionfactor")        L.BrakingFrictionFactor = Value;
        else if (Key == "gravityscale")                 L.GravityScale = Value;
        else if (Key == "jumpzvelocity")                L.JumpZVelocity = Value;
        else if (Key == "doublejumpzvelocity")          L.DoubleJumpZVelocity = Value;
        else if (Key == "jumpmaxholdtime")              L.JumpMaxHoldTime = Value;
        else if (Key == "aircontrol")                   L.AirControl = Value;
        else if (Key == "fallinglateralfriction")       L.FallingLateralFriction = Value;
        else if (Key == "maxjumpcount")                 L.MaxJumpCount = int(Value);
        else if (Key == "sprintspeedmultiplier")        L.SprintSpeedMultiplier = Value;
        else if (Key == "crouchspeed")                  L.CrouchSpeed = Value;
        else if (Key == "focusmovemultiplier")          L.FocusMoveMultiplier = Value;
        else if (Key == "slideboostmultiplier")         L.SlideBoostMultiplier = Value;
        else if (Key == "slidespeedcapmultiplier")      L.SlideSpeedCapMultiplier = Value;
        else if (Key == "slideboostarmmultiplier")      L.SlideBoostArmMultiplier = Value;
        else if (Key == "slideentryminfraction")        L.SlideEntryMinFraction = Value;
        else if (Key == "slideexitspeedfraction")       L.SlideExitSpeedFraction = Value;
        else if (Key == "slidegroundfriction")          L.SlideGroundFriction = Value;
        else if (Key == "slidebraking")                 L.SlideBraking = Value;
        else if (Key == "slidedownhillaccel")           L.SlideDownhillAccel = Value;
        else if (Key == "slidemaxduration")             L.SlideMaxDuration = Value;
        else if (Key == "slideboostcooldown")           L.SlideBoostCooldown = Value;
        else if (Key == "slidesustainminslope")         L.SlideSustainMinSlope = Value;
        else if (Key == "brequiresprinttoslide")        L.bRequireSprintToSlide = Value != 0.0;
        // Camera-feel knobs: read per-tick by the component, so the assignment alone is live
        // (ApplyMovementConfig below is harmless for them).
        else if (Key == "sprintfovbonus")               C.SprintFOVBonus = Value;
        else if (Key == "slidefovbonus")                C.SlideFOVBonus = Value;
        else if (Key == "fovblendspeed")                C.FOVBlendSpeed = Value;
        else if (Key == "firekickpitch")                C.FireKickPitch = Value;
        else if (Key == "firekickyawrange")             C.FireKickYawRange = Value;
        else if (Key == "firekickmaxoffset")            C.FireKickMaxOffset = Value;
        else if (Key == "kickspringstiffness")          C.KickSpringStiffness = Value;
        else if (Key == "kickspringdamping")            C.KickSpringDamping = Value;
        else if (Key == "landdipperfallspeed")          C.LandDipPerFallSpeed = Value;
        else if (Key == "landdipmax")                   C.LandDipMax = Value;
        else if (Key == "dipspringstiffness")           C.DipSpringStiffness = Value;
        else if (Key == "dipspringdamping")             C.DipSpringDamping = Value;
        else if (Key == "sliderollmax")                 C.SlideRollMax = Value;
        else if (Key == "sliderollblendspeed")          C.SlideRollBlendSpeed = Value;
        // Facing knobs live on the hero (idle free-look).
        else if (Key == "idlefreelookyawlimit")         Hero.IdleFreeLookYawLimit = Value;
        else if (Key == "idlealignyawrate")             Hero.IdleAlignYawRate = Value;
        // Stamina pip regen knobs live on the hero (D-0023).
        else if (Key == "staminaregenseconds")          Hero.StaminaRegenSeconds = Value;
        else if (Key == "staminaregendelay")            Hero.StaminaRegenDelay = Value;
        // Weapon spread/heat knobs — tune the shared definition ASSET in memory (host-only live
        // tuning; not persisted; takes effect on the next shot/cooldown tick, no reload needed).
        else if (Key == "heatpershot")
        {
            if (Hero.Weapon == nullptr || Hero.Weapon.Definition == nullptr) { Print("[MoveTune] heatpershot: no weapon equipped", 5.0); return; }
            Hero.Weapon.Definition.HeatPerShot = Value;
            Print(f"[MoveTune] {Param} = {Value} (applied to weapon definition in memory)", 6.0);
            return;
        }
        else if (Key == "heatcooldown")
        {
            if (Hero.Weapon == nullptr || Hero.Weapon.Definition == nullptr) { Print("[MoveTune] heatcooldown: no weapon equipped", 5.0); return; }
            Hero.Weapon.Definition.HeatCooldownPerSecond = Value;
            Print(f"[MoveTune] {Param} = {Value} (applied to weapon definition in memory)", 6.0);
            return;
        }
        else if (Key == "spreadrecoverydelay")
        {
            if (Hero.Weapon == nullptr || Hero.Weapon.Definition == nullptr) { Print("[MoveTune] spreadrecoverydelay: no weapon equipped", 5.0); return; }
            Hero.Weapon.Definition.SpreadRecoveryDelay = Value;
            Print(f"[MoveTune] {Param} = {Value} (applied to weapon definition in memory)", 6.0);
            return;
        }
        else
        {
            Print(f"[MoveTune] unknown param '{Param}' — run bare MoveTune to list every name", 8.0);
            return;
        }

        L.ApplyMovementConfig();
        if (L.IsSliding())
            Print(f"[MoveTune] {Param} = {Value} (applied; ground-friction class values land when the slide ends)", 6.0);
        else
            Print(f"[MoveTune] {Param} = {Value} (applied)", 6.0);
    }

    // --- Debug: slide-rule battery (feel pass). Asserts StartSlide's three Apex-rule invariants
    // (boost below the arming threshold, hard cap regardless, no boost above the threshold) plus
    // the D-0023 stamina spend math. All checks read SYNCHRONOUSLY after the call — no tick runs
    // between the press and the read, so the speed we read is exactly StartSlide's transform of
    // the velocity we set (and no tick means checks 1-3 never observe a slide edge, so they stay
    // pip-neutral). The slide is a COMMIT move now — CrouchReleased no longer ends it — so each
    // check force-resets via ResetAirState() to enter clean (else check 2/3's press is a no-op
    // against the still-running check-1 slide). `[MoveSmoke] RESULT 4/4` is the SmokeTest.ps1
    // assertion (RaidArena case). Polls at boot like the other batteries so it works as -ExecCmds.
    private int MoveSmokeRetries = 0;

    UFUNCTION(Exec)
    void MoveSmoke()
    {
        AHeroCharacter Hero = GetHero();
        URogueLocomotionComponent L = Hero != nullptr ? Hero.Locomotion : nullptr;
        UCharacterMovementComponent Move = Hero != nullptr ? Hero.CharacterMovement : nullptr;
        if (L == nullptr || Move == nullptr || !Move.IsMovingOnGround())
        {
            // Boot-time friendliness: -ExecCmds fires before the hero embodies (and the spawn
            // drop settles) — poll until it stands on ground; slide entry requires grounded.
            if (MoveSmokeRetries < 30)
            {
                MoveSmokeRetries++;
                System::SetTimer(this, n"MoveSmoke", 1.0, false);
                return;
            }
            Print("[MoveSmoke] gave up waiting for a grounded hero pawn", 8.0);
            return;
        }

        int Pass = 0;
        FVector Fwd = Hero.GetActorForwardVector();
        Fwd.Z = 0.0;
        Fwd = Fwd.GetSafeNormal();

        // 1) Boost: enter at exactly sprint speed (below the arming threshold, cooldown cold)
        //    -> boosted past sprint, to ~sprint * SlideBoostMultiplier.
        Hero.SetSprint(true);
        Move.Velocity = Fwd * L.SprintSpeed();
        Hero.CrouchPressed();
        float Speed1 = FlatSpeed(Move);
        bool bCheck1 = L.IsSliding() && Speed1 > L.SprintSpeed()
                    && Speed1 >= L.SprintSpeed() * L.SlideBoostMultiplier * 0.95;
        if (bCheck1)
            Pass++;
        else
            Print(f"[MoveSmoke] FAIL 1: boost (sliding={L.IsSliding()} speed={Speed1} sprint={L.SprintSpeed()} target={L.SprintSpeed() * L.SlideBoostMultiplier})", 15.0);
        Hero.CrouchReleased();
        L.ResetAirState();

        // 2) Cap: enter at twice the cap -> clamped to SlideCap. The cap clamps every entry,
        //    boost cooldown (still hot from check 1) or not.
        Move.Velocity = Fwd * (L.SlideCap() * 2.0);
        Hero.CrouchPressed();
        float Speed2 = FlatSpeed(Move);
        bool bCheck2 = Speed2 <= L.SlideCap() * 1.01;
        if (bCheck2)
            Pass++;
        else
            Print(f"[MoveSmoke] FAIL 2: cap (speed={Speed2} cap={L.SlideCap()})", 15.0);
        Hero.CrouchReleased();
        L.ResetAirState();

        // 3) No boost above the arming threshold: entry just over sprint * SlideBoostArmMultiplier
        //    must keep its speed, not gain. (Check 1's boost cooldown is also still hot —
        //    intentional belt-and-suspenders: either rule alone must refuse the boost.)
        float Entry3 = L.SprintSpeed() * (L.SlideBoostArmMultiplier + 0.05);
        Move.Velocity = Fwd * Entry3;
        Hero.CrouchPressed();
        float Speed3 = FlatSpeed(Move);
        bool bCheck3 = Speed3 <= Entry3 * 1.01;
        if (bCheck3)
            Pass++;
        else
            Print(f"[MoveSmoke] FAIL 3: armed-over-threshold boost leaked (speed={Speed3} entry={Entry3})", 15.0);
        Hero.CrouchReleased();
        Hero.SetSprint(false);
        L.ResetAirState();

        // 4) Stamina gate math (D-0023): one spend drops exactly one pip — synthetic, no PIE
        //    timing. On authority with a pip banked the count must read S0 - 1; anywhere else
        //    (no authority, or already empty) the spend must be a no-op.
        float S0 = Hero.GetStamina();
        Hero.SpendStaminaPip();
        float S1 = Hero.GetStamina();
        float SExpected = (HasAuthority() && S0 >= 1.0) ? S0 - 1.0 : S0;
        bool bCheck4 = Math::Abs(S1 - SExpected) < 0.001;
        if (bCheck4)
            Pass++;
        else
            Print(f"[MoveSmoke] FAIL 4: stamina spend (before={S0} after={S1} expected={SExpected})", 15.0);
        // Restore the spent pip so the smoke is side-effect free (no public restore — write the
        // base value back through the same ASC path SpendStaminaPip mutates; server only).
        if (HasAuthority())
        {
            UAngelscriptAbilitySystemComponent ASC = Hero.GetRogueAbilitySystem();
            if (ASC != nullptr)
                ASC.SetAttributeBaseValue(URogueMovementSet, n"Stamina", S0);
        }

        Print(f"[MoveSmoke] RESULT {Pass}/4", 15.0);
    }

    // Headless determinism + validation gate for procedural generation (procgen Plan 1). Pure: no
    // pawn/world needed, so it runs immediately under -ExecCmds. SmokeTest.ps1 greps the RESULT line.
    UFUNCTION(Exec)
    void GenSmoke()
    {
        FRaidGenConfig Cfg;
        int Pass = 0;
        int Total = 0;

        // 1. Same seed -> identical layout.
        Total += 1;
        FRaidLayout A = RaidGen::Generate(12345, Cfg);
        FRaidLayout B = RaidGen::Generate(12345, Cfg);
        if (RaidGen::LayoutsEqual(A, B)) Pass += 1;
        else Print("[GenSmoke] FAIL 1: same seed diverged", 12.0);

        // 2. Different seed -> different layout.
        Total += 1;
        FRaidLayout C = RaidGen::Generate(99999, Cfg);
        if (!RaidGen::LayoutsEqual(A, C)) Pass += 1;
        else Print("[GenSmoke] FAIL 2: different seeds identical", 12.0);

        // 3. Validated layout passes the full battery.
        Total += 1;
        FRaidLayout V = RaidGen::GenerateValidated(12345, Cfg);
        FRaidValidationResult RV = RaidValidate::Validate(V, Cfg);
        if (V.bValid && RV.bOk) Pass += 1;
        else Print(f"[GenSmoke] FAIL 3: validated invalid ({RV.PassCount}/{RV.Total} {RV.FirstFail})", 12.0);

        // 4. Validated generation is reproducible (reroll determinism).
        Total += 1;
        FRaidLayout V2 = RaidGen::GenerateValidated(12345, Cfg);
        if (RaidGen::LayoutsEqual(V, V2)) Pass += 1;
        else Print("[GenSmoke] FAIL 4: GenerateValidated not reproducible", 12.0);

        // 5. The safe fallback itself validates (never-softlock net).
        Total += 1;
        FRaidLayout Safe = RaidGen::BuildSafeFallback(Cfg);
        FRaidValidationResult RS = RaidValidate::Validate(Safe, Cfg);
        if (RS.bOk) Pass += 1;
        else Print(f"[GenSmoke] FAIL 5: fallback invalid ({RS.PassCount}/{RS.Total} {RS.FirstFail})", 12.0);

        // 6. An unreachable platform is rejected (jump-reachability active).
        Total += 1;
        FRaidLayout BadReach = RaidGen::Generate(12345, Cfg);
        for (int i = 0; i < BadReach.MainSites[0].Nodes.Num(); i++)
            if (BadReach.MainSites[0].Nodes[i].Slot == ERaidSlotType::HighGround)
            { BadReach.MainSites[0].Nodes[i].Location.Z = 5000.0; break; }
        FRaidValidationResult RR = RaidValidate::Validate(BadReach, Cfg);
        if (!RR.bOk && RR.FirstFail == "jump-reachability") Pass += 1;
        else Print(f"[GenSmoke] FAIL 6: unreachable platform not caught ({RR.FirstFail})", 12.0);

        Print(f"[GenSmoke] RESULT {Pass}/{Total}", 15.0);
    }

    private float FlatSpeed(UCharacterMovementComponent Move) const
    {
        FVector Vel = Move.Velocity;
        return FVector(Vel.X, Vel.Y, 0.0).Size();
    }

    // --- Debug: apply EVERY upgrade in the GameMode pool to the host hero, then print the weapon
    // attribute values — proves the GE assets actually move the URogueCombatSet attributes. ---
    private int GrantRetries = 0;

    UFUNCTION(Exec)
    void GrantAllUpgrades()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        AHeroCharacter Hero = GetHero();
        if (GameMode == nullptr || Hero == nullptr)
        {
            // Boot-time friendliness: poll until the hero is embodied (host only — clients give up).
            if (GameMode != nullptr && GrantRetries < 30)
            {
                GrantRetries++;
                System::SetTimer(this, n"GrantAllUpgrades", 1.0, false);
                return;
            }
            Print("[GrantAll] host only, and needs a hero pawn", 5.0);
            return;
        }
        UAngelscriptAbilitySystemComponent ASC = Hero.GetRogueAbilitySystem();
        if (ASC == nullptr)
            return;

        int Applied = 0;
        for (URogueUpgradeDef Upgrade : GameMode.UpgradePool)
        {
            if (Upgrade == nullptr || Upgrade.Effect.Get() == nullptr)
                continue;
            ASC.ApplyGameplayEffectToTarget(Upgrade.Effect, ASC, 1.0, FGameplayEffectContextHandle());
            Applied++;
        }

        float DmgBonus = ASC.GetAttributeCurrentValue(URogueCombatSet, n"WeaponDamageBonus", -1.0);
        float FireRate = ASC.GetAttributeCurrentValue(URogueCombatSet, n"FireRateBonus", -1.0);
        float Pierce = ASC.GetAttributeCurrentValue(URogueCombatSet, n"PierceCount", -1.0);
        float Chain = ASC.GetAttributeCurrentValue(URogueCombatSet, n"ChainCount", -1.0);
        float Burn = ASC.GetAttributeCurrentValue(URogueCombatSet, n"BurnChance", -1.0);
        float Poison = ASC.GetAttributeCurrentValue(URogueCombatSet, n"PoisonChance", -1.0);
        float Mag = ASC.GetAttributeCurrentValue(URogueCombatSet, n"MagazineBonus", -1.0);
        float Reload = ASC.GetAttributeCurrentValue(URogueCombatSet, n"ReloadSpeedBonus", -1.0);
        Print(f"[GrantAll] applied {Applied} GEs", 8.0);
        Print(f"[GrantAll] weapon attrs: dmg={DmgBonus} firerate={FireRate} pierce={Pierce} chain={Chain} burn={Burn} poison={Poison} mag={Mag} reload={Reload}", 10.0);
    }

    // --- Debug: per-upgrade test commands (DL_Upgrades is the matching firing-range level). ---
    // `ListUpgrades`          — print the GameMode pool with names/rarities.
    // `GrantUpgrade <name>`   — apply ONE upgrade by (partial) name; repeat the command to stack.
    // `UpgradeSmoke`          — automated battery: applies every pool upgrade one at a time and
    //                           asserts its GE moved at least one tracked attribute. Final line
    //                           `[UpgradeSmoke] RESULT n/n` is the SmokeTest.ps1 assertion.
    // All host-only (GEs apply on the authority's ASC); all poll at boot so they work as -ExecCmds.

    UFUNCTION(Exec)
    void ListUpgrades()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (GameMode == nullptr)
        {
            Print("[Upgrades] host only", 5.0);
            return;
        }
        for (int i = 0; i < GameMode.UpgradePool.Num(); i++)
        {
            URogueUpgradeDef Upgrade = GameMode.UpgradePool[i];
            if (Upgrade != nullptr)
                Print(f"[Upgrades] {i}: {Upgrade.GetName()} \"{Upgrade.DisplayName}\" r{Upgrade.Rarity}", 12.0);
        }
    }

    private FString PendingGrantFilter;
    private int GrantOneRetries = 0;

    UFUNCTION(Exec)
    void GrantUpgrade(FString NameFilter)
    {
        if (NameFilter.IsEmpty())
        {
            Print("[Upgrade] usage: GrantUpgrade <part of the upgrade name> (see ListUpgrades)", 6.0);
            return;
        }
        PendingGrantFilter = NameFilter;
        GrantOneRetries = 0;
        TryGrantUpgrade();
    }

    UFUNCTION()
    private void TryGrantUpgrade()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        if (GameMode == nullptr || ASC == nullptr)
        {
            // Boot-time friendliness: -ExecCmds fires before the hero embodies, so poll (host only).
            if (GameMode != nullptr && GrantOneRetries < 30)
            {
                GrantOneRetries++;
                System::SetTimer(this, n"TryGrantUpgrade", 1.0, false);
                return;
            }
            Print("[Upgrade] host only, and needs a hero pawn", 5.0);
            return;
        }

        FString Filter = PendingGrantFilter.ToLower();
        for (URogueUpgradeDef Upgrade : GameMode.UpgradePool)
        {
            if (Upgrade == nullptr || Upgrade.Effect.Get() == nullptr)
                continue;
            FString AssetName = FString(f"{Upgrade.GetName()}").ToLower();
            FString Display = Upgrade.DisplayName.ToString().ToLower();
            if (AssetName.Contains(Filter) || Display.Contains(Filter))
            {
                ApplyUpgradeAndReport(ASC, Upgrade);
                return;
            }
        }
        FString Candidates;
        for (URogueUpgradeDef Upgrade : GameMode.UpgradePool)
        {
            if (Upgrade != nullptr)
                Candidates += f" {Upgrade.GetName()}";
        }
        Print(f"[Upgrade] no pool entry matches '{PendingGrantFilter}' — pool:{Candidates}", 8.0);
    }

    private int UpgradeSmokeRetries = 0;

    UFUNCTION(Exec)
    void UpgradeSmoke()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        if (GameMode == nullptr || ASC == nullptr)
        {
            if (GameMode != nullptr && UpgradeSmokeRetries < 30)
            {
                UpgradeSmokeRetries++;
                System::SetTimer(this, n"UpgradeSmoke", 1.0, false);
                return;
            }
            Print("[UpgradeSmoke] host only, and needs a hero pawn", 5.0);
            return;
        }

        int Total = 0;
        int Ok = 0;
        for (URogueUpgradeDef Upgrade : GameMode.UpgradePool)
        {
            if (Upgrade == nullptr || Upgrade.Effect.Get() == nullptr)
                continue;
            Total++;
            if (ApplyUpgradeAndReport(ASC, Upgrade) > 0)
                Ok++;
        }
        Print(f"[UpgradeSmoke] RESULT {Ok}/{Total} upgrades moved attributes", 12.0);
    }

    // Every attribute an upgrade GE may legally touch. The diff battery snapshots these around
    // each apply; an upgrade whose GE moves none of them is a broken asset (wrong attribute path,
    // wrong op — see memory ge-modifier-editing-via-python).
    private void GetTrackedAttributes(TArray<FName>& OutNames, TArray<bool>& OutIsHealthSet)
    {
        OutNames.Empty();
        OutIsHealthSet.Empty();
        // URogueCombatSet
        OutNames.Add(n"MoveSpeed");           OutIsHealthSet.Add(false);
        OutNames.Add(n"AbilityPower");        OutIsHealthSet.Add(false);
        OutNames.Add(n"CooldownReduction");   OutIsHealthSet.Add(false);
        OutNames.Add(n"BarrageRadiusBonus");  OutIsHealthSet.Add(false);
        OutNames.Add(n"BarrageClusterBonus"); OutIsHealthSet.Add(false);
        OutNames.Add(n"WeaponDamageBonus");   OutIsHealthSet.Add(false);
        OutNames.Add(n"FireRateBonus");       OutIsHealthSet.Add(false);
        OutNames.Add(n"PierceCount");         OutIsHealthSet.Add(false);
        OutNames.Add(n"ChainCount");          OutIsHealthSet.Add(false);
        OutNames.Add(n"BurnChance");          OutIsHealthSet.Add(false);
        OutNames.Add(n"PoisonChance");        OutIsHealthSet.Add(false);
        OutNames.Add(n"MagazineBonus");       OutIsHealthSet.Add(false);
        OutNames.Add(n"ReloadSpeedBonus");    OutIsHealthSet.Add(false);
        OutNames.Add(n"ChainIgniteFraction");      OutIsHealthSet.Add(false);
        OutNames.Add(n"ClusterChainBonusArcs");    OutIsHealthSet.Add(false);
        OutNames.Add(n"PoisonBurstDps");           OutIsHealthSet.Add(false);
        OutNames.Add(n"ClusterKillShieldAmount");  OutIsHealthSet.Add(false);
        OutNames.Add(n"TauntRadiusBonus");         OutIsHealthSet.Add(false);
        OutNames.Add(n"TauntClusterDurationBonus");OutIsHealthSet.Add(false);
        OutNames.Add(n"TauntDamage");              OutIsHealthSet.Add(false);
        OutNames.Add(n"TauntVortex");              OutIsHealthSet.Add(false);
        OutNames.Add(n"BarrageDamageBonus");       OutIsHealthSet.Add(false);
        OutNames.Add(n"BarrageSalvoCount");        OutIsHealthSet.Add(false);
        OutNames.Add(n"BarrageCarpet");            OutIsHealthSet.Add(false);
        // URogueHealthSet
        OutNames.Add(n"Health");              OutIsHealthSet.Add(true);
        OutNames.Add(n"MaxHealth");           OutIsHealthSet.Add(true);
        OutNames.Add(n"Shield");              OutIsHealthSet.Add(true);
        OutNames.Add(n"MaxShield");           OutIsHealthSet.Add(true);
        OutNames.Add(n"Armor");               OutIsHealthSet.Add(true);
    }

    private float ReadTrackedAttribute(UAngelscriptAbilitySystemComponent ASC, bool bHealthSet, FName Attr)
    {
        if (bHealthSet)
            return ASC.GetAttributeCurrentValue(URogueHealthSet, Attr, 0.0);
        return ASC.GetAttributeCurrentValue(URogueCombatSet, Attr, 0.0);
    }

    // Apply one upgrade's GE and print exactly which attributes moved. Returns how many moved.
    private int ApplyUpgradeAndReport(UAngelscriptAbilitySystemComponent ASC, URogueUpgradeDef Upgrade)
    {
        TArray<FName> Names;
        TArray<bool> bHealthSet;
        GetTrackedAttributes(Names, bHealthSet);

        TArray<float> Before;
        for (int i = 0; i < Names.Num(); i++)
            Before.Add(ReadTrackedAttribute(ASC, bHealthSet[i], Names[i]));

        ASC.ApplyGameplayEffectToTarget(Upgrade.Effect, ASC, 1.0, FGameplayEffectContextHandle());

        int Changed = 0;
        FString Moves;
        for (int i = 0; i < Names.Num(); i++)
        {
            float After = ReadTrackedAttribute(ASC, bHealthSet[i], Names[i]);
            if (Math::Abs(After - Before[i]) > 0.0001)
            {
                Changed++;
                Moves += f" {Names[i]} {Before[i]}->{After}";
            }
        }

        if (Changed > 0)
            Print(f"[Upgrade] {Upgrade.DisplayName}:{Moves}", 8.0);
        else
            Print(f"[Upgrade] {Upgrade.DisplayName}: NO ATTRIBUTE CHANGE (broken GE?)", 8.0);
        return Changed;
    }

    // --- Debug: grant shared team XP (host only). `RaidGiveXP 100` = exactly one level-up at the
    // base curve -> pause + offer; `RaidGiveXP 1000` crosses several levels but offers once. ---
    UFUNCTION(Exec)
    void RaidGiveXP(float Amount)
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (GameMode == nullptr)
        {
            Print("[XP] host only", 4.0);
            return;
        }
        GameMode.AddTeamXP(Amount);
    }

    // --- Debug: teleport the hero onto the mini-boss chest so its proximity-open fires through
    // the real path (useful headlessly and to skip the walk in PIE). ---
    private int GoToChestRetries = 0;

    UFUNCTION(Exec)
    void RaidGoToChest()
    {
        AHeroCharacter Hero = GetHero();
        TArray<AUpgradeChest> Chests;
        GetAllActorsOfClass(Chests);
        if (Hero == nullptr || Chests.Num() == 0)
        {
            // Boot-time friendliness: the chest only exists after the boss dies — poll for it.
            if (GoToChestRetries < 60)
            {
                GoToChestRetries++;
                System::SetTimer(this, n"RaidGoToChest", 1.0, false);
                return;
            }
            Print("[Chest] needs a hero and a dropped chest", 4.0);
            return;
        }
        Hero.SetActorLocation(Chests[0].GetActorLocation());
        Print("[Chest] teleported to the chest", 3.0);
    }

    // --- Debug: kill the nearest live OBJECTIVE elite (skips fodder) — steps the XP/kill flow
    // one elite at a time (kills award XPValue; killing the boss drops the synergy chest). ---
    UFUNCTION(Exec)
    void RaidKillOneElite()
    {
        AHeroCharacter Hero = GetHero();
        FVector From = Hero != nullptr ? Hero.GetActorLocation() : FVector();

        TArray<AEliteEnemyBase> Enemies;
        GetAllActorsOfClass(Enemies);
        AEliteEnemyBase Target = nullptr;
        float BestDistSq = 1.0e18;
        for (AEliteEnemyBase Enemy : Enemies)
        {
            if (Enemy == nullptr || Enemy.Health == nullptr || Enemy.Health.IsDead())
                continue;
            if (Cast<AFodderEnemy>(Enemy) != nullptr)
                continue;   // fodder doesn't gate the objective — skip it
            float DistSq = Enemy.GetActorLocation().DistSquared(From);
            if (DistSq < BestDistSq)
            {
                BestDistSq = DistSq;
                Target = Enemy;
            }
        }
        if (Target == nullptr)
        {
            Print("[Debug] RaidKillOneElite — no live objective elites", 3.0);
            return;
        }
        Target.Health.ApplyDamage(9999999.0, Hero);
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        int Left = Combat != nullptr ? Combat.GetEliteCount() : -1;
        Print(f"[Debug] RaidKillOneElite — killed {Target.GetName()}, {Left} objective elites left", 4.0);
    }

    // --- Debug: ring a telegraph danger zone at the hero (the cue-pass ground ring, no damage).
    // Eyeball the outline + fill timing in PIE; headlessly, grep the log for "[Telegraph] zone". ---
    private int TelegraphSmokeRetries = 0;

    UFUNCTION(Exec)
    void TelegraphSmoke()
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        AHeroCharacter Hero = GetHero();
        if (Combat == nullptr || Hero == nullptr)
        {
            // Boot-time friendliness: poll until the hero is embodied (works as -ExecCmds).
            if (TelegraphSmokeRetries < 30)
            {
                TelegraphSmokeRetries++;
                System::SetTimer(this, n"TelegraphSmoke", 1.0, false);
                return;
            }
            Print("[Telegraph] host only, and needs a hero pawn", 5.0);
            return;
        }
        Combat.ShowTelegraphZone(Hero.GetActorLocation(), 450.0, 2.0);
        Print("[Telegraph] smoke zone requested (r=450, 2s)", 5.0);
    }

    // --- Debug: Loop-v2 flow battery (D-0019). Asserts: cap filtering, milestone guarantee,
    // synergy duo-gating (solo relaxation), utility padding, reroll spend, watchdog auto-pick.
    // Host-only; polls at boot so it works as -ExecCmds. SmokeTest.ps1 asserts the RESULT line.
    private int FlowSmokeRetries = 0;

    UFUNCTION(Exec)
    void UpgradeFlowSmoke()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        AHeroCharacter Hero = GetHero();
        if (GameMode == nullptr || Hero == nullptr || PlayerState == nullptr)
        {
            if (GameMode != nullptr && FlowSmokeRetries < 30)
            {
                FlowSmokeRetries++;
                System::SetTimer(this, n"UpgradeFlowSmoke", 1.0, false);
                return;
            }
            Print("[FlowSmoke] host only, and needs a hero pawn", 5.0);
            return;
        }

        int Pass = 0;
        int Total = 7;

        // 1) Baseline: a hand has OptionsPerOffer cards (utility padding guarantees it
        //    once the UtilityPool is assigned; before Task 5 content, accept >= 1).
        TArray<URogueUpgradeDef> Hand = GameMode.DebugRollFor(PlayerState, 1, false);
        if (Hand.Num() >= 1)
            Pass++;
        else
            Print("[FlowSmoke] FAIL 1: empty baseline hand", 10.0);

        // 2) Cap filtering: max out the first capped card; it must vanish from 20 salted rolls.
        URogueUpgradeDef Capped;
        for (URogueUpgradeDef Def : GameMode.UpgradePool)
        {
            if (Def != nullptr && !Def.bSynergyUpgrade && !Def.bMilestone && Def.MaxStacks > 0)
            {
                Capped = Def;
                break;
            }
        }
        if (Capped != nullptr)
        {
            for (int i = 0; i < Capped.MaxStacks; i++)
                GameMode.AddStack(PlayerState, Capped);
            bool bLeaked = false;
            for (int s = 0; s < 20; s++)
            {
                TArray<URogueUpgradeDef> H = GameMode.DebugRollFor(PlayerState, 100 + s, false);
                if (H.Contains(Capped))
                    bLeaked = true;
            }
            if (!bLeaked)
                Pass++;
            else
                Print("[FlowSmoke] FAIL 2: capped card still offered", 10.0);
        }
        else
        {
            Pass++;   // no capped cards in pool — vacuously true
            Print("[FlowSmoke] note: no capped card to test", 5.0);
        }

        // 3) Milestone guarantee: find a bMilestone card, satisfy its self-prereqs, assert it
        //    appears in the next hand. Skips (vacuous pass) until Task 5 authors one.
        URogueUpgradeDef Milestone;
        for (URogueUpgradeDef Def : GameMode.UpgradePool)
        {
            if (Def != nullptr && Def.bMilestone && Def.bPrereqSelf && Def.PrereqA != nullptr)
            {
                Milestone = Def;
                break;
            }
        }
        if (Milestone != nullptr)
        {
            int Need = Milestone.PrereqAStacks - GameMode.GetStackCount(PlayerState, Milestone.PrereqA);
            for (int i = 0; i < Need; i++)
                GameMode.AddStack(PlayerState, Milestone.PrereqA);
            if (Milestone.PrereqB != nullptr)
            {
                int NeedB = Milestone.PrereqBStacks - GameMode.GetStackCount(PlayerState, Milestone.PrereqB);
                for (int i = 0; i < NeedB; i++)
                    GameMode.AddStack(PlayerState, Milestone.PrereqB);
            }
            TArray<URogueUpgradeDef> MH = GameMode.DebugRollFor(PlayerState, 7, false);
            if (MH.Contains(Milestone))
                Pass++;
            else
                Print("[FlowSmoke] FAIL 3: eligible milestone not guaranteed a slot", 10.0);
        }
        else
        {
            Pass++;
            Print("[FlowSmoke] note: no milestone card to test (pre-Task-5)", 5.0);
        }

        // 4) Synergy duo gate: an un-met synergy card must NOT roll; after satisfying its
        //    prereqs on this (solo) player, it MUST become rollable.
        URogueUpgradeDef Synergy;
        for (URogueUpgradeDef Def : GameMode.UpgradePool)
        {
            if (Def != nullptr && Def.bSynergyUpgrade && Def.PrereqA != nullptr && !Def.bPrereqSelf)
            {
                Synergy = Def;
                break;
            }
        }
        if (Synergy != nullptr)
        {
            bool bEarlyLeak = false;
            if (GameMode.GetStackCount(PlayerState, Synergy.PrereqA) < Synergy.PrereqAStacks)
            {
                for (int s = 0; s < 20; s++)
                {
                    TArray<URogueUpgradeDef> H = GameMode.DebugRollFor(PlayerState, 200 + s, true);
                    if (H.Contains(Synergy))
                        bEarlyLeak = true;
                }
            }
            int NeedA = Synergy.PrereqAStacks - GameMode.GetStackCount(PlayerState, Synergy.PrereqA);
            for (int i = 0; i < NeedA; i++)
                GameMode.AddStack(PlayerState, Synergy.PrereqA);
            if (Synergy.PrereqB != nullptr)
            {
                int NeedB = Synergy.PrereqBStacks - GameMode.GetStackCount(PlayerState, Synergy.PrereqB);
                for (int i = 0; i < NeedB; i++)
                    GameMode.AddStack(PlayerState, Synergy.PrereqB);
            }
            bool bNowOffered = false;
            for (int s = 0; s < 20; s++)
            {
                TArray<URogueUpgradeDef> H = GameMode.DebugRollFor(PlayerState, 300 + s, true);
                if (H.Contains(Synergy))
                    bNowOffered = true;
            }
            if (!bEarlyLeak && bNowOffered)
                Pass++;
            else
                Print(f"[FlowSmoke] FAIL 4: duo gate (earlyLeak={bEarlyLeak} nowOffered={bNowOffered})", 10.0);
        }
        else
        {
            Pass++;
            Print("[FlowSmoke] note: no gated synergy card to test (pre-Task-5)", 5.0);
        }

        // 5) Determinism: same salt twice = identical hand.
        TArray<URogueUpgradeDef> D1 = GameMode.DebugRollFor(PlayerState, 42, false);
        TArray<URogueUpgradeDef> D2 = GameMode.DebugRollFor(PlayerState, 42, false);
        bool bSame = D1.Num() == D2.Num();
        if (bSame)
        {
            for (int i = 0; i < D1.Num(); i++)
            {
                if (D1[i] != D2[i])
                    bSame = false;
            }
        }
        if (bSame && D1.Num() > 0)
            Pass++;
        else
            Print("[FlowSmoke] FAIL 5: same-salt rolls differ", 10.0);

        // 6) Live round-trip: real offer -> reroll spend -> watchdog auto-pick -> resume.
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        int RerollsBefore = GS != nullptr ? GS.SquadRerollsRemaining : -1;
        GameMode.OfferUpgradesWeighted(70.0, 25.0, 5.0, false);
        Server_RequestReroll();
        int RerollsAfter = GS != nullptr ? GS.SquadRerollsRemaining : -1;
        if (RerollsBefore == 1 && RerollsAfter == 0)
            Pass++;
        else
            Print(f"[FlowSmoke] FAIL 6: reroll {RerollsBefore} -> {RerollsAfter}", 10.0);
        // The watchdog now auto-picks and resumes (fast under headless paused-tick); grep
        // "[Upgrades] raid resumed" separately.

        // 7) Hero gating — RequiredHeroClass must match the candidate player's pawn.
        //    Host pawn is a hero, so an AHeroCharacter gate passes and a spectator gate fails.
        URogueUpgradeDef GatedOk = Cast<URogueUpgradeDef>(NewObject(this, URogueUpgradeDef));
        GatedOk.RequiredHeroClass = AHeroCharacter;
        URogueUpgradeDef GatedWrong = Cast<URogueUpgradeDef>(NewObject(this, URogueUpgradeDef));
        GatedWrong.RequiredHeroClass = ASpectatorPawn;
        bool bCheck7 = GameMode.IsEligible(GatedOk, PlayerState)
                    && !GameMode.IsEligible(GatedWrong, PlayerState);
        if (bCheck7)
            Pass++;
        else
            Print("[FlowSmoke] FAIL 7: hero gating (RequiredHeroClass)", 10.0);

        Print(f"[FlowSmoke] RESULT {Pass}/{Total}", 15.0);
    }

    // --- Debug: print the XP curve table + live team state (pacing tuning, D-0019). ---
    UFUNCTION(Exec)
    void RaidXPReport()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (GameMode == nullptr || GS == nullptr)
        {
            Print("[XPReport] host only", 4.0);
            return;
        }
        float Cumulative = 0.0;
        for (int Lv = 1; Lv <= 12; Lv++)
        {
            float Step = GameMode.XPBasePerLevel + GameMode.XPGrowthPerLevel * float(Lv - 1);
            Cumulative += Step;
            Print(f"[XPReport] L{Lv}->L{Lv + 1}: {Step} (cumulative {Cumulative})", 12.0);
        }
        Print(f"[XPReport] live: level {GS.TeamLevel}, {GS.TeamXP}/{GS.XPToNextLevel}", 12.0);
    }

    // --- Debug: behavior-evolution battery (D-0020). Spawns its own dummies through the
    // SpawnDirector (so deaths reach the GameMode death path), drives the seam + abilities,
    // and asserts each evolution behavior. `[EvoSmoke] RESULT n/7` is the SmokeTest assertion.
    // Run WITHOUT UpgradeSmoke in the same session: UpgradeSmoke applies every pool GE, which
    // would pre-set the evolution flags this battery toggles deliberately. Host-only; polls at
    // boot like the other batteries.
    private int EvoRetries = 0;
    private int EvoPassed = 0;
    private AEliteEnemyBase EvoVortexDummy;
    private AEliteEnemyBase EvoSalvoDummy;
    private float EvoSalvoStartHP = 0.0;
    private AEliteEnemyBase EvoCarpetDummy;
    private float EvoCarpetStartHP = 0.0;

    UFUNCTION(Exec)
    void EvoSmoke()
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        USpawnDirector Director = USpawnDirector::Get();
        if (GameMode == nullptr || ASC == nullptr || Combat == nullptr || Director == nullptr)
        {
            if (EvoRetries < 30)
            {
                EvoRetries++;
                System::SetTimer(this, n"EvoSmoke", 1.0, false);
                return;
            }
            Print("[EvoSmoke] gave up waiting for hero/gamemode", 8.0);
            return;
        }

        FVector H = Hero.GetActorLocation();
        FVector F = Hero.GetActorForwardVector();
        F.Z = 0.0;
        F = F.GetSafeNormal();
        FVector R = FVector(-F.Y, F.X, 0.0);

        // Check 1: Searing Arcs — chain arcs ignite their targets (per-shot params, no GE needed).
        AEliteEnemyBase A1 = Director.SpawnElite(ATargetDummy, H + F * 400.0, FRotator());
        AEliteEnemyBase B1 = Director.SpawnElite(ATargetDummy, H + F * 400.0 + R * 250.0, FRotator());
        bool bCheck1 = false;
        if (A1 != nullptr && B1 != nullptr && B1.Health != nullptr)
        {
            FWeaponShotParams Shot;
            Shot.Damage = 10.0;
            Shot.ChainCount = 5;        // generous: B1 must be among the nearest arcs even if
            Shot.ChainRadius = 300.0;   // placed range targets share the lane
            Shot.ChainIgniteFraction = 0.5;
            FireAt(Combat, Hero, A1, Shot);
            bCheck1 = B1.Health.HasActiveDot(ERogueDotType::Burn);
        }
        EvoCheck("searing arcs (ignite-on-arc)", bCheck1);

        // Check 2: Overwhelm — bonus arcs on a Clustered victim with base ChainCount 0.
        AEliteEnemyBase A2 = Director.SpawnElite(ATargetDummy, H - F * 500.0, FRotator());
        AEliteEnemyBase B2 = Director.SpawnElite(ATargetDummy, H - F * 500.0 + R * 200.0, FRotator());
        AEliteEnemyBase C2 = Director.SpawnElite(ATargetDummy, H - F * 500.0 - R * 200.0, FRotator());
        bool bCheck2 = false;
        if (A2 != nullptr && B2 != nullptr && C2 != nullptr)
        {
            Combat.MarkClustered(A2.GetActorLocation(), 50.0, 30.0);   // only the victim
            FWeaponShotParams Shot;
            Shot.Damage = 10.0;
            Shot.ChainCount = 0;
            Shot.ClusterChainBonusArcs = 2;
            Shot.ChainRadius = 300.0;
            FHitscanResult Res = FireAt(Combat, Hero, A2, Shot);
            bCheck2 = Res.ExtraEnemiesHit >= 2;
        }
        EvoCheck("overwhelm (cluster bonus arcs)", bCheck2);

        // Check 3: Toxic Burst — a poisoned victim's death dots its neighbors (GE + death path).
        bool bCheck3 = false;
        URogueUpgradeDef ToxicBurst = FindPoolDef(GameMode, "ToxicBurst");
        AEliteEnemyBase A3 = Director.SpawnElite(ATargetDummy, H + R * 700.0, FRotator());
        AEliteEnemyBase B3 = Director.SpawnElite(ATargetDummy, H + R * 700.0 + F * 150.0, FRotator());
        if (ToxicBurst != nullptr && ToxicBurst.Effect.Get() != nullptr
            && A3 != nullptr && B3 != nullptr && A3.Health != nullptr && B3.Health != nullptr)
        {
            ASC.ApplyGameplayEffectToTarget(ToxicBurst.Effect, ASC, 1.0, FGameplayEffectContextHandle());
            A3.Health.ApplyDot(ERogueDotType::Poison, 1.0, 30.0, Hero);
            A3.Health.ApplyDamage(999999.0, Hero);   // death fires HandleEnemyKilled inline
            bCheck3 = B3.Health.HasActiveDot(ERogueDotType::Poison);
        }
        EvoCheck("toxic burst (poison death cloud)", bCheck3);

        // Check 4: Iron Bulwark — a Clustered kill grants squad Shield (its GE also adds MaxShield).
        bool bCheck4 = false;
        URogueUpgradeDef Bulwark = FindPoolDef(GameMode, "IronBulwark");
        AEliteEnemyBase A4 = Director.SpawnElite(ATargetDummy, H - R * 700.0, FRotator());
        if (Bulwark != nullptr && Bulwark.Effect.Get() != nullptr && A4 != nullptr && A4.Health != nullptr)
        {
            ASC.ApplyGameplayEffectToTarget(Bulwark.Effect, ASC, 1.0, FGameplayEffectContextHandle());
            float ShieldBefore = ASC.GetAttributeCurrentValue(URogueHealthSet, n"Shield", 0.0);
            Combat.MarkClustered(A4.GetActorLocation(), 50.0, 30.0);
            A4.Health.ApplyDamage(999999.0, Hero);
            float ShieldAfter = ASC.GetAttributeCurrentValue(URogueHealthSet, n"Shield", 0.0);
            bCheck4 = ShieldAfter > ShieldBefore + 0.5;
        }
        EvoCheck("iron bulwark (clustered-kill shield)", bCheck4);

        // Checks 5-7 need real time (vortex pulses / salvo echo / carpet march) — phase chain.
        System::SetTimer(this, n"EvoPhaseVortex", 0.5, false);
    }

    UFUNCTION()
    private void EvoPhaseVortex()
    {
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        USpawnDirector Director = USpawnDirector::Get();
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (ASC == nullptr || Director == nullptr || GameMode == nullptr)
        {
            EvoFinishEarly("vortex phase lost the hero");
            return;
        }

        FVector F = Hero.GetActorForwardVector();
        EvoVortexDummy = Director.SpawnElite(ATargetDummy, Hero.GetActorLocation() + F * 600.0, FRotator());

        URogueUpgradeDef Horizon = FindPoolDef(GameMode, "EventHorizon");
        if (Horizon != nullptr && Horizon.Effect.Get() != nullptr)
            ASC.ApplyGameplayEffectToTarget(Horizon.Effect, ASC, 1.0, FGameplayEffectContextHandle());

        ASC.GiveAbility(UGA_Taunt, 1, -1, nullptr);
        bool bActivated = ASC.TryActivateAbilityByClass(UGA_Taunt, false);
        Print(f"[EvoSmoke] taunt activated={bActivated}", 6.0);

        // Base Clustered expires 3.0s after activation; the vortex refreshes through 3.0s, so at
        // +4.0s "still clustered" can only mean pulses fired.
        System::SetTimer(this, n"EvoCheckVortex", 4.0, false);
    }

    UFUNCTION()
    private void EvoCheckVortex()
    {
        bool bStill = EvoVortexDummy != nullptr && EvoVortexDummy.IsClustered();
        EvoCheck("event horizon (vortex refresh)", bStill);
        System::SetTimer(this, n"EvoPhaseSalvo", 0.5, false);
    }

    UFUNCTION()
    private void EvoPhaseSalvo()
    {
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        USpawnDirector Director = USpawnDirector::Get();
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (ASC == nullptr || Director == nullptr || GameMode == nullptr)
        {
            EvoFinishEarly("salvo phase lost the hero");
            return;
        }

        FVector F = Hero.GetActorForwardVector();
        EvoSalvoDummy = Director.SpawnElite(ATargetDummy, Hero.GetActorLocation() + F * 300.0, FRotator());
        EvoSalvoStartHP = (EvoSalvoDummy != nullptr && EvoSalvoDummy.Health != nullptr)
            ? EvoSalvoDummy.Health.Health : 0.0;

        URogueUpgradeDef Salvo = FindPoolDef(GameMode, "TwinSalvo");
        if (Salvo != nullptr && Salvo.Effect.Get() != nullptr)
            ASC.ApplyGameplayEffectToTarget(Salvo.Effect, ASC, 1.0, FGameplayEffectContextHandle());

        ASC.GiveAbility(UGA_Barrage, 1, -1, nullptr);
        bool bActivated = ASC.TryActivateAbilityByClass(UGA_Barrage, false);
        Print(f"[EvoSmoke] barrage(salvo) activated={bActivated}", 6.0);

        System::SetTimer(this, n"EvoCheckSalvo", 1.5, false);
    }

    UFUNCTION()
    private void EvoCheckSalvo()
    {
        // Base strike alone = 40 (unclustered). Strike + 60% echo = 64. Assert past the single.
        bool bEcho = false;
        if (EvoSalvoDummy != nullptr && EvoSalvoDummy.Health != nullptr)
            bEcho = (EvoSalvoStartHP - EvoSalvoDummy.Health.Health) >= 50.0;
        EvoCheck("twin salvo (echo strike)", bEcho);

        // Wait out the barrage cooldown before the carpet activation.
        System::SetTimer(this, n"EvoPhaseCarpet", 12.0, false);
    }

    UFUNCTION()
    private void EvoPhaseCarpet()
    {
        AHeroCharacter Hero = GetHero();
        UAngelscriptAbilitySystemComponent ASC = Hero != nullptr ? Hero.GetRogueAbilitySystem() : nullptr;
        USpawnDirector Director = USpawnDirector::Get();
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        if (ASC == nullptr || Director == nullptr || GameMode == nullptr)
        {
            EvoFinishEarly("carpet phase lost the hero");
            return;
        }

        FVector F = Hero.GetActorForwardVector();
        // 900uu out: beyond the 600 base radius, covered by carpet pad 2 (3 * 300uu).
        EvoCarpetDummy = Director.SpawnElite(ATargetDummy, Hero.GetActorLocation() + F * 900.0, FRotator());
        EvoCarpetStartHP = (EvoCarpetDummy != nullptr && EvoCarpetDummy.Health != nullptr)
            ? EvoCarpetDummy.Health.Health : 0.0;

        URogueUpgradeDef Carpet = FindPoolDef(GameMode, "CarpetBombing");
        if (Carpet != nullptr && Carpet.Effect.Get() != nullptr)
            ASC.ApplyGameplayEffectToTarget(Carpet.Effect, ASC, 1.0, FGameplayEffectContextHandle());

        bool bActivated = ASC.TryActivateAbilityByClass(UGA_Barrage, false);
        Print(f"[EvoSmoke] barrage(carpet) activated={bActivated}", 6.0);

        System::SetTimer(this, n"EvoCheckCarpet", 3.0, false);
    }

    UFUNCTION()
    private void EvoCheckCarpet()
    {
        bool bMarched = false;
        if (EvoCarpetDummy != nullptr && EvoCarpetDummy.Health != nullptr)
            bMarched = EvoCarpetDummy.Health.Health < EvoCarpetStartHP - 0.5;
        EvoCheck("carpet bombing (marching strip)", bMarched);
        Print(f"[EvoSmoke] RESULT {EvoPassed}/7", 20.0);
    }

    private void EvoFinishEarly(FString Why)
    {
        Print(f"[EvoSmoke] aborted: {Why}", 10.0);
        Print(f"[EvoSmoke] RESULT {EvoPassed}/7", 20.0);
    }

    private FHitscanResult FireAt(UCombatSubsystem Combat, AHeroCharacter Hero,
                                  AEliteEnemyBase Target, FWeaponShotParams Shot)
    {
        FVector From = Hero.GetMuzzleLocation();
        FVector Dir = (Target.GetActorLocation() - From).GetSafeNormal();
        return Combat.FireWeaponShot(From, From + Dir * 20000.0, Shot, Hero);
    }

    private URogueUpgradeDef FindPoolDef(ARaidGameMode GameMode, FString NamePart)
    {
        FString Filter = NamePart.ToLower();
        for (URogueUpgradeDef Def : GameMode.UpgradePool)
        {
            if (Def == nullptr)
                continue;
            FString AssetName = FString(f"{Def.GetName()}").ToLower();
            if (AssetName.Contains(Filter))
                return Def;
        }
        Print(f"[EvoSmoke] pool card not found: {NamePart}", 8.0);
        return nullptr;
    }

    private void EvoCheck(FString Name, bool bPass)
    {
        if (bPass)
            EvoPassed++;
        FString Tag = bPass ? "PASS" : "FAIL";
        Print(f"[EvoSmoke] {Tag}: {Name}", 10.0);
    }

    // --- Debug: wave-director pure-function battery (D-0020). No spawning — asserts the plan
    // math. `[DirectorSmoke] RESULT 6/6` is the SmokeTest assertion. Safe anywhere, any time.
    UFUNCTION(Exec)
    void DirectorReport()
    {
        FDirectorTunables T;   // struct defaults mirror ARaidObjective's class defaults
        int Passed = 0;

        // The curve for human eyeballs: levels 1..12, representative wave 6, solo vs duo.
        for (int L = 1; L <= 12; L++)
        {
            FWavePlan P1 = RaidDirector::ComputeWavePlan(L, 6, 1, T);
            FWavePlan P2 = RaidDirector::ComputeWavePlan(L, 6, 2, T);
            Print(f"[Director] L{L}: solo={P1.FodderCount}@{P1.Interval}s duo={P2.FodderCount}@{P2.Interval}s inject={P1.EliteInjectIndex}", 12.0);
        }

        // 1: wave size never shrinks as level rises.
        bool bMonotonic = true;
        for (int L = 1; L < 12; L++)
        {
            if (RaidDirector::ComputeWavePlan(L + 1, 6, 1, T).FodderCount
                < RaidDirector::ComputeWavePlan(L, 6, 1, T).FodderCount)
                bMonotonic = false;
        }
        Passed += DirCheck("size monotonic in level", bMonotonic);

        // 2: interval respects the floor and never grows with level.
        bool bTempo = true;
        for (int L = 1; L <= 20; L++)
        {
            FWavePlan P = RaidDirector::ComputeWavePlan(L, 6, 1, T);
            if (P.Interval < T.MinInterval - 0.001)
                bTempo = false;
            if (L > 1 && P.Interval > RaidDirector::ComputeWavePlan(L - 1, 6, 1, T).Interval + 0.001)
                bTempo = false;
        }
        Passed += DirCheck("tempo floor + monotonic", bTempo);

        // 3: no injection below the start level.
        bool bNoEarly = true;
        for (int W = 0; W < 12; W++)
        {
            if (RaidDirector::ComputeWavePlan(T.EliteInjectStartLevel - 1, W, 1, T).EliteInjectIndex >= 0)
                bNoEarly = false;
        }
        Passed += DirCheck("no injection before start level", bNoEarly);

        // 4: cadence — every 3rd wave at the start level, every 2nd at the fast level.
        bool bCadence = RaidDirector::ComputeWavePlan(T.EliteInjectStartLevel, 3, 1, T).EliteInjectIndex >= 0
                     && RaidDirector::ComputeWavePlan(T.EliteInjectStartLevel, 4, 1, T).EliteInjectIndex < 0
                     && RaidDirector::ComputeWavePlan(T.EliteInjectFastLevel, 4, 1, T).EliteInjectIndex >= 0
                     && RaidDirector::ComputeWavePlan(T.EliteInjectFastLevel, 3, 1, T).EliteInjectIndex < 0;
        Passed += DirCheck("injection cadence 3rd->2nd", bCadence);

        // 5: clamp holds under absurd inputs.
        bool bClamp = RaidDirector::ComputeWavePlan(99, 999, 4, T).FodderCount == T.MaxPerWave;
        Passed += DirCheck("size clamp", bClamp);

        // 6: pure determinism + a duo is never lighter than solo.
        FWavePlan X = RaidDirector::ComputeWavePlan(7, 9, 2, T);
        FWavePlan Y = RaidDirector::ComputeWavePlan(7, 9, 2, T);
        bool bDet = X.FodderCount == Y.FodderCount && X.Interval == Y.Interval
                 && X.EliteInjectIndex == Y.EliteInjectIndex
                 && X.FodderCount >= RaidDirector::ComputeWavePlan(7, 9, 1, T).FodderCount;
        Passed += DirCheck("determinism + player scaling", bDet);

        Print(f"[DirectorSmoke] RESULT {Passed}/6", 12.0);
    }

    private int DirCheck(FString Name, bool bPass)
    {
        FString Tag = bPass ? "PASS" : "FAIL";
        Print(f"[DirectorSmoke] {Tag}: {Name}", 10.0);
        return bPass ? 1 : 0;
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

    // --- Debug: drive the whole raid loop to a terminal state and assert the win/loss bridges
    // fire (they had no automated proof). `RaidLoopSmoke` runs the VICTORY path (clear the arena
    // -> extraction opens -> CallExtraction spawns the defend wave -> survive the timer ->
    // ERunPhase::Victory); `RaidLoopSmoke defeat` runs the LOSS bridge (NotifyPartyWiped ->
    // ERaidPhase::Failed -> ERunPhase::Defeat). Each ends the run, so the two run in SEPARATE
    // SmokeTest boots. Host/authority only; polls at boot like MoveSmoke. SmokeTest.ps1 asserts
    // `[RaidLoopSmoke] RESULT 4/4` (victory) / `2/2` (defeat). ---
    private FString RaidLoopMode = "victory";
    private int RaidLoopRetries = 0;
    private bool bRaidLoopExtracting = false;
    private bool bRaidLoopWaveSpawned = false;

    UFUNCTION(Exec)
    void RaidLoopSmoke(FString Mode = "victory")
    {
        RaidLoopMode = (Mode == "defeat") ? "defeat" : "victory";
        RaidLoopRetries = 0;
        bRaidLoopExtracting = false;
        bRaidLoopWaveSpawned = false;
        RaidLoopSmokeStep();
    }

    // Re-entry point for the boot poll. SetTimer needs a parameterless UFUNCTION; re-firing the
    // Exec directly would reset Mode to its "victory" default on every retry.
    UFUNCTION()
    void RaidLoopSmokeStep()
    {
        if (!HasAuthority())
        {
            Print("[RaidLoopSmoke] host only (authority required)", 8.0);
            return;
        }
        if (RaidLoopMode == "defeat")
            RaidLoopDefeatStep();
        else
            RaidLoopVictoryStep();
    }

    private void RaidLoopRetry()
    {
        if (RaidLoopRetries < 60)
        {
            RaidLoopRetries++;
            System::SetTimer(this, n"RaidLoopSmokeStep", 0.5, false);
            return;
        }
        int Total = (RaidLoopMode == "defeat") ? 2 : 4;
        Print(f"[RaidLoopSmoke] RESULT 0/{Total} — timed out waiting for the loop", 15.0);
    }

    private ARaidObjective FindRaidObjective()
    {
        TArray<ARaidObjective> Objectives;
        GetAllActorsOfClass(Objectives);
        return Objectives.Num() > 0 ? Objectives[0] : nullptr;
    }

    private void RaidLoopVictoryStep()
    {
        ARaidObjective Obj = FindRaidObjective();
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (Obj == nullptr || Combat == nullptr || GS == nullptr)
        {
            RaidLoopRetry();
            return;
        }

        if (Obj.Phase == ERaidPhase::InProgress)
        {
            // Wait until the gating elites have actually spawned, then arm a tiny defend wave and
            // nuke the arena so the objective's own clear-detection opens extraction.
            if (Combat.GetEliteCount() <= 0)
            {
                RaidLoopRetry();
                return;
            }
            Obj.DefendWaveEliteClass = ACarapace;   // prove the spawner; shipping default is gated (plan Task 3)
            Obj.ExtractionDefendSeconds = 0.1;       // collapse the hold so the boot test is quick
            AHeroCharacter Hero = GetHero();
            FVector Center = Hero != nullptr ? Hero.GetActorLocation() : Obj.GetActorLocation();
            Combat.ApplyRadialDamage(Center, 1000000.0, 999999.0, 1.0, Hero);
            RaidLoopRetry();
            return;
        }

        if (Obj.Phase == ERaidPhase::ExtractionReady)
        {
            int Before = Combat.GetEliteCount();
            Obj.CallExtraction();
            bRaidLoopExtracting = (Obj.Phase == ERaidPhase::Extracting);
            int After = Combat.GetEliteCount();
            bRaidLoopWaveSpawned = (After - Before) >= Math::Max(1, Obj.DefendWaveCount / 2);
            RaidLoopRetry();
            return;
        }

        if (Obj.Phase == ERaidPhase::Extracting)
        {
            // Defend timer is ticking down (0.1s); just wait for it to expire into Extracted.
            RaidLoopRetry();
            return;
        }

        if (Obj.Phase == ERaidPhase::Extracted)
        {
            int Pass = 0;
            if (bRaidLoopExtracting) Pass++;
            else Print("[RaidLoopSmoke] FAIL: CallExtraction did not enter Extracting", 15.0);
            if (bRaidLoopWaveSpawned) Pass++;
            else Print("[RaidLoopSmoke] FAIL: defend wave did not spawn on extraction", 15.0);
            Pass++;   // reached Extracted = survived the hold
            if (GS.Phase == ERunPhase::Victory) Pass++;
            else Print(f"[RaidLoopSmoke] FAIL: run phase is not Victory (got {GS.Phase})", 15.0);
            Print(f"[RaidLoopSmoke] RESULT {Pass}/4", 15.0);
            return;
        }

        // Failed during a victory run (e.g. nothing spawned) — surface it rather than hang.
        Print("[RaidLoopSmoke] RESULT 0/4 — objective reached an unexpected terminal state", 15.0);
    }

    private void RaidLoopDefeatStep()
    {
        ARaidObjective Obj = FindRaidObjective();
        ARaidGameState GS = Cast<ARaidGameState>(Gameplay::GetGameState());
        if (Obj == nullptr || GS == nullptr)
        {
            RaidLoopRetry();
            return;
        }

        // Trigger the loss bridge directly (DownComponent already covers the down/revive path that
        // calls this once every hero is incapacitated). NotifyPartyWiped -> Failed -> Defeat is
        // synchronous, so we can assert right after.
        if (Obj.Phase != ERaidPhase::Failed)
            Obj.NotifyPartyWiped();

        if (Obj.Phase != ERaidPhase::Failed)
        {
            RaidLoopRetry();   // give it a tick if it hasn't applied yet
            return;
        }

        int Pass = 1;   // reached Failed
        if (GS.Phase == ERunPhase::Defeat) Pass++;
        else Print(f"[RaidLoopSmoke] FAIL: run phase is not Defeat (got {GS.Phase})", 15.0);
        Print(f"[RaidLoopSmoke] RESULT {Pass}/2", 15.0);
    }

    // --- End-of-run results (CommonUI: pushed onto the Menu layer). Triggered by the HUD's
    // banner->panel timer, or instantly via the RaidResults console command. ---
    private UResultsScreenWidget ResultsScreen;

    void ShowResultsScreen()
    {
        if (Layout == nullptr || ResultsScreen != nullptr)
            return;
        ResultsScreen = Cast<UResultsScreenWidget>(
            Layout.PushToLayer(ERogueUILayer::Menu, UResultsScreenWidget));
    }

    UFUNCTION(Exec)
    void RaidResults()
    {
        ShowResultsScreen();
        if (HUDWidget != nullptr)
            HUDWidget.HideResultBanner();
    }

    // --- Escape menu (Esc / P, or this console command). An overlay — nothing pauses.
    // CommonUI: pushed onto the Menu layer; the back action (Esc/B) pops it, any deactivation
    // path notifies us via NotifyPauseMenuClosed. Cursor/input mode = the widget's config. ---
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
        if (Layout == nullptr)
            return;
        PauseMenu = Cast<UEscapeMenuWidget>(
            Layout.PushToLayer(ERogueUILayer::Menu, UEscapeMenuWidget));
        if (PauseMenu != nullptr)
            Print("[Menu] escape menu shown", 2.0);
    }

    void ClosePauseMenu()
    {
        if (PauseMenu != nullptr)
            PauseMenu.DeactivateWidget();   // stack pops it; OnDeactivated calls back below
    }

    // Called from the widget's OnDeactivated — covers RESUME, the Esc/B back action, and travel.
    void NotifyPauseMenuClosed()
    {
        PauseMenu = nullptr;
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
