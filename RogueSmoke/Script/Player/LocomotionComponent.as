// LocomotionComponent.as
// The hero's mobility state machine (D-0015): hold-to-sprint, crouch, momentum slide, and the
// double-jump config. Apex/Deadlock feel, built on stock UCharacterMovementComponent so movement
// stays client-predicted without a custom C++ movement mode (the documented upgrade path).
//
// Composition home (CODING_STANDARDS §3), mirroring URogueWeaponComponent: tunables + runtime state
// here, the hero exposes thin input methods that drive it. The slide physics run from the hero Tick
// via delta seconds (no timers/world-clock), on both the locally-controlled client (prediction) and
// the server (authority) so stock CMC reconciliation keeps them aligned.
//
// MVP: state is not separately replicated — CMC already replicates velocity/crouch/jump. The tunable
// fields below are the seam meta-progression (D-0013) will later modify (speed, jump height/count,
// slide distance) and re-apply via ApplyGroundSpeed().
class URogueLocomotionComponent : UActorComponent
{
    // --- Sprint ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Sprint")
    float SprintSpeedMultiplier = 1.6;

    // --- Jump ---
    // Double-jump enabled by default so it's playable now. When meta-progression lands this becomes an
    // unlock: default this to 1 and have an upgrade raise it.
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Jump")
    int MaxJumpCount = 2;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Jump")
    float JumpZVelocity = 600.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Jump")
    float AirControl = 0.35;

    // --- Crouch ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Crouch")
    float CrouchSpeed = 300.0;

    // --- Slide ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBoostSpeed = 900.0;          // target horizontal speed entering a slide

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideMinSpeed = 350.0;            // drop below this and the slide ends

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideGroundFriction = 0.4;        // low friction = the slide carries

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBraking = 200.0;             // BrakingDecelerationWalking during the slide

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideDownhillAccel = 2048.0;      // extra accel along a downhill slope

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideMaxDuration = 1.5;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideCooldown = 0.4;              // re-slide gate

    // Downhill steepness (|horizontal floor normal|, ~sin of the slope angle) at or above which a
    // slide is "descending" and sustains itself instead of timing out. 0.1 ~= a 6 degree ramp.
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideSustainMinSlope = 0.1;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    bool bRequireSprintToSlide = true;

    // --- Runtime state (host-authoritative MVP; CMC replicates the actual motion) ---
    private ACharacter OwnerCharacter;
    private UCharacterMovementComponent Move;
    private float BaseWalkSpeed = 600.0;    // from the MoveSpeed attribute
    private bool bSprinting = false;
    private bool bCrouchHeld = false;
    private bool bSliding = false;
    private float SlideTimeRemaining = 0.0;
    private float SlideCooldownRemaining = 0.0;

    // Stashed walking values restored when a slide ends.
    private float DefaultGroundFriction = 8.0;
    private float DefaultBrakingDeceleration = 2048.0;

    // Called by the hero once its ASC is ready. Caches owner + CMC and pushes jump/crouch config.
    void Initialize(ACharacter InOwner)
    {
        OwnerCharacter = InOwner;
        if (OwnerCharacter == nullptr)
            return;

        Move = OwnerCharacter.CharacterMovement;
        if (Move == nullptr)
            return;

        DefaultGroundFriction = Move.GroundFriction;
        DefaultBrakingDeceleration = Move.BrakingDecelerationWalking;

        OwnerCharacter.JumpMaxCount = MaxJumpCount;
        Move.JumpZVelocity = JumpZVelocity;
        Move.AirControl = AirControl;

        // Enable crouching (NavAgentProps.bCanCrouch defaults false). Read-modify-write because the
        // struct property is a value copy in AngelScript.
        FNavAgentProperties NavProps = Move.NavAgentProps;
        NavProps.bCanCrouch = true;
        Move.NavAgentProps = NavProps;

        ApplyGroundSpeed();
    }

    // Base (un-sprinted) walk speed, sourced from the MoveSpeed attribute by the hero.
    void SetBaseSpeed(float NewBaseSpeed)
    {
        if (NewBaseSpeed > 0.0)
            BaseWalkSpeed = NewBaseSpeed;
        ApplyGroundSpeed();
    }

    void SetSprint(bool bWantsSprint)
    {
        bSprinting = bWantsSprint;
        ApplyGroundSpeed();
    }

    bool IsSprinting() const { return bSprinting; }
    bool IsSliding() const { return bSliding; }

