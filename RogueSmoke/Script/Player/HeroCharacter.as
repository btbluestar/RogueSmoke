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
        }

        // Server + clients: the locomotion component owns MaxWalkSpeed (base + sprint/crouch). Seed its
        // base from the MoveSpeed attribute, which the grant registers on the server and replicates.
        // NOTE (MVP gap): set once here, not live-updated — no current upgrade changes MoveSpeed.
        // When one does, re-call Locomotion.SetBaseSpeed from a MoveSpeed attribute-changed callback.
        Locomotion.Initialize(this);
        float CurrentMoveSpeed = ASC.GetAttributeCurrentValue(URogueCombatSet, n"MoveSpeed", 600.0);
        Locomotion.SetBaseSpeed(CurrentMoveSpeed);
    }

    // Called by ARaidPlayerController from the IA_Move action value.
    // Camera-relative strafe movement (D-0014): forward/right are taken from the control yaw so
    // input is relative to where the player is aiming, independent of the body's facing.
    UFUNCTION(BlueprintCallable)
    void DoMove(FVector2D Axis)
    {
        FRotator YawRotation(0.0, GetControlRotation().Yaw, 0.0);
        AddMovementInput(YawRotation.GetForwardVector(), Axis.Y);
        AddMovementInput(YawRotation.GetRightVector(), Axis.X);
    }

    // --- Movement input (D-0015). Called by ARaidPlayerController on the owning client. ---
    // Jump/double-jump is natively client-predicted by ACharacter (saved moves), so no Server_ mirror
    // is needed; JumpMaxCount (set by the locomotion component) gates the second jump.
    UFUNCTION(BlueprintCallable)
    void DoJump() { Jump(); }

    UFUNCTION(BlueprintCallable)
    void DoStopJump() { StopJumping(); }

    // Sprint/crouch change CMC values, so apply locally (prediction) AND mirror on the server so its
    // simulation matches and doesn't correct the client back.
    void SetSprint(bool bWants)
    {
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

    // Server: drive weapon timing and full-auto refire. Overriding Tick is what makes the class tick.
    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        // Slide physics run wherever movement is simulated (predicted client + authority).
        if (IsLocallyControlled() || HasAuthority())
            Locomotion.TickLocomotion(DeltaSeconds);

        if (HasAuthority() && Weapon != nullptr)
        {
            Weapon.TickWeapon(DeltaSeconds);

            // Full-auto: keep re-activating the fire ability while held; CanFire() gates the rate.
            if (bWantsToFire && Weapon.Definition != nullptr && Weapon.Definition.bFullAuto)
                ActivateGrantedAbility(FireInputTag);
        }
    }

    // Resolve an input tag to the granted ability spec and activate it (server-authoritative).
    private void ActivateGrantedAbility(FGameplayTag InputTag)
    {
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

    // Cosmetic, fire-and-forget: tracer(s) on all machines + recoil on the owning client only.
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_FireFX(FVector MuzzleLocation, TArray<FVector> Impacts)
    {
        for (FVector Impact : Impacts)
            System::DrawDebugLine(MuzzleLocation, Impact, FLinearColor(1.0, 1.0, 0.0), 0.05, 2.0);

        if (IsLocallyControlled() && Weapon != nullptr && Weapon.Definition != nullptr)
        {
            AddControllerPitchInput(-Weapon.Definition.RecoilPitchPerShot);
            AddControllerYawInput(Math::RandRange(-Weapon.Definition.RecoilYawRange, Weapon.Definition.RecoilYawRange));
        }
    }

    // Apply a chosen upgrade server-side: the upgrade's GameplayEffect is applied to the player ASC,
    // modifying attributes authoritatively. Called by UpgradeSelectWidget on the owning client. D-0010.
    UFUNCTION(Server)
    void Server_ApplyUpgrade(URogueUpgradeDef Upgrade)
    {
        if (Upgrade == nullptr || Upgrade.Effect.Get() == nullptr)
            return;

        UAngelscriptAbilitySystemComponent ASC = GetRogueAbilitySystem();
        if (ASC != nullptr)
            ASC.ApplyGameplayEffectToTarget(Upgrade.Effect, ASC, 1.0, FGameplayEffectContextHandle());
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
