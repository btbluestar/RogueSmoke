// LocomotionComponent.as
// The hero's mobility state machine (D-0015, reworked for the feel pass — see
// docs/superpowers/research/2026-06-11-feel-research-notes.md Report A):
// Deadlock-lean ground response, heavy gravity, and Apex-style slide-hop momentum
// (boost-arming threshold + hard cap + landing re-entry), all on stock CMC so movement
// stays client-predicted. Every knob is a UPROPERTY: MoveTune sets them live in PIE and
// meta-progression (D-0013) modifies them later.
//
// Composition home (CODING_STANDARDS §3), mirroring URogueWeaponComponent: tunables + runtime
// state live here; the hero exposes thin input methods that drive it. Slide physics run from the
// hero Tick via delta seconds (no timers/world-clock) on both the locally-controlled client and
// the server, so stock CMC reconciliation keeps them aligned.
class URogueLocomotionComponent : UActorComponent
{
    // --- Ground response (input feels direct) ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float MaxAcceleration = 6144.0;          // sv_accelerate 10 ~= full speed in ~0.15 s

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float BrakingDecelerationWalking = 3072.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float GroundFriction = 10.0;

    // Separate braking friction decouples stop-feel from turn-feel (PBCharacterMovement pattern).
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float BrakingFriction = 6.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Ground")
    float BrakingFrictionFactor = 1.0;

    // --- Gravity & air (the floatiness fix) ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float GravityScale = 1.8;                // Deadlock ~2.07, Apex ~1.46; lean Deadlock

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float JumpZVelocity = 750.0;             // similar height as before, faster arc

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float DoubleJumpZVelocity = 700.0;       // second jump reads better slightly weaker

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float JumpMaxHoldTime = 0.0;             // both refs use fixed impulse; tunable for PIE A/B

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float AirControl = 0.9;                  // Deadlock-style direct air steering

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Air")
    float FallingLateralFriction = 0.0;      // preserve slide-hop momentum in air

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Jump")
    int MaxJumpCount = 2;

    // --- Sprint / crouch / focus ---
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Sprint")
    float SprintSpeedMultiplier = 1.6;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Crouch")
    float CrouchSpeed = 300.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Focus")
    float FocusMoveMultiplier = 0.8;

    // --- Slide (the fastest ground state; momentum tool) ---
    // Boost target / cap / arming threshold are fractions of the CURRENT sprint speed so
    // MoveSpeed upgrades scale the whole slide economy with them.
    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBoostMultiplier = 1.3;        // boost target = sprint * this (Apex ~1.34 cap)

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideSpeedCapMultiplier = 1.5;     // hard cap = sprint * this (Deadlock dash-jump 1.55x)

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBoostArmMultiplier = 1.1;     // boost ONLY if speed < sprint * this (anti-bhop, Apex rule)

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideEntryMinFraction = 0.85;      // entry needs speed >= sprint * this

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideExitSpeedFraction = 0.9;      // slide ends when speed < BaseWalkSpeed * this

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideGroundFriction = 0.6;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBraking = 256.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideDownhillAccel = 2048.0;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideMaxDuration = 1.5;

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideBoostCooldown = 1.8;          // belt-and-suspenders on top of the arming rule

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    float SlideSustainMinSlope = 0.1;        // ~6 deg: descending refreshes the duration

    UPROPERTY(EditDefaultsOnly, Category = "Movement|Slide")
    bool bRequireSprintToSlide = true;

    // --- Runtime state (CMC replicates the actual motion; this mirrors on client+server) ---
    private ACharacter OwnerCharacter;
    private UCharacterMovementComponent Move;
    private float BaseWalkSpeed = 600.0;
    private bool bSprinting = false;
    private bool bFocusing = false;
    private bool bCrouchHeld = false;
    private bool bSliding = false;
    private bool bPostSlideHopAir = false;   // cap applies in air after a slide-hop
    private float SlideTimeRemaining = 0.0;
    private float BoostCooldownRemaining = 0.0;
    private float LastLandingFallSpeed = 0.0;  // |Z| at last landing; anim/camera read it

    void Initialize(ACharacter InOwner)
    {
        OwnerCharacter = InOwner;
        if (OwnerCharacter == nullptr)
            return;
        Move = OwnerCharacter.CharacterMovement;
        if (Move == nullptr)
            return;

        // Enable crouching (struct property is a value copy in AS: read-modify-write).
        FNavAgentProperties NavProps = Move.NavAgentProps;
        NavProps.bCanCrouch = true;
        Move.NavAgentProps = NavProps;

        ApplyMovementConfig();
    }

