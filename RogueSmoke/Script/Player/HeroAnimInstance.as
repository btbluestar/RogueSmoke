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
    // AimPitch/AimYaw deliberately do NOT live here. The Lyra graph (ABP_Mannequin_Base) owns
    // them as its own BP variables, written per-frame by its thread-safe update functions; the
    // item anim layers read them by name from the main instance. Declaring same-named UPROPERTYs
    // on this parent collides at BP compile, and deleting the BP vars to resolve it silently
    // deletes their writer nodes — which froze the torso aim and pointed the gun 90° off
    // (checkpoint-A bugs #1/#5). The retired v1 ABP_Hero bound these two; it loses them if
    // recompiled, which is acceptable pending its deletion.
    UPROPERTY(BlueprintReadOnly, Category = "Aim")
    float AimOffsetAlpha = 1.0;

    // --- Landing recovery (additive alpha, decays after touchdown scaled by fall speed) ---
    UPROPERTY(BlueprintReadOnly, Category = "Landing")
    float LandRecoveryAlpha = 0.0;

    // --- Lyra ABP_Mannequin_Base surface (D-0022 migration). The copied graph property-binds
    // these by name; GameplayTag_* replace Lyra's GameplayTagPropertyMap (we fill them from
    // replicated state so simulated proxies work too). ---
    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    float GroundDistance = -1.0;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsFiring = false;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsReloading = false;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsADS = false;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsDashing = false;

    UPROPERTY(BlueprintReadOnly, Category = "Lyra")
    bool GameplayTag_IsMelee = false;

    // Authored root SPEED of the jog row: root displacement / clip length (955 cm over 1.533 s).
    // The walk row is 304 cm/s. Don't confuse displacement with speed — that bug shipped once.
    UPROPERTY(EditDefaultsOnly, Category = "Tuning")
    float JogAuthoredSpeed = 623.0;

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

        // Below the jog row the blendspace's authored-speed sample placement already matches foot
        // speed to ground speed exactly (304-walk..623-jog interpolation), so PlayRate stays 1.
        // Above it (sprint 960) the jog clip rate-scales: 960/623 = 1.54.
        StrafeSpeed = Math::Min(GroundSpeed, JogAuthoredSpeed);
        PlayRate = (GroundSpeed > JogAuthoredSpeed)
            ? Math::Clamp(GroundSpeed / JogAuthoredSpeed, 0.8, 1.6) : 1.0;

        // Aim pitch/yaw are computed by the Lyra graph's own UpdateAimingData (BP vars on
        // ABP_Mannequin_Base) — see the Aim section comment above. Only the v1 alpha remains.
        AimOffsetAlpha = Hero.IsIncapacitated() ? 0.0 : 1.0;

        // Lyra surface: ground distance feeds distance-matched landing (trace only while airborne).
        if (bIsFalling)
        {
            FVector TraceStart = Hero.GetActorLocation();
            FVector TraceEnd = TraceStart - FVector(0.0, 0.0, 2000.0);
            FHitResult GroundHit;
            TArray<AActor> IgnoredActors;
            IgnoredActors.Add(Hero);
            if (System::LineTraceSingle(TraceStart, TraceEnd, ETraceTypeQuery::Visibility, false,
                                        IgnoredActors, EDrawDebugTrace::None, GroundHit, true))
                GroundDistance = GroundHit.Distance - Hero.CapsuleComponent.CapsuleHalfHeight;
            else
                GroundDistance = 2000.0;
        }
        else
            GroundDistance = 0.0;

        GameplayTag_IsFiring = Hero.IsFireHeldForFacing();
        GameplayTag_IsADS = Hero.bFocusing;
        GameplayTag_IsDashing = bIsSliding;
        // IsReloading / IsMelee stay false until those states exist.

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

}
