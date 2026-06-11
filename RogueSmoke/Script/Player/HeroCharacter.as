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
    default CameraBoom.bEnableCameraLag = false;                  // camera bolted to the pawn — lag smoothing eats the spring kick/dip displacements, so the feel layer needs a rigid camera

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

    // --- Camera juice (owning-client cosmetic): focus zoom, FOV kicks, fire kick, landing dip. ---
    UPROPERTY(DefaultComponent)
    URogueCameraFeelComponent CameraFeel;

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

    // Server-only: did a shot actually fire during this trigger hold? Gates the release tail so an
    // empty-mag press+release can't ring a tail without a bang.
    private bool bFiredSinceHeld = false;

    // Upper-body feedback montages (assigned on the hero BPs; UpperBody slot in ABP_Hero).
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    UAnimMontage FireMontage;

    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    UAnimMontage ReloadMontage;

    // Server-only edge detector: WeaponComponent starts reloads internally (auto-reload on empty),
    // so the hero polls the transition in Tick rather than hooking every call site.
    private bool bWasReloading = false;

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

    // --- Idle free-look (anim feel): at true idle the camera orbits without twisting the body
    // (no more feet shuffling on the spot). The camera-yaw hard lock resumes — with a quick
    // catch-up turn the strafe blendspace masks — whenever the body must face the camera again:
    // move input, airborne, firing, focusing, sliding, or the camera swinging past the deadband. ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Facing")
    float IdleFreeLookYawLimit = 100.0;    // deg the camera may swing before dragging the body

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Facing")
    float IdleAlignYawRate = 540.0;        // deg/s catch-up turn speed

    // Lyra linked-layer (D-0022): the per-weapon anim overlay linked onto the base ABP at spawn.
    // Swapping weapons later = LinkAnimClassLayers with a different layer class; data, not graph work.
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    TSubclassOf<UAnimInstance> WeaponAnimLayer;

    // v1 actor-level idle free-look. OFF under the Lyra stack: RootYawOffset + authored
    // turn-in-place owns feet-planted idle now. Flip back on only to A/B the v1 ABP.
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    bool bActorLevelFreeLook = false;

    // GASP slide set (retargeted onto the Lyra skeleton) played as dynamic montages on the
    // full-body slot — slide is OUR mechanic, Lyra's graph knows nothing about it (D-0022).
    UPROPERTY(EditDefaultsOnly, Category = "Animation|Slide")
    UAnimSequenceBase SlideInAnim;

    UPROPERTY(EditDefaultsOnly, Category = "Animation|Slide")
    UAnimSequenceBase SlideLoopAnim;

    UPROPERTY(EditDefaultsOnly, Category = "Animation|Slide")
    UAnimSequenceBase SlideOutStandAnim;

    UPROPERTY(EditDefaultsOnly, Category = "Animation|Slide")
    UAnimSequenceBase SlideOutCrouchAnim;

    UPROPERTY(EditDefaultsOnly, Category = "Animation|Slide")
    UAnimSequenceBase SlideOutRunAnim;

    UPROPERTY(EditDefaultsOnly, Category = "Animation|Slide")
    FName SlideMontageSlot = n"DefaultSlot";

    private bool bSlideAnimActive = false;
    private float SlideLoopStartTime = 0.0;

    // Owner+server mirror of held-fire intent: bWantsToFire is server-only, but facing must
    // agree on the predicting owner too or the body snaps on correction.
    private bool bFireHeldForFacing = false;

    // World time of the last confirmed enemy hit on the owning client; the HUD flashes a hitmarker.
    UPROPERTY(BlueprintReadOnly, Category = "Weapon")
    float LastHitConfirmTime = -100.0;

    // World time of the last confirmed killing blow by this hero (owning client); HUD pops the marker.
    UPROPERTY(BlueprintReadOnly, Category = "Weapon")
    float LastKillConfirmTime = -100.0;

    // Server-only: this player's replicated kill stat as of the last credit check (kill-confirm seam).
    private int LastKillStatSeen = 0;

    // Server: the GameMode calls this on every hero when a pooled enemy dies. The C++ death
    // delegates (FOnDeath / SpawnDirector.OnEnemyKilled) drop the instigator, but
    // UHealthComponent::ApplyDamage credited the killer's PlayerState.Kills BEFORE broadcasting
    // OnDeath — so "my kill stat just rose" IS the kill attribution, with no C++ change.
    void NotifyKillCreditCheck()
    {
        if (!HasAuthority())
            return;
        ARoguePlayerState PS = Cast<ARoguePlayerState>(PlayerState);
        if (PS == nullptr)
            return;
        if (PS.Kills > LastKillStatSeen)
            Client_KillConfirm();
        LastKillStatSeen = PS.Kills;
    }

    // Cosmetic kill confirmation to the killing player only (research B: third distinct confirm
    // layer, the loudest of the three).
    UFUNCTION(Client, Unreliable)
    void Client_KillConfirm()
    {
        LastKillConfirmTime = Gameplay::GetTimeSeconds();
        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        if (Def != nullptr && Def.KillConfirmSound != nullptr)
            Gameplay::SpawnSound2D(Def.KillConfirmSound);
    }

    // Floating damage numbers waiting for the HUD to spawn them (owning client only).
    private TArray<FVector> PendingDamageLocs;
    private TArray<float> PendingDamageAmounts;

    // Cosmetic, per-hit damage feedback to the shooter only. Unreliable: a lost number is noise.
    UFUNCTION(Client, Unreliable)
    void Client_DamageNumbers(TArray<FVector> Locations, TArray<float> Amounts)
    {
        int Count = Math::Min(Locations.Num(), Amounts.Num());
        for (int i = 0; i < Count; i++)
        {
            PendingDamageLocs.Add(Locations[i]);
            PendingDamageAmounts.Add(Amounts[i]);
        }
    }

    // The HUD drains the buffer once per frame.
    void TakePendingDamageNumbers(TArray<FVector>& OutLocs, TArray<float>& OutAmounts)
    {
        OutLocs = PendingDamageLocs;
        OutAmounts = PendingDamageAmounts;
        PendingDamageLocs.Empty();
        PendingDamageAmounts.Empty();
    }

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
            // Route through the stop-edge detector so a held trigger cut by going down still
            // rings the tail out (server only; on clients the flag is always false — no edge).
            SetHeldFire(false);
            SetFireHeldForFacing(false);
            CharacterMovement.StopMovementImmediately();
            Locomotion.ResetAirState();
        }
    }

    // Owner + server: the facing logic needs the held-fire intent on both machines.
    void SetFireHeldForFacing(bool bHeld) { bFireHeldForFacing = bHeld; }
    bool IsFireHeldForFacing() const { return bFireHeldForFacing; }

    // Server: write the held-fire flag through the stop-edge detector — a held->released
    // transition rings the gun tail out on all machines (full-auto fatigue fix).
    private void SetHeldFire(bool bWants)
    {
        bool bWas = bWantsToFire;
        bWantsToFire = bWants;
        if (bWas && !bWants && HasAuthority() && bFiredSinceHeld)
            Multicast_FireStopped();
        if (!bWants)
            bFiredSinceHeld = false;
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

            // Kill-confirm cache: start from the PS's current count so a pre-existing stat
            // (pawn swap, travel) can't fire a stale confirm on the first death.
            ARoguePlayerState RPS = Cast<ARoguePlayerState>(PlayerState);
            if (RPS != nullptr)
                LastKillStatSeen = RPS.Kills;

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

        // Camera feel: hand it the camera rig (owning-client cosmetic; ticks from the hero Tick).
        CameraFeel.Initialize(this, CameraBoom, FollowCamera);

        // Down/revive: subscribes (server) to Health hitting 0. Self-gates to authority.
        Down.Initialize(this);

        // Visible weapon mesh (all machines): attach to the right hand and set the asset from the class
        // default (available everywhere even though the runtime Weapon.Definition is server-only today).
        // Lyra mannequin carries an authored grip socket (weapon_r on hand_r); fall back to the
        // bare hand bone on meshes that lack it.
        FName GripSocket = Mesh.DoesSocketExist(n"weapon_r") ? n"weapon_r" : n"hand_r";
        WeaponMesh.AttachToComponent(Mesh, GripSocket, EAttachmentRule::SnapToTarget,
            EAttachmentRule::SnapToTarget, EAttachmentRule::SnapToTarget, false);
        if (DefaultWeapon != nullptr && DefaultWeapon.WeaponMesh != nullptr)
        {
            WeaponMesh.SetSkeletalMeshAsset(DefaultWeapon.WeaponMesh);
            if (DefaultWeapon.WeaponAnimClass.IsValid())
                WeaponMesh.SetAnimInstanceClass(DefaultWeapon.WeaponAnimClass);
        }

        // Lyra linked-layer: overlay the held weapon's anim set onto the base locomotion graph.
        if (WeaponAnimLayer.IsValid())
            Mesh.LinkAnimClassLayers(WeaponAnimLayer);
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

    // The weapon definition for cosmetic use on ANY machine: runtime definition where valid
    // (server/host), else the class-default DefaultWeapon (same fallback as WeaponMesh).
    URogueWeaponDefinition GetCosmeticWeaponDef() const
    {
        if (Weapon != nullptr && Weapon.Definition != nullptr)
            return Weapon.Definition;
        return DefaultWeapon;
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
        if (IsLocallyControlled())
            CameraFeel.NotifyLanded(FallSpeed);
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

    // Server: drive weapon timing and full-auto refire. Overriding Tick is what makes the class tick.
    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        // Slide physics run wherever movement is simulated (predicted client + authority).
        if (IsLocallyControlled() || HasAuthority())
        {
            // Anim edge-detect BEFORE the locomotion tick: a slide that starts and ends across
            // one frame boundary must still register its start edge here.
            TickSlideAnim();
            Locomotion.TickLocomotion(DeltaSeconds);
            TickFacing(DeltaSeconds);
        }

        // Owning client: camera feel (focus zoom + FOV kicks + fire kick + landing dip; cosmetic, local only).
        if (IsLocallyControlled())
            CameraFeel.TickCameraFeel(DeltaSeconds);

        if (HasAuthority())
        {
            // Down/revive bleed-out + revive-proximity (server-authoritative).
            Down.TickDown(DeltaSeconds);

            if (Weapon != nullptr)
            {
                Weapon.TickWeapon(DeltaSeconds);

                // Reload started this frame (manual or auto): cosmetic montage everywhere.
                bool bReloadingNow = Weapon.IsReloading();
                if (bReloadingNow && !bWasReloading)
                    Multicast_ReloadFX();
                bWasReloading = bReloadingNow;

                // Full-auto: keep re-activating the fire ability while held; CanFire() gates the rate.
                if (bWantsToFire && Weapon.Definition != nullptr && Weapon.Definition.bFullAuto)
                    ActivateGrantedAbility(FireInputTag);
            }
        }
    }

    // Idle free-look: drop the camera-yaw hard lock while truly idle so the camera orbits a
    // planted body; restore it (via a smooth catch-up turn) the moment the body must face the
    // camera. Runs on the predicting owner and the server (sim proxies replicate rotation);
    // both sides compute from the same inputs so corrections stay negligible.
    private void TickFacing(float DeltaSeconds)
    {
        if (!bActorLevelFreeLook)
        {
            // Lyra stack owns idle facing; keep the classic hard camera-yaw lock.
            if (!bUseControllerRotationYaw)
                bUseControllerRotationYaw = true;
            return;
        }

        if (Controller == nullptr)
            return;

        if (IsIncapacitated())
        {
            // Downed bodies never rotate with the camera.
            bUseControllerRotationYaw = false;
            return;
        }

        bool bNeedsCamera = bFireHeldForFacing || bFocusing
            || CharacterMovement.IsFalling()
            || Locomotion.IsSliding()
            || CharacterMovement.GetCurrentAcceleration().SizeSquared() > 1.0;

        float CtrlYaw = GetControlRotation().Yaw;
        float ActorYaw = GetActorRotation().Yaw;
        float Delta = NormalizeYaw(CtrlYaw - ActorYaw);

        if (bNeedsCamera)
        {
            if (bUseControllerRotationYaw)
                return;
            // Catch up to the camera, then hand back to the hard lock.
            float Step = IdleAlignYawRate * DeltaSeconds;
            if (Math::Abs(Delta) <= Step)
            {
                bUseControllerRotationYaw = true;
            }
            else
            {
                FRotator R = GetActorRotation();
                R.Yaw += (Delta > 0.0) ? Step : -Step;
                SetActorRotation(R);
            }
            return;
        }

        // True idle: free-look. If the camera swings past the deadband, drag the body just
        // enough to ride the limit (reads as a natural repositioning step).
        bUseControllerRotationYaw = false;
        if (Math::Abs(Delta) > IdleFreeLookYawLimit)
        {
            float Step = Math::Min(IdleAlignYawRate * DeltaSeconds, Math::Abs(Delta) - IdleFreeLookYawLimit);
            FRotator R = GetActorRotation();
            R.Yaw += (Delta > 0.0) ? Step : -Step;
            SetActorRotation(R);
        }
    }

    private float NormalizeYaw(float Angle) const
    {
        float A = Angle;
        while (A > 180.0)  A -= 360.0;
        while (A < -180.0) A += 360.0;
        return A;
    }

    // Slide anim driver: edge-detect the locomotion slide state every Tick (all machines — the
    // state derives from replicated movement, so owner predicts, server confirms, proxies mirror).
    private void TickSlideAnim()
    {
        bool bSlidingNow = Locomotion != nullptr && Locomotion.IsSliding();
        if (bSlidingNow == bSlideAnimActive)
        {
            // Mid-slide: hand over from the one-shot In to the Loop once the In has played out.
            if (bSlidingNow && SlideLoopAnim != nullptr && SlideLoopStartTime > 0.0
                && Gameplay::GetTimeSeconds() >= SlideLoopStartTime)
            {
                PlaySlideAnim(SlideLoopAnim, 9999);
                SlideLoopStartTime = 0.0;
            }
            return;
        }
        bSlideAnimActive = bSlidingNow;
        if (bSlidingNow)
        {
            if (SlideInAnim != nullptr)
            {
                PlaySlideAnim(SlideInAnim, 1);
                SlideLoopStartTime = Gameplay::GetTimeSeconds() + SlideInAnim.GetPlayLength() - 0.2;
            }
            else if (SlideLoopAnim != nullptr)
            {
                PlaySlideAnim(SlideLoopAnim, 9999);
            }
        }
        else
        {
            SlideLoopStartTime = 0.0;
            // Exit pick: crouch-held -> crouch exit; still fast -> run exit; else stand exit.
            UAnimSequenceBase Out = SlideOutStandAnim;
            if (Locomotion != nullptr && Locomotion.IsCrouchHeld() && SlideOutCrouchAnim != nullptr)
                Out = SlideOutCrouchAnim;
            else
            {
                FVector V = GetVelocity();
                if (FVector(V.X, V.Y, 0.0).Size() > 500.0 && SlideOutRunAnim != nullptr)
                    Out = SlideOutRunAnim;
            }
            if (Out != nullptr)
                PlaySlideAnim(Out, 1);
        }
    }

    private void PlaySlideAnim(UAnimSequenceBase Anim, int LoopCount)
    {
        UAnimInstance AnimInst = Mesh.GetAnimInstance();
        if (AnimInst == nullptr)
            return;
        AnimInst.PlaySlotAnimationAsDynamicMontage(Anim, SlideMontageSlot, 0.15, 0.25, 1.0, LoopCount, -1.0, 0.0);
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
        // Incapacitated heroes can't hold the trigger; forcing the flag through the same edge
        // detector means a press-while-down is a no-op and a held->incap stop releases the tail.
        bool bWant = bWants && !IsIncapacitated();
        SetHeldFire(bWant);
        SetFireHeldForFacing(bWant);
        if (bWant)
            ActivateGrantedAbility(FireInputTag);
    }

    // Manual reload (R). Auto-reload also fires when the magazine empties (WeaponComponent.NotifyFired).
    UFUNCTION(Server)
    void Server_RequestReload()
    {
        if (Weapon != nullptr)
            Weapon.StartReload();
    }

    // Cosmetic, fire-and-forget: muzzle flash / tracer(s) / impacts + fire sound on all machines,
    // recoil and a hitmarker on the owning client. MuzzleLocation is the gun muzzle (computed
    // server-side) so tracers come from the weapon, not the camera (D-0014). ImpactIsEnemy is
    // per-pellet (surface-aware impact FX); bHitEnemy flags that any pellet damaged an enemy.
    // All FX/audio slots on the definition are optional: null = debug-line / silent fallback.
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_FireFX(FVector MuzzleLocation, TArray<FVector> Impacts, TArray<bool> ImpactIsEnemy, bool bHitEnemy)
    {
        // A real shot happened this hold — arms the release tail (server-side flag; the multicast
        // body also runs on the listen server).
        if (HasAuthority())
            bFiredSinceHeld = true;

        PlayUpperBodyMontage(FireMontage);

        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        PlayWeaponMontage(Def != nullptr ? Def.WeaponFireMontage : nullptr);
        for (int i = 0; i < Impacts.Num(); i++)
        {
            FVector Impact = Impacts[i];

            // Tracer: spawn aimed muzzle->impact and hand the system its endpoint.
            if (Def != nullptr && Def.TracerFX != nullptr)
            {
                FRotator TracerRot = (Impact - MuzzleLocation).ToOrientationRotator();
                UNiagaraComponent Tracer = Niagara::SpawnSystemAtLocation(Def.TracerFX, MuzzleLocation, TracerRot);
                if (Tracer != nullptr)
                    Tracer.SetVectorParameter(n"TracerEnd", Impact);
            }
            else
            {
                System::DrawDebugLine(MuzzleLocation, Impact, FLinearColor(1.0, 1.0, 0.0), 0.05, 2.0);
            }

            // Surface-aware impact: enemy flesh vs world geometry.
            if (Def != nullptr)
            {
                bool bEnemyImpact = i < ImpactIsEnemy.Num() && ImpactIsEnemy[i];
                UNiagaraSystem ImpactFX = bEnemyImpact ? Def.ImpactEnemyFX : Def.ImpactWorldFX;
                if (ImpactFX != nullptr)
                    Niagara::SpawnSystemAtLocation(ImpactFX, Impact);
            }
        }

        if (Def != nullptr)
        {
            // Muzzle flash rides the gun (one per cartridge, not per pellet). A bad socket name
            // silently attaches to the mesh root — acceptable cosmetic degradation.
            if (Def.MuzzleFlashFX != nullptr)
                Niagara::SpawnSystemAttached(Def.MuzzleFlashFX, WeaponMesh, Def.MuzzleSocket,
                    FVector::ZeroVector, FRotator::ZeroRotator, EAttachLocation::SnapToTarget, true);
            if (Def.FireSound != nullptr)
                Gameplay::SpawnSoundAtLocation(Def.FireSound, MuzzleLocation);
        }

        if (IsLocallyControlled())
        {
            CameraFeel.NotifyFired();
            if (Weapon != nullptr && Weapon.Definition != nullptr)
            {
                AddControllerPitchInput(-Weapon.Definition.RecoilPitchPerShot);
                AddControllerYawInput(Math::RandRange(-Weapon.Definition.RecoilYawRange, Weapon.Definition.RecoilYawRange));
            }
            if (bHitEnemy)
            {
                LastHitConfirmTime = Gameplay::GetTimeSeconds();
                if (Def != nullptr && Def.HitTickSound != nullptr)
                    Gameplay::SpawnSound2D(Def.HitTickSound);
            }
        }
    }

    // Cosmetic: the gun tail rings out once when the trigger releases (full-auto fatigue fix).
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_FireStopped()
    {
        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        if (Def != nullptr && Def.FireTailSound != nullptr)
            Gameplay::SpawnSoundAtLocation(Def.FireTailSound, GetCosmeticMuzzleLocation());
    }

    // Muzzle position safe on ANY machine: socket via the cosmetic def when the mesh exists,
    // else the actor location (close enough for a tail sound). GetMuzzleLocation stays the
    // server-path origin (it reads the server-only Weapon.Definition).
    FVector GetCosmeticMuzzleLocation() const
    {
        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        if (Def != nullptr && WeaponMesh != nullptr && WeaponMesh.GetSkeletalMeshAsset() != nullptr
            && WeaponMesh.DoesSocketExist(Def.MuzzleSocket))
        {
            return WeaponMesh.GetSocketLocation(Def.MuzzleSocket);
        }
        return GetActorLocation();
    }

    // Cosmetic, fire-and-forget: reload montage + reload sound on all machines.
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_ReloadFX()
    {
        PlayUpperBodyMontage(ReloadMontage);
        URogueWeaponDefinition Def = GetCosmeticWeaponDef();
        PlayWeaponMontage(Def != nullptr ? Def.WeaponReloadMontage : nullptr);
        if (Def != nullptr && Def.ReloadSound != nullptr)
            Gameplay::SpawnSoundAtLocation(Def.ReloadSound, GetActorLocation());
    }

    private void PlayUpperBodyMontage(UAnimMontage Montage)
    {
        if (Montage == nullptr || Mesh == nullptr)
            return;
        UAnimInstance AnimInst = Mesh.GetAnimInstance();
        if (AnimInst != nullptr)
            AnimInst.Montage_Play(Montage);
    }

    private void PlayWeaponMontage(UAnimMontage Montage)
    {
        if (Montage == nullptr || WeaponMesh == nullptr || WeaponMesh.GetSkeletalMeshAsset() == nullptr)
            return;
        UAnimInstance WeapAnim = WeaponMesh.GetAnimInstance();
        if (WeapAnim != nullptr)
            WeapAnim.Montage_Play(Montage);
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