    // Push every tunable into the CMC/character. Initialize, MoveTune, and upgrades call this.
    void ApplyMovementConfig()
    {
        if (Move == nullptr || OwnerCharacter == nullptr)
            return;
        Move.MaxAcceleration = MaxAcceleration;
        Move.BrakingDecelerationWalking = BrakingDecelerationWalking;
        Move.BrakingFriction = BrakingFriction;
        Move.BrakingFrictionFactor = BrakingFrictionFactor;
        Move.bUseSeparateBrakingFriction = true;
        Move.GravityScale = GravityScale;
        Move.JumpZVelocity = JumpZVelocity;
        Move.AirControl = AirControl;
        Move.FallingLateralFriction = FallingLateralFriction;
        OwnerCharacter.JumpMaxCount = MaxJumpCount;
        OwnerCharacter.JumpMaxHoldTime = JumpMaxHoldTime;
        // GroundFriction is stateful (the slide swaps it); only set when not mid-slide.
        if (!bSliding)
            Move.GroundFriction = GroundFriction;
        ApplyGroundSpeed();
    }

    void SetBaseSpeed(float NewBaseSpeed)
    {
        if (NewBaseSpeed > 0.0)
            BaseWalkSpeed = NewBaseSpeed;
        ApplyGroundSpeed();
    }

    void SetSprint(bool bWantsSprint)   { bSprinting = bWantsSprint; ApplyGroundSpeed(); }
    void SetFocus(bool bWantsFocus)     { bFocusing = bWantsFocus;   ApplyGroundSpeed(); }

    bool IsSprinting() const { return bSprinting; }
    bool IsSliding() const { return bSliding; }
    bool IsCrouchHeld() const { return bCrouchHeld; }
    float GetLastLandingFallSpeed() const { return LastLandingFallSpeed; }
    float SprintSpeed() const { return BaseWalkSpeed * SprintSpeedMultiplier; }
    float SlideCap() const { return SprintSpeed() * SlideSpeedCapMultiplier; }

    void RequestCrouchOrSlide()
    {
        bCrouchHeld = true;
        if (OwnerCharacter == nullptr || Move == nullptr)
            return;

        bool bGrounded = Move.IsMovingOnGround();
        bool bFastEnough = HorizontalSpeed() >= SprintSpeed() * SlideEntryMinFraction;
        bool bSprintOk = !bRequireSprintToSlide || bSprinting;

        if (bGrounded && bFastEnough && bSprintOk && !bSliding)
            StartSlide();
        else
            OwnerCharacter.Crouch();
    }

    void ReleaseCrouch()
    {
        bCrouchHeld = false;
        if (bSliding)
            EndSlide();
        else if (OwnerCharacter != nullptr)
            OwnerCharacter.UnCrouch();
    }

    // Jump pressed while sliding (owning client; the server converges via the airborne check in
    // TickLocomotion). Ends the slide WITHOUT bleeding speed: friction restore only matters on
    // ground, FallingLateralFriction 0 keeps the velocity in the air. Keeps crouch if held so
    // landing can chain straight into the next slide (the Deadlock loop).
    void NotifySlideJump()
    {
        if (!bSliding)
            return;
        bSliding = false;
        bPostSlideHopAir = true;
        Move.GroundFriction = GroundFriction;
        Move.BrakingDecelerationWalking = BrakingDecelerationWalking;
        // The hero UnCrouches before Jump() so stock CanJump allows it.
        // Chain re-entry on landing keys off bCrouchHeld, not the crouch posture.
    }

    // Second-jump shaping: stock CMC reuses JumpZVelocity for every jump; rescale the second.
    // Runs on predicted client + server (OnJumped fires on both) and is idempotent (sets, not adds).
    void NotifyJumped(int JumpCount)
    {
        if (Move == nullptr || JumpCount < 2)
            return;
        FVector Vel = Move.Velocity;
        Move.Velocity = FVector(Vel.X, Vel.Y, DoubleJumpZVelocity);
    }

    // Landed (server + owning client). Re-enter the slide instantly if the chain is alive.
    void NotifyLanded(float FallSpeed)
    {
        LastLandingFallSpeed = FallSpeed;
        bPostSlideHopAir = false;
        if (OwnerCharacter == nullptr || Move == nullptr)
            return;
        bool bFastEnough = HorizontalSpeed() >= SprintSpeed() * SlideEntryMinFraction;
        bool bSprintOk = !bRequireSprintToSlide || bSprinting;
        if (bCrouchHeld && bFastEnough && bSprintOk && !bSliding)
            StartSlide();
    }

    // Clears transient slide/air state when motion is force-stopped outside the normal landing
    // path (down/revive, teleports) so the post-hop speed cap can't leak into the next life.
    void ResetAirState()
    {
        bSliding = false;
        bPostSlideHopAir = false;
        SlideTimeRemaining = 0.0;
    }

