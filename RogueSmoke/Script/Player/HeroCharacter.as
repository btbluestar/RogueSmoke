// HeroCharacter.as
// Base hero: third-person shooter camera, strafe/aim-locked (D-0014). Now a GAS pawn — subclasses ARogueHeroBase (C++), which
// bridges to the PlayerState-owned AbilitySystemComponent (Lyra-style) and fires OnAbilitySystemReady
// once the ASC actor info is initialized. Heroes are data-driven: each assigns a URogueAbilitySet
// (abilities + effects + attribute sets) and a URogueInputConfig (input tag -> ability).
//
// Input lives on ARaidPlayerController (Controller = the player; Pawn = the body). The controller
// maps ability inputs to gameplay tags and routes activation through Server_ActivateAbilityInput.
class AHeroCharacter : ARogueHeroBase
{
    // Third-person shooter camera (D-0014). Boom sits behind the character with a shoulder offset
    // and follows the control rotation, so aiming yaws/pitches the camera over the shoulder.
    UPROPERTY(DefaultComponent)
    USpringArmComponent CameraBoom;
    default CameraBoom.TargetArmLength = 350.0;                   // over-the-shoulder distance (GDD §9)
    default CameraBoom.SocketOffset = FVector(0.0, 60.0, 60.0);   // shoulder offset (right + up)
    default CameraBoom.bUsePawnControlRotation = true;            // aim drives the boom
    default CameraBoom.bEnableCameraLag = true;

    UPROPERTY(DefaultComponent, Attach = CameraBoom)
    UCameraComponent FollowCamera;

    // Strafe/aim-locked: the body faces the control (aim) rotation, not the move direction (D-0014).
    default bUseControllerRotationYaw = true;
    default bUseControllerRotationPitch = false;
    default bUseControllerRotationRoll = false;
    default CharacterMovement.bOrientRotationToMovement = false;

    // Align the skeletal mesh to the capsule: the mannequin's pivot is at its feet, so drop it by
    // the capsule half-height (88) to plant the feet on the capsule bottom (which rests on the floor).
    // Yaw -90 faces the mesh forward down the capsule's +X. Hero BPs assign the mesh asset only.
    default Mesh.RelativeLocation = FVector(0.0, 0.0, -88.0);
    default Mesh.RelativeRotation = FRotator(0.0, -90.0, 0.0);

    // Per-hero kit: abilities + effects + attribute sets granted on possession. Assign on the BP hero.
    UPROPERTY(EditDefaultsOnly, Category = "Ability System")
    URogueAbilitySet AbilitySet;

    // Input-bound abilities granted from AbilitySet (server-side), looked up by tag on activation.
    private TArray<FRogueGrantedAbility> GrantedAbilities;

    // --- Movement (D-0015): sprint / crouch / slide / double-jump state machine. ---
    UPROPERTY(DefaultComponent)
    URogueLocomotionComponent Locomotion;

    // --- Shooting (modular weapon system) ---
    // Runtime weapon state (ammo/heat/spread/reload). Logic only; the weapon mesh is content on the BP.
    UPROPERTY(DefaultComponent)
    URogueWeaponComponent Weapon;

    // The weapon this hero spawns with. Assign a DA_Weapon_* on the BP hero.
    UPROPERTY(EditDefaultsOnly, Category = "Weapon")
    URogueWeaponDefinition DefaultWeapon;

    // The input tag GA_WeaponFire is granted under (set to InputTag.Weapon.Fire on the BP hero).
    UPROPERTY(EditDefaultsOnly, Category = "Weapon")
    FGameplayTag FireInputTag;

    // Server-only: is the fire input currently held (drives full-auto refire in Tick).
    private bool bWantsToFire = false;

    // Visible weapon mesh, attached to the right hand. Its muzzle socket (WeaponDefinition.MuzzleSocket)
    // is the true bullet origin for third-person convergence (D-0014). Mesh asset comes from the
    // equipped definition; assigned on all machines so clients see the gun too.
    UPROPERTY(DefaultComponent)
    USkeletalMeshComponent WeaponMesh;
    default WeaponMesh.SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // --- Focus / light ADS (D-0014). bFocusing gates the authoritative spread (server reads it when
    // firing) and the local camera zoom; the strafe slow lives on the locomotion component so the
    // owning client can predict it. The camera zoom itself is owning-client cosmetic only. ---
    UPROPERTY(Replicated)
    bool bFocusing = false;

