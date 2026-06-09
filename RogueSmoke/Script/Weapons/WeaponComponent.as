// WeaponComponent.as
// Runtime weapon "instance" on a hero (Lyra's ULyraRangedWeaponInstance role): holds the equipped
// definition + per-life state (ammo, spread heat, refire + reload timers). Server-authoritative; the
// hero drives timing from its Tick via delta seconds, so no world-clock or timers are needed.
//
// MVP: state is server-side only (listen-server host sees it). Replicating AmmoInMag + Definition for
// remote clients' HUD/FX is a follow-up (see plan).
class URogueWeaponComponent : UActorComponent
{
    // The equipped weapon. Set via EquipWeapon(); null until a hero equips a DefaultWeapon.
    UPROPERTY(BlueprintReadOnly, Category = "Weapon")
    URogueWeaponDefinition Definition;

    UPROPERTY(BlueprintReadOnly, Category = "Weapon")
    int AmmoInMag = 0;

    // Normalized spread heat, 0 (SpreadMin) .. 1 (SpreadMax). Server-only.
    private float CurrentHeat = 0.0;
    private float RefireCooldown = 0.0;       // seconds until the next shot is allowed
    private float TimeSinceLastShot = 1000.0; // large so heat recovery is allowed at start
    private bool bIsReloading = false;
    private float ReloadRemaining = 0.0;

    void EquipWeapon(URogueWeaponDefinition NewDefinition)
    {
        Definition = NewDefinition;
        AmmoInMag = (Definition != nullptr) ? Definition.MagazineSize : 0;
        CurrentHeat = 0.0;
        RefireCooldown = 0.0;
        TimeSinceLastShot = 1000.0;
        bIsReloading = false;
        ReloadRemaining = 0.0;
    }

    bool CanFire() const
    {
        return Definition != nullptr && !bIsReloading && AmmoInMag > 0 && RefireCooldown <= 0.0;
    }

    bool IsReloading() const { return bIsReloading; }

    // Full spread cone angle in degrees, factoring heat + movement.
    float GetSpreadDegrees(bool bMoving) const
    {
        if (Definition == nullptr)
            return 0.0;

        float Spread = Math::Lerp(Definition.SpreadMinDegrees, Definition.SpreadMaxDegrees,
                                  Math::Clamp(CurrentHeat, 0.0, 1.0));
        if (bMoving)
            Spread *= Definition.MovingSpreadMultiplier;
        return Spread;
    }

    // Called right after a shot resolves (server). Spends ammo, heats up, starts the refire gate.
    void NotifyFired()
    {
        if (Definition == nullptr)
            return;

        if (AmmoInMag > 0)
            AmmoInMag -= 1;
        RefireCooldown = Definition.FireInterval;
        TimeSinceLastShot = 0.0;
        CurrentHeat = Math::Min(1.0, CurrentHeat + Definition.HeatPerShot);

        if (AmmoInMag <= 0)
            StartReload();
    }

    void StartReload()
    {
        if (Definition == nullptr || bIsReloading)
            return;
        if (AmmoInMag >= Definition.MagazineSize)
            return;

        bIsReloading = true;
        ReloadRemaining = Definition.ReloadSeconds;
    }

    // Driven by the owning hero's Tick (server only). Advances refire, reload, and heat recovery.
    void TickWeapon(float DeltaSeconds)
    {
        if (Definition == nullptr)
            return;

        if (RefireCooldown > 0.0)
            RefireCooldown -= DeltaSeconds;

        TimeSinceLastShot += DeltaSeconds;

        if (bIsReloading)
        {
            ReloadRemaining -= DeltaSeconds;
            if (ReloadRemaining <= 0.0)
            {
                AmmoInMag = Definition.MagazineSize;
                bIsReloading = false;
            }
        }
        else if (TimeSinceLastShot >= Definition.SpreadRecoveryDelay && CurrentHeat > 0.0)
        {
            CurrentHeat = Math::Max(0.0, CurrentHeat - Definition.HeatCooldownPerSecond * DeltaSeconds);
        }
    }
}
