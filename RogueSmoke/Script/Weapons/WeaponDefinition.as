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

    // --- Cosmetic (assigned per weapon; consumed by FX / montage) ---
    UPROPERTY(EditDefaultsOnly, Category = "Cosmetic")
    USkeletalMesh WeaponMesh;

    UPROPERTY(EditDefaultsOnly, Category = "Cosmetic")
    FName MuzzleSocket = n"Muzzle";

    UPROPERTY(EditDefaultsOnly, Category = "Cosmetic")
    UAnimMontage FireMontage;

    UPROPERTY(EditDefaultsOnly, Category = "Cosmetic")
    USoundBase FireSound;
}