    private void StartSlide()
    {
        bSliding = true;
        SlideTimeRemaining = SlideMaxDuration;
        OwnerCharacter.Crouch();

        Move.GroundFriction = SlideGroundFriction;
        Move.BrakingDecelerationWalking = SlideBraking;

        FVector Vel = Move.Velocity;
        FVector Flat = FVector(Vel.X, Vel.Y, 0.0);
        FVector Dir = Flat.GetSafeNormal();
        if (Dir.IsNearlyZero())
            Dir = OwnerCharacter.GetActorForwardVector();

        // Apex's anti-bhop governor: the boost only arms below the threshold; above it you keep
        // your speed but gain nothing. Idempotent on client+server (sets toward a target).
        float Speed = Flat.Size();
        float Target = Speed;
        if (Speed < SprintSpeed() * SlideBoostArmMultiplier && BoostCooldownRemaining <= 0.0)
        {
            Target = Math::Max(Speed, SprintSpeed() * SlideBoostMultiplier);
            BoostCooldownRemaining = SlideBoostCooldown;
        }
        Target = Math::Min(Target, SlideCap());
        FVector NewFlat = Dir * Target;
        Move.Velocity = FVector(NewFlat.X, NewFlat.Y, Vel.Z);
    }

    private void EndSlide()
    {
        bSliding = false;
        Move.GroundFriction = GroundFriction;
        Move.BrakingDecelerationWalking = BrakingDecelerationWalking;
        if (!bCrouchHeld && OwnerCharacter != nullptr)
            OwnerCharacter.UnCrouch();
    }

    private void ApplyGroundSpeed()
    {
        if (Move == nullptr)
            return;
        float Speed = BaseWalkSpeed * (bSprinting ? SprintSpeedMultiplier : 1.0);
        if (bFocusing)
            Speed *= FocusMoveMultiplier;
        Move.MaxWalkSpeed = Speed;
        Move.MaxWalkSpeedCrouched = CrouchSpeed;
    }

    private float HorizontalSpeed() const
    {
        if (Move == nullptr)
            return 0.0;
        FVector Vel = Move.Velocity;
        return FVector(Vel.X, Vel.Y, 0.0).Size();
    }

    void TickLocomotion(float DeltaSeconds)
    {
        if (Move == nullptr || OwnerCharacter == nullptr)
            return;

        if (BoostCooldownRemaining > 0.0)
            BoostCooldownRemaining -= DeltaSeconds;

        // Post-slide-hop air: hold the hard cap so air control can't compound past it.
        if (bPostSlideHopAir && Move.IsFalling())
        {
            FVector Vel = Move.Velocity;
            FVector Flat = FVector(Vel.X, Vel.Y, 0.0);
            if (Flat.Size() > SlideCap())
            {
                FVector Capped = Flat.GetSafeNormal() * SlideCap();
                Move.Velocity = FVector(Capped.X, Capped.Y, Vel.Z);
            }
        }

        if (!bSliding)
            return;

        // Left the ground mid-slide (jumped or rolled off a ledge). NotifySlideJump is the
        // explicit jump intent (owning client); this branch is the convergence/ledge path —
        // same state transition, kept separate so the intent call site stays meaningful.
        if (!Move.IsMovingOnGround())
        {
            bSliding = false;
            bPostSlideHopAir = true;
            Move.GroundFriction = GroundFriction;
            Move.BrakingDecelerationWalking = BrakingDecelerationWalking;
            return;
        }

        // Downhill: accelerate along the slope and sustain the slide (Apex-style).
        FVector FloorNormal = Move.CurrentFloor.HitResult.Normal;
        FVector Downhill = FVector(FloorNormal.X, FloorNormal.Y, 0.0);
        bool bDescending = false;
        if (!Downhill.IsNearlyZero())
        {
            FVector DownDir = Downhill.GetSafeNormal();
            Move.Velocity += DownDir * SlideDownhillAccel * DeltaSeconds;

            FVector VelFlat = FVector(Move.Velocity.X, Move.Velocity.Y, 0.0);
            bDescending = Downhill.Size() >= SlideSustainMinSlope
                && VelFlat.GetSafeNormal().DotProduct(DownDir) > 0.0;
        }

        // Cap while sliding too (downhill accel must not run away).
        FVector SVel = Move.Velocity;
        FVector SFlat = FVector(SVel.X, SVel.Y, 0.0);
        if (SFlat.Size() > SlideCap())
        {
            FVector Capped = SFlat.GetSafeNormal() * SlideCap();
            Move.Velocity = FVector(Capped.X, Capped.Y, SVel.Z);
        }

        if (bDescending)
            SlideTimeRemaining = SlideMaxDuration;
        else
            SlideTimeRemaining -= DeltaSeconds;

        if (SlideTimeRemaining <= 0.0 || HorizontalSpeed() < BaseWalkSpeed * SlideExitSpeedFraction)
            EndSlide();
    }
}