    private float FocusAlpha = 0.0;          // owning-client camera blend 0..1
    const float BaseCameraFOV = 90.0;
    const float BaseArmLength = 350.0;       // mirrors CameraBoom.TargetArmLength default
    const float FocusBlendSpeed = 9.0;       // FInterp speed for the zoom blend

    // World time of the last confirmed enemy hit on the owning client; the HUD flashes a hitmarker.
    UPROPERTY(BlueprintReadOnly, Category = "Weapon")
    float LastHitConfirmTime = -100.0;

    // --- Down/revive (MVP lose condition, D-0010). Logic lives in URogueDownComponent; the
    // replicated life-state lives here on the pawn so teammates see/skip downed allies. ---
    UPROPERTY(DefaultComponent)
    URogueDownComponent Down;

    // DOWNED = 0 HP, bleeding out, revivable. DEAD = bled out (run-dead). Either => incapacitated:
    // no movement/abilities/fire. Both replicate; OnRep applies the cosmetic stop on clients.
    UPROPERTY(Replicated, ReplicatedUsing = OnRep_Incap, BlueprintReadOnly, Category = "Down")
    bool bDowned = false;

    UPROPERTY(Replicated, ReplicatedUsing = OnRep_Incap, BlueprintReadOnly, Category = "Down")
    bool bDeadDowned = false;

    bool IsDowned() const { return bDowned; }
    bool IsIncapacitated() const { return bDowned || bDeadDowned; }

    // Server only (called by URogueDownComponent). Updates the host's cosmetics immediately;
    // clients get it through OnRep_Incap.
    void SetDownedState(bool bInDowned, bool bInDead)
    {
        bDowned = bInDowned;
        bDeadDowned = bInDead;
        ApplyIncapacitatedState();
    }

    UFUNCTION()
    void OnRep_Incap() { ApplyIncapacitatedState(); }

    // Halt the body + cut held fire while incapacitated. (Crawl/ragdoll pose is later polish.)
    private void ApplyIncapacitatedState()
    {
        if (IsIncapacitated())
        {
            bWantsToFire = false;
            CharacterMovement.StopMovementImmediately();
            Locomotion.ResetAirState();
        }
    }

    // Called by ARogueHeroBase once the ASC is initialized for this pawn (server + clients).
    UFUNCTION(BlueprintOverride)
    void OnAbilitySystemReady()
    {
        UAngelscriptAbilitySystemComponent ASC = GetRogueAbilitySystem();
        if (ASC == nullptr)
            return;

        // Server: grant the kit (registers attribute sets, applies initial effects, grants abilities)
        // and equip the default weapon.
        if (HasAuthority())
        {
            if (AbilitySet != nullptr)
            {
                GrantedAbilities.Empty();
                AbilitySet.GiveToAbilitySystem(ASC, GrantedAbilities);
            }
            if (Weapon != nullptr && DefaultWeapon != nullptr)
                Weapon.EquipWeapon(DefaultWeapon);

            // Weapon-state upgrades (fire rate / reload / magazine) live on the server-only weapon
            // component; mirror the attributes into it and keep them live (same pattern as MoveSpeed
            // below, but server-only since that's where weapon timing runs).
            PushWeaponBonuses();
            ASC.RegisterAttributeChangedCallback(URogueCombatSet, n"FireRateBonus", this, n"OnWeaponBonusChanged");
            ASC.RegisterAttributeChangedCallback(URogueCombatSet, n"ReloadSpeedBonus", this, n"OnWeaponBonusChanged");
            ASC.RegisterAttributeChangedCallback(URogueCombatSet, n"MagazineBonus", this, n"OnWeaponBonusChanged");
        }

        // Server + clients: the locomotion component owns MaxWalkSpeed (base + sprint/crouch). Seed its
        // base from the MoveSpeed attribute (the grant registers it on the server and replicates), then
        // keep it live so the Swift upgrade (a MoveSpeed GE) actually takes effect — the callback fires
        // on the server when the GE applies and on clients via the attribute's replication.
        Locomotion.Initialize(this);
        float CurrentMoveSpeed = ASC.GetAttributeCurrentValue(URogueCombatSet, n"MoveSpeed", 600.0);
        Locomotion.SetBaseSpeed(CurrentMoveSpeed);
        ASC.RegisterAttributeChangedCallback(URogueCombatSet, n"MoveSpeed", this, n"OnMoveSpeedChanged");

        // Down/revive: subscribes (server) to Health hitting 0. Self-gates to authority.
        Down.Initialize(this);

        // Visible weapon mesh (all machines): attach to the right hand and set the asset from the class
        // default (available everywhere even though the runtime Weapon.Definition is server-only today).
        WeaponMesh.AttachToComponent(Mesh, n"hand_r", EAttachmentRule::SnapToTarget,
            EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget, false);
        if (DefaultWeapon != nullptr && DefaultWeapon.WeaponMesh != nullptr)
            WeaponMesh.SetSkeletalMeshAsset(DefaultWeapon.WeaponMesh);
    }

