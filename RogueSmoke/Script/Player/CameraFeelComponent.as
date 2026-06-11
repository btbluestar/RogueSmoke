// CameraFeelComponent.as
// Owning-client camera juice (research Report B): landing dip, sprint/slide FOV kick, per-shot
// cosmetic camera kick with spring recovery, and the focus (light-ADS) zoom blend moved here from
// the hero so camera logic has one home. Strictly cosmetic — never touches control rotation, so
// the server-authoritative aim point is unaffected (the Destiny visual-recoil/aim split).
class URogueCameraFeelComponent : UActorComponent
{
    // --- Focus zoom (moved from HeroCharacter) ---
    UPROPERTY(EditDefaultsOnly, Category = "Camera|Focus")
    float FocusBlendSpeed = 9.0;

    // --- Speed FOV kicks ---
    UPROPERTY(EditDefaultsOnly, Category = "Camera|FOV")
    float SprintFOVBonus = 5.0;

    UPROPERTY(EditDefaultsOnly, Category = "Camera|FOV")
    float SlideFOVBonus = 8.0;

    UPROPERTY(EditDefaultsOnly, Category = "Camera|FOV")
    float FOVBlendSpeed = 8.0;

    // --- Per-shot cosmetic kick (spring-recovered; camera-local, aim point untouched) ---
    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float FireKickPitch = 0.4;             // degrees up per shot

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float FireKickYawRange = 0.15;         // +/- random yaw per shot

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float FireKickMaxOffset = 2.5;         // stacked-offset clamp under full-auto

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float KickSpringStiffness = 400.0;     // ~0.1 s recovery

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Kick")
    float KickSpringDamping = 30.0;

    // --- Landing dip ---
    UPROPERTY(EditDefaultsOnly, Category = "Camera|Landing")
    float LandDipPerFallSpeed = 0.02;      // cm of dip per cm/s of fall speed

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Landing")
    float LandDipMax = 22.0;               // cm

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Landing")
    float DipSpringStiffness = 220.0;      // ~0.2 s recovery

    UPROPERTY(EditDefaultsOnly, Category = "Camera|Landing")
    float DipSpringDamping = 22.0;

    // --- Runtime ---
    private AHeroCharacter Hero;
    private USpringArmComponent Boom;
    private UCameraComponent Camera;
    // Resting FOV/arm seeded from the actual rig at Initialize — single source of truth is the hero's camera setup.
    private float BaseCameraFOV = 90.0;
    private float BaseArmLength = 350.0;
    private float FocusAlpha = 0.0;
    private float SpeedFOVBonus = 0.0;
    private float KickPitch = 0.0;
    private float KickPitchVel = 0.0;
    private float KickYaw = 0.0;
    private float KickYawVel = 0.0;
    private float DipZ = 0.0;
    private float DipZVel = 0.0;
    private FVector CameraBaseRelLocation;

    void Initialize(AHeroCharacter InHero, USpringArmComponent InBoom, UCameraComponent InCamera)
    {
        Hero = InHero;
        Boom = InBoom;
        Camera = InCamera;
        if (Boom != nullptr)
            BaseArmLength = Boom.TargetArmLength;
        if (Camera != nullptr)
        {
            BaseCameraFOV = Camera.FieldOfView;
            CameraBaseRelLocation = Camera.GetRelativeLocation();
        }
    }

    // Impulses land as direct position offsets (identical at any frame rate); the springs
    // recover them. Stacked full-auto kicks clamp at FireKickMaxOffset.
    void NotifyFired()
    {
        KickPitch = Math::Clamp(KickPitch + FireKickPitch, -FireKickMaxOffset, FireKickMaxOffset);
        KickYaw = Math::Clamp(KickYaw + Math::RandRange(-FireKickYawRange, FireKickYawRange),
            -FireKickMaxOffset, FireKickMaxOffset);
    }

    void NotifyLanded(float FallSpeed)
    {
        float Dip = Math::Min(FallSpeed * LandDipPerFallSpeed, LandDipMax);
        DipZ = Math::Max(DipZ - Dip, -LandDipMax);   // dip is downward (negative Z), bounded
    }

    // Called from the hero Tick, owning client only. Composites every channel onto the camera.
    void TickCameraFeel(float DeltaSeconds)
    {
        if (Hero == nullptr || Boom == nullptr || Camera == nullptr)
            return;

        // Springs (semi-implicit Euler; stable at game framerates).
        KickPitchVel += (-KickSpringStiffness * KickPitch - KickSpringDamping * KickPitchVel) * DeltaSeconds;
        KickPitch = Math::Clamp(KickPitch + KickPitchVel * DeltaSeconds, -FireKickMaxOffset, FireKickMaxOffset);
        KickYawVel += (-KickSpringStiffness * KickYaw - KickSpringDamping * KickYawVel) * DeltaSeconds;
        KickYaw = Math::Clamp(KickYaw + KickYawVel * DeltaSeconds, -FireKickMaxOffset, FireKickMaxOffset);
        DipZVel += (-DipSpringStiffness * DipZ - DipSpringDamping * DipZVel) * DeltaSeconds;
        DipZ += DipZVel * DeltaSeconds;

        // Focus zoom (same behavior the hero had; weapon overrides via the cosmetic def).
        float Target = Hero.bFocusing ? 1.0 : 0.0;
        FocusAlpha = Math::FInterpTo(FocusAlpha, Target, DeltaSeconds, FocusBlendSpeed);
        float TargetFOV = 70.0;
        float TargetArm = 220.0;
        URogueWeaponDefinition Def = Hero.GetCosmeticWeaponDef();
        if (Def != nullptr)
        {
            TargetFOV = Def.FocusFOV;
            TargetArm = Def.FocusArmLength;
        }

        // Speed FOV: sprint/slide widen, fast blend; focus zoom wins by composing after it.
        float WantSpeedBonus = 0.0;
        if (Hero.Locomotion.IsSliding())
            WantSpeedBonus = SlideFOVBonus;
        else if (Hero.Locomotion.IsSprinting())
            WantSpeedBonus = SprintFOVBonus;
        SpeedFOVBonus = Math::FInterpTo(SpeedFOVBonus, WantSpeedBonus, DeltaSeconds, FOVBlendSpeed);

        Camera.FieldOfView = Math::Lerp(BaseCameraFOV + SpeedFOVBonus, TargetFOV, FocusAlpha);
        Boom.TargetArmLength = Math::Lerp(BaseArmLength, TargetArm, FocusAlpha);
        Camera.SetRelativeRotation(FRotator(KickPitch, KickYaw, 0.0));
        Camera.SetRelativeLocation(CameraBaseRelLocation + FVector(0.0, 0.0, DipZ));
    }
}