    // Crouch key pressed: slide if we have the speed (and sprint, when required), else crouch-walk.
    void RequestCrouchOrSlide()
    {
        bCrouchHeld = true;
        if (OwnerCharacter == nullptr || Move == nullptr)
            return;

        bool bGrounded = Move.IsMovingOnGround();
        bool bFastEnough = HorizontalSpeed() >= SlideMinSpeed;
        bool bSprintOk = !bRequireSprintToSlide || bSprinting;

        if (bGrounded && bFastEnough && bSprintOk && SlideCooldownRemaining <= 0.0 && !bSliding)
            StartSlide();
        else
            OwnerCharacter.Crouch();
    }

    // Crouch key released: end the slide if sliding, otherwise stand up.
    void ReleaseCrouch()
    {
        bCrouchHeld = false;
        if (bSliding)
            EndSlide();
        else if (OwnerCharacter != nullptr)
            OwnerCharacter.UnCrouch();
    }

    private void StartSlide()
    {
        bSliding = true;
        SlideTimeRemaining = SlideMaxDuration;
        OwnerCharacter.Crouch();

        Move.GroundFriction = SlideGroundFriction;
        Move.BrakingDecelerationWalking = SlideBraking;

        // Idempotent impulse: set horizontal speed to max(current, boost) along the move direction, so
        // running this on client and server can't compound. Keep the existing vertical velocity.
        FVector Vel = Move.Velocity;
        FVector Flat = FVector(Vel.X, Vel.Y, 0.0);
        FVector Dir = Flat.GetSafeNormal();
        if (Dir.IsNearlyZero())
            Dir = OwnerCharacter.GetActorForwardVector();
        float Target = Math::Max(Flat.Size(), SlideBoostSpeed);
        FVector NewFlat = Dir * Target;
        Move.Velocity = FVector(NewFlat.X, NewFlat.Y, Vel.Z);
    }

    private void EndSlide()
    {
        bSliding = false;
        SlideCooldownRemaining = SlideCooldown;
        Move.GroundFriction = DefaultGroundFriction;
        Move.BrakingDecelerationWalking = DefaultBrakingDeceleration;

        // Stand back up unless the crouch key is still held (then stay crouch-walking).
        if (!bCrouchHeld && OwnerCharacter != nullptr)
            OwnerCharacter.UnCrouch();
    }

    private void ApplyGroundSpeed()
    {
        if (Move == nullptr)
            return;
        Move.MaxWalkSpeed = BaseWalkSpeed * (bSprinting ? SprintSpeedMultiplier : 1.0);
        Move.MaxWalkSpeedCrouched = CrouchSpeed;
    }

    private float HorizontalSpeed() const
    {
        if (Move == nullptr)
            return 0.0;
        FVector Vel = Move.Velocity;
        return FVector(Vel.X, Vel.Y, 0.0).Size();
    }

    // Driven by the hero's Tick (wherever movement is simulated). Advances the re-slide cooldown and,
    // while sliding, adds downhill acceleration and checks the slide's end conditions.
    void TickLocomotion(float DeltaSeconds)
    {
        if (Move == nullptr || OwnerCharacter == nullptr)
            return;

        if (SlideCooldownRemaining > 0.0)
            SlideCooldownRemaining -= DeltaSeconds;

        if (!bSliding)
            return;

        // Downhill speed-up: push velocity along the slope's downhill direction (zero on flat ground).
        // The floor normal tilts toward downhill, so its horizontal projection *is* the downhill
        // vector, and its length grows with slope steepness.
        FVector FloorNormal = Move.CurrentFloor.HitResult.Normal;
        FVector Downhill = FVector(FloorNormal.X, FloorNormal.Y, 0.0);
        bool bDescending = false;
        if (!Downhill.IsNearlyZero())
        {
            FVector DownDir = Downhill.GetSafeNormal();
            Move.Velocity += DownDir * SlideDownhillAccel * DeltaSeconds;

            // "Descending" = slope steep enough AND we're actually moving down it.
            FVector VelFlat = FVector(Move.Velocity.X, Move.Velocity.Y, 0.0);
            bDescending = Downhill.Size() >= SlideSustainMinSlope
                && VelFlat.GetSafeNormal().DotProduct(DownDir) > 0.0;
        }

        // The duration cap only burns down on flat/uphill ground; a sustained descent refreshes it so
        // a long ramp keeps you sliding (Apex-style) instead of the timer popping you upright mid-slope.
        // The slide still ends when the slope flattens and speed bleeds below SlideMinSpeed, on release,
        // or on leaving the ground.
        if (bDescending)
            SlideTimeRemaining = SlideMaxDuration;
        else
            SlideTimeRemaining -= DeltaSeconds;

        bool bStop = SlideTimeRemaining <= 0.0
            || HorizontalSpeed() < SlideMinSpeed
            || !Move.IsMovingOnGround();
        if (bStop)
            EndSlide();
    }
}