    // True bullet origin for third-person convergence: the weapon muzzle socket when the mesh + socket
    // exist, else a shoulder-height offset toward aim so shooting works before weapon art is assigned.
    // Called server-side from the fire ability (where Weapon.Definition is valid).
    FVector GetMuzzleLocation() const
    {
        if (WeaponMesh.GetSkeletalMeshAsset() != nullptr && Weapon != nullptr && Weapon.Definition != nullptr
            && WeaponMesh.DoesSocketExist(Weapon.Definition.MuzzleSocket))
        {
            return WeaponMesh.GetSocketLocation(Weapon.Definition.MuzzleSocket);
        }
        FRotator AimYaw = FRotator(0.0, GetControlRotation().Yaw, 0.0);
        return GetActorLocation()
            + AimYaw.GetForwardVector() * 60.0
            + AimYaw.GetRightVector() * 30.0
            + FVector(0.0, 0.0, 40.0);
    }

    // Keep locomotion's base speed in sync with the MoveSpeed attribute (e.g. the Swift upgrade).
    UFUNCTION()
    void OnMoveSpeedChanged(FAngelscriptAttributeChangedData Data)
    {
        Locomotion.SetBaseSpeed(Data.GetNewValue());
    }

    // Any of the three weapon-state attributes changed (an upgrade GE applied): re-read all three
    // and push them into the weapon component. Server-only by registration.
    UFUNCTION()
    void OnWeaponBonusChanged(FAngelscriptAttributeChangedData Data)
    {
        PushWeaponBonuses();
    }

    private void PushWeaponBonuses()
    {
        UAngelscriptAbilitySystemComponent ASC = GetRogueAbilitySystem();
        if (ASC == nullptr || Weapon == nullptr)
            return;
        Weapon.SetUpgradeBonuses(
            ASC.GetAttributeCurrentValue(URogueCombatSet, n"FireRateBonus", 0.0),
            ASC.GetAttributeCurrentValue(URogueCombatSet, n"ReloadSpeedBonus", 0.0),
            ASC.GetAttributeCurrentValue(URogueCombatSet, n"MagazineBonus", 0.0));
    }

    // Called by ARaidPlayerController from the IA_Move action value.
    // Camera-relative strafe movement (D-0014): forward/right are taken from the control yaw so
    // input is relative to where the player is aiming, independent of the body's facing.
    UFUNCTION(BlueprintCallable)
    void DoMove(FVector2D Axis)
    {
        if (IsIncapacitated())
            return;
        FRotator YawRotation(0.0, GetControlRotation().Yaw, 0.0);
        AddMovementInput(YawRotation.GetForwardVector(), Axis.Y);
        AddMovementInput(YawRotation.GetRightVector(), Axis.X);
    }

