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
        int Total = 6;

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
