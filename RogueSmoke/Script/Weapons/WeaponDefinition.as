// WeaponDefinition.as
// Data-driven definition of a ranged weapon variant (assault rifle, minigun, shotgun, ...).
// Pure data: each variant is a DA_ asset. Consumed by URogueWeaponComponent (runtime state) and
// UGA_WeaponFire (the fire ability). Modular by design — new weapons are new DataAssets, no new code.
// Spread uses a simplified Lyra heat model (cf. ULyraRangedWeaponInstance): spread grows with a
// normalized "heat" value that rises per shot and recovers over time.
class URogueWeaponDefinition : UPrimaryDataAsset
{
    UPROPERTY(EditDefaultsOnly, Category = "Identity")
    FText DisplayName;

    // --- Firing ---
    UPROPERTY(EditDefaultsOnly, Category = "Firing")
    float Damage = 12.0;

    UPROPERTY(EditDefaultsOnly, Category = "Firing")
    float MaxRange = 12000.0;            // cm

    // Pellets per shot (1 = rifle/SMG, >1 = shotgun).
    UPROPERTY(EditDefaultsOnly, Category = "Firing")
    int BulletsPerCartridge = 1;

    // Seconds between shots. Also the full-auto refire interval.
    UPROPERTY(EditDefaultsOnly, Category = "Firing")
    float FireInterval = 0.15;

    // Hold-to-fire (AR/minigun) vs one shot per press (semi/pistol).
    UPROPERTY(EditDefaultsOnly, Category = "Firing")
    bool bFullAuto = false;

    // --- Spread / bloom (heat model) ---
    // Spread degrees = Lerp(Min, Max, Heat). Heat (0..1) rises HeatPerShot each shot and, after
    // SpreadRecoveryDelay seconds without firing, recovers at HeatCooldownPerSecond.
    UPROPERTY(EditDefaultsOnly, Category = "Spread")
    float SpreadMinDegrees = 1.0;

    UPROPERTY(EditDefaultsOnly, Category = "Spread")
    float SpreadMaxDegrees = 8.0;

    UPROPERTY(EditDefaultsOnly, Category = "Spread")
    float HeatPerShot = 0.12;

    UPROPERTY(EditDefaultsOnly, Category = "Spread")
    float HeatCooldownPerSecond = 0.6;

    // Optional Lyra-shaped heat->spread curve (X = heat 0..1, Y = spread degrees). When points
    // exist they REPLACE the Min/Max lerp: flat early (accurate opening shots), steep late —
    // the Lyra rifle feel. Empty array = legacy linear lerp. (D-0022 phase 5)
    UPROPERTY(EditDefaultsOnly, Category = "Spread")
    TArray<FVector2D> HeatToSpreadCurve;
    default HeatToSpreadCurve.Add(FVector2D(0.0, 1.0));
    default HeatToSpreadCurve.Add(FVector2D(0.35, 1.6));
    default HeatToSpreadCurve.Add(FVector2D(0.7, 3.5));
    default HeatToSpreadCurve.Add(FVector2D(1.0, 8.0));

    UPROPERTY(EditDefaultsOnly, Category = "Spread")
    float SpreadRecoveryDelay = 0.2;

    // Extra spread while the shooter is moving (hip-fire penalty).
    UPROPERTY(EditDefaultsOnly, Category = "Spread")
    float MovingSpreadMultiplier = 1.5;

    // --- Ammo ---
    UPROPERTY(EditDefaultsOnly, Category = "Ammo")
    int MagazineSize = 30;

    UPROPERTY(EditDefaultsOnly, Category = "Ammo")
    float ReloadSeconds = 1.8;

    // --- Recoil (applied to the owning client's aim per shot) ---
    UPROPERTY(EditDefaultsOnly, Category = "Recoil")
    float RecoilPitchPerShot = 0.4;

    UPROPERTY(EditDefaultsOnly, Category = "Recoil")
    float RecoilYawRange = 0.2;

    // --- Focus (light ADS, D-0014): hold-to-aim zooms in and tightens spread. Camera-only zoom; no
    // separate aim rig (the over-the-shoulder camera stays). The strafe slow is on the locomotion comp. ---
    UPROPERTY(EditDefaultsOnly, Category = "Focus")
    float FocusSpreadMultiplier = 0.4;       // spread x this while focusing

    UPROPERTY(EditDefaultsOnly, Category = "Focus")
    float FocusFOV = 70.0;                    // camera FOV when fully focused (hipfire is 90)

    UPROPERTY(EditDefaultsOnly, Category = "Focus")
    float FocusArmLength = 220.0;             // boom pull-in when fully focused (hipfire is 350)

    // --- Cosmetic (assigned per weapon; consumed by FX / montage) ---
    UPROPERTY(EditDefaultsOnly, Category = "Cosmetic")
    USkeletalMesh WeaponMesh;

    UPROPERTY(EditDefaultsOnly, Category = "Cosmetic")
    FName MuzzleSocket = n"Muzzle";

    UPROPERTY(EditDefaultsOnly, Category = "Cosmetic")
    UAnimMontage FireMontage;

    // Anim blueprint the visible gun mesh runs (Lyra weapon ABPs animate bolt/magazine).
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    TSubclassOf<UAnimInstance> WeaponAnimClass;

    // Montages played on the GUN's own mesh (not the character): fire kick / reload sequence.
    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    UAnimMontage WeaponFireMontage;

    UPROPERTY(EditDefaultsOnly, Category = "Animation")
    UAnimMontage WeaponReloadMontage;

    // --- Feel: VFX (all optional; null = debug-line fallback) ---
    UPROPERTY(EditDefaultsOnly, Category = "Feel|VFX")
    UNiagaraSystem MuzzleFlashFX;

    UPROPERTY(EditDefaultsOnly, Category = "Feel|VFX")
    UNiagaraSystem TracerFX;           // expects a user vector param "TracerEnd"

    UPROPERTY(EditDefaultsOnly, Category = "Feel|VFX")
    UNiagaraSystem ImpactWorldFX;

    UPROPERTY(EditDefaultsOnly, Category = "Feel|VFX")
    UNiagaraSystem ImpactEnemyFX;

    // --- Feel: audio (all optional; null = silent) ---
    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase FireSound;              // short per-shot (transient+body); 4-6 round-robins inside the cue

    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase FireTailSound;          // played once on trigger release (the tail sells power); Multicast_FireStopped

    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase ReloadSound;

    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase HitTickSound;           // owning client, confirmed hit

    UPROPERTY(EditDefaultsOnly, Category = "Feel|Audio")
    USoundBase KillConfirmSound;       // owning client, killing blow (Client_KillConfirm)
}