    // --- Movement input (D-0015). Called by ARaidPlayerController on the owning client. ---
    // Jump/double-jump is natively client-predicted by ACharacter (saved moves), so no Server_ mirror
    // is needed; JumpMaxCount (set by the locomotion component) gates the second jump.
    UFUNCTION(BlueprintCallable)
    void DoJump()
    {
        if (IsIncapacitated())
            return;
        // Slide-hop: jumping out of a slide keeps 100% of horizontal velocity (D-0015 rework).
        if (Locomotion.IsSliding())
            Locomotion.NotifySlideJump();
        // Auto-stand like Apex/Deadlock: stock CanJump refuses while crouched, and UnCrouch
        // is a no-op when already standing. Landing re-entry keys off the held crouch input.
        UnCrouch();
        Jump();
    }

    // Fires on the predicted client and the server; idempotent (sets the second jump's Z).
    UFUNCTION(BlueprintOverride)
    void OnJumped()
    {
        Locomotion.NotifyJumped(JumpCurrentCount);
    }

    // Fires on the server and the owning client. Slide re-entry + camera/anim get the fall speed.
    UFUNCTION(BlueprintOverride)
    void OnLanded(FHitResult Hit)
    {
        float FallSpeed = Math::Abs(CharacterMovement.Velocity.Z);
        Locomotion.NotifyLanded(FallSpeed);
    }

    UFUNCTION(BlueprintCallable)
    void DoStopJump() { StopJumping(); }

    // Sprint/crouch change CMC values, so apply locally (prediction) AND mirror on the server so its
    // simulation matches and doesn't correct the client back.
    void SetSprint(bool bWants)
    {
        if (IsIncapacitated())
            return;
        Locomotion.SetSprint(bWants);
        Server_SetSprint(bWants);
    }

    UFUNCTION(Server)
    void Server_SetSprint(bool bWants)
    {
        Locomotion.SetSprint(bWants);
    }

    void CrouchPressed()
    {
        if (IsIncapacitated())
            return;
        Locomotion.RequestCrouchOrSlide();
        Server_CrouchPressed();
    }

    UFUNCTION(Server)
    void Server_CrouchPressed()
    {
        Locomotion.RequestCrouchOrSlide();
    }

    void CrouchReleased()
    {
        Locomotion.ReleaseCrouch();
        Server_CrouchReleased();
    }

    UFUNCTION(Server)
    void Server_CrouchReleased()
    {
        Locomotion.ReleaseCrouch();
    }

    // Focus / light ADS (hold). Local predicts the spread/zoom/move-slow; the server mirror makes the
    // authoritative spread + move-speed match (same shape as sprint). Camera zoom is owning-client only.
    void SetFocus(bool bWants)
    {
        bool bWant = bWants && !IsIncapacitated();
        bFocusing = bWant;
        Locomotion.SetFocus(bWant);
        Server_SetFocus(bWant);
    }

    UFUNCTION(Server)
    void Server_SetFocus(bool bWants)
    {
        bFocusing = bWants;
        Locomotion.SetFocus(bWants);
    }

    // Owning-client cosmetic: blend the focus camera (FOV zoom + boom pull-in) toward the focus state.
    private void UpdateFocusCamera(float DeltaSeconds)
    {
        float Target = bFocusing ? 1.0 : 0.0;
        FocusAlpha = Math::FInterpTo(FocusAlpha, Target, DeltaSeconds, FocusBlendSpeed);

        float TargetFOV = 70.0;          // fallbacks for remote clients (Weapon.Definition is server-only)
        float TargetArm = 220.0;
        if (Weapon != nullptr && Weapon.Definition != nullptr)
        {
            TargetFOV = Weapon.Definition.FocusFOV;
            TargetArm = Weapon.Definition.FocusArmLength;
        }
        FollowCamera.FieldOfView = Math::Lerp(BaseCameraFOV, TargetFOV, FocusAlpha);
        CameraBoom.TargetArmLength = Math::Lerp(BaseArmLength, TargetArm, FocusAlpha);
    }

