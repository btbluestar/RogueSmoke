// HeroAnimInstance.as
// The hero's anim-graph data source (Lyra's LyraAnimInstance role). Computes every variable the
// layered ABP reads — see docs/guides/ABP_HERO_BUILD_GUIDE.md for the graph that consumes these.
// Works on ALL machines (owner, server, simulated proxies): everything derives from replicated
// state (velocity, BaseAimRotation, movement mode, locomotion component mirrors).
class URogueHeroAnimInstance : UAnimInstance
{
    // --- Locomotion (lower body) ---
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float GroundSpeed = 0.0;

    // Signed angle (-180..180) between velocity and actor facing; drives the strafe blendspace X.
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float Direction = 0.0;

    // Blendspace Y input in AUTHORED units: min(GroundSpeed, JogAuthoredSpeed).
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float StrafeSpeed = 0.0;

    // Anti-foot-slide: GroundSpeed / JogAuthoredSpeed above authored speed, clamped (Research C.3).
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float PlayRate = 1.0;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bShouldMove = false;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsFalling = false;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float VerticalVelocity = 0.0;

    // Mirrors CMC IsCrouching() — TRUE during slides too (slide crouches the capsule); a crouch-idle ABP state must gate on bIsCrouching && !bIsSliding.
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsCrouching = false;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsSliding = false;

    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool bIsSprinting = false;

    // --- Aim (upper body) ---
    UPROPERTY(BlueprintReadOnly, Category = "Aim")
    float AimPitch = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "Aim")
    float AimYaw = 0.0;

    UPROPERTY(BlueprintReadOnly, Category = "Aim")
    float AimOffsetAlpha = 1.0;

    // --- Landing recovery (additive alpha, decays after touchdown scaled by fall speed) ---
    UPROPERTY(BlueprintReadOnly, Category = "Landing")
    float LandRecoveryAlpha = 0.0;

    // Authored root speed of the jog row; measure once in Task 8 and correct here if needed.
    UPROPERTY(EditDefaultsOnly, Category = "Tuning")
    float JogAuthoredSpeed = 450.0;

    UPROPERTY(EditDefaultsOnly, Category = "Tuning")
    float LandRecoverySeconds = 0.4;

    private bool bWasFalling = false;
    private float PrevFallSpeed = 0.0;

    UFUNCTION(BlueprintOverride)
    void BlueprintUpdateAnimation(float DeltaTimeX)
    {
        AHeroCharacter Hero = Cast<AHeroCharacter>(TryGetPawnOwner());
        if (Hero == nullptr || Hero.CharacterMovement == nullptr)
            return;
        UCharacterMovementComponent Move = Hero.CharacterMovement;

        FVector Vel = Hero.GetVelocity();
        FVector Flat = FVector(Vel.X, Vel.Y, 0.0);
        GroundSpeed = Flat.Size();
        VerticalVelocity = Vel.Z;
        bIsFalling = Move.IsFalling();
        bIsCrouching = Move.IsCrouching();
        URogueLocomotionComponent Loco = Hero.Locomotion;
        bIsSliding = Loco != nullptr && Loco.IsSliding();
        bIsSprinting = Loco != nullptr && Loco.IsSprinting();

        // Acceleration check prevents the idle-pop while decelerating to a stop (Research C.3).
        bShouldMove = GroundSpeed > 3.0 && Move.GetCurrentAcceleration().SizeSquared() > 0.0;

        // Strafe direction: velocity in actor space (manual — no CalculateDirection binding bet).
        FRotator ActorRot = Hero.GetActorRotation();
        FVector Local = ActorRot.UnrotateVector(Flat);
        Direction = (GroundSpeed > 3.0) ? Math::RadiansToDegrees(Math::Atan2(Local.Y, Local.X)) : 0.0;

        StrafeSpeed = Math::Min(GroundSpeed, JogAuthoredSpeed);
        PlayRate = (GroundSpeed > JogAuthoredSpeed)
            ? Math::Clamp(GroundSpeed / JogAuthoredSpeed, 0.8, 1.35) : 1.0;

        // Aim deltas from BaseAimRotation: replicated (compressed) to simulated proxies, unlike
        // ControlRotation — this is what makes remote players' torsos track their crosshair.
        FRotator AimRot = Hero.GetBaseAimRotation();
        AimPitch = Math::Clamp(NormalizeAngle(AimRot.Pitch - ActorRot.Pitch), -90.0, 90.0);
        AimYaw = Math::Clamp(NormalizeAngle(AimRot.Yaw - ActorRot.Yaw), -90.0, 90.0);
        AimOffsetAlpha = Hero.IsIncapacitated() ? 0.0 : 1.0;

        // Self-contained landing detection (works on sim proxies, no event needed): falling last
        // frame + grounded now = landed; alpha scales with how hard we hit relative to the
        // locomotion component's live jump impulse (public UPROPERTY, AS-to-AS read).
        if (bWasFalling && !bIsFalling)
        {
            float JumpRef = (Loco != nullptr) ? Loco.JumpZVelocity : 750.0;
            LandRecoveryAlpha = Math::Clamp(PrevFallSpeed / Math::Max(JumpRef, 1.0), 0.15, 1.0);
        }
        else if (LandRecoveryAlpha > 0.0)
            LandRecoveryAlpha = Math::Max(0.0, LandRecoveryAlpha - DeltaTimeX / Math::Max(LandRecoverySeconds, 0.01));
        bWasFalling = bIsFalling;
        if (bIsFalling)
            PrevFallSpeed = Math::Abs(Vel.Z);
    }

    private float NormalizeAngle(float Angle) const
    {
        float A = Angle;
        while (A > 180.0)  A -= 360.0;
        while (A < -180.0) A += 360.0;
        return A;
    }
}