    // Server: drive weapon timing and full-auto refire. Overriding Tick is what makes the class tick.
    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        // Slide physics run wherever movement is simulated (predicted client + authority).
        if (IsLocallyControlled() || HasAuthority())
            Locomotion.TickLocomotion(DeltaSeconds);

        // Owning client: blend the focus camera zoom (cosmetic, local only).
        if (IsLocallyControlled())
            UpdateFocusCamera(DeltaSeconds);

        if (HasAuthority())
        {
            // Down/revive bleed-out + revive-proximity (server-authoritative).
            Down.TickDown(DeltaSeconds);

            if (Weapon != nullptr)
            {
                Weapon.TickWeapon(DeltaSeconds);

                // Full-auto: keep re-activating the fire ability while held; CanFire() gates the rate.
                if (bWantsToFire && Weapon.Definition != nullptr && Weapon.Definition.bFullAuto)
                    ActivateGrantedAbility(FireInputTag);
            }
        }
    }

    // Resolve an input tag to the granted ability spec and activate it (server-authoritative).
    private void ActivateGrantedAbility(FGameplayTag InputTag)
    {
        if (IsIncapacitated())
            return;
        UAngelscriptAbilitySystemComponent ASC = GetRogueAbilitySystem();
        if (ASC == nullptr)
            return;

        for (const FRogueGrantedAbility& Granted : GrantedAbilities)
        {
            if (Granted.InputTag == InputTag)
            {
                ASC.TryActivateAbilitySpec(Granted.Handle, false);   // already on server
                return;
            }
        }
    }

    // Client intent -> server activation. The controller calls this when an ability input fires.
    UFUNCTION(Server)
    void Server_ActivateAbilityInput(FGameplayTag InputTag)
    {
        ActivateGrantedAbility(InputTag);
    }

    // Fire input pressed/released. On press, fire immediately (semi + first full-auto shot); the held
    // flag drives full-auto in Tick. D-0014 shooter.
    UFUNCTION(Server)
    void Server_SetWantsToFire(bool bWants)
    {
        if (IsIncapacitated())
        {
            bWantsToFire = false;
            return;
        }
        bWantsToFire = bWants;
        if (bWants)
            ActivateGrantedAbility(FireInputTag);
    }

    // Manual reload (R). Auto-reload also fires when the magazine empties (WeaponComponent.NotifyFired).
    UFUNCTION(Server)
    void Server_RequestReload()
    {
        if (Weapon != nullptr)
            Weapon.StartReload();
    }

    // Cosmetic, fire-and-forget: muzzle tracer(s) on all machines + recoil and a hitmarker on the
    // owning client. MuzzleLocation is the gun muzzle (computed server-side) so tracers come from the
    // weapon, not the camera (D-0014). bHitEnemy flags that a pellet damaged an enemy this shot.
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_FireFX(FVector MuzzleLocation, TArray<FVector> Impacts, bool bHitEnemy)
    {
        for (FVector Impact : Impacts)
            System::DrawDebugLine(MuzzleLocation, Impact, FLinearColor(1.0, 1.0, 0.0), 0.05, 2.0);

        if (IsLocallyControlled())
        {
            if (Weapon != nullptr && Weapon.Definition != nullptr)
            {
                AddControllerPitchInput(-Weapon.Definition.RecoilPitchPerShot);
                AddControllerYawInput(Math::RandRange(-Weapon.Definition.RecoilYawRange, Weapon.Definition.RecoilYawRange));
            }
            if (bHitEnemy)
                LastHitConfirmTime = Gameplay::GetTimeSeconds();
        }
    }

    // Apply a chosen upgrade server-side. The GameMode validates the card against the player's
    // offered hand (client intent never trusted), applies the GE, and resumes the raid when
    // everyone has picked. D-0010 / D-0019.
    UFUNCTION(Server)
    void Server_ApplyUpgrade(URogueUpgradeDef Upgrade)
    {
        ARaidGameMode GameMode = Cast<ARaidGameMode>(Gameplay::GetGameMode());
        ARaidPlayerController PC = Cast<ARaidPlayerController>(GetController());
        if (GameMode != nullptr && PC != nullptr)
            GameMode.ApplyUpgradeFor(PC, Upgrade);
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
