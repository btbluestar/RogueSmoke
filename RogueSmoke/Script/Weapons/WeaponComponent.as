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

    // Weapon-upgrade bonuses (WEAPON_UPGRADES_PLAN.md): additive fractions mirrored from the
    // owner's URogueCombatSet by the hero's attribute-changed callbacks (server-side, where all
    // weapon state lives). Applied at the choke points below, never baked into the Definition —
    // the DataAsset stays the designer's source of truth.
    private float FireRateBonus = 0.0;
    private float ReloadSpeedBonus = 0.0;
    private float MagazineBonus = 0.0;

    // Pushed by AHeroCharacter when the matching attributes change (upgrade GEs applying).
    // A magazine upgrade mid-life takes effect on the next reload — no retroactive top-up.
    void SetUpgradeBonuses(float InFireRateBonus, float InReloadSpeedBonus, float InMagazineBonus)
    {
        FireRateBonus = Math::Max(InFireRateBonus, 0.0);
        ReloadSpeedBonus = Math::Max(InReloadSpeedBonus, 0.0);
        MagazineBonus = Math::Max(InMagazineBonus, 0.0);
    }

    int GetEffectiveMagazineSize() const
    {
        if (Definition == nullptr)
            return 0;
        return Math::CeilToInt(float(Definition.MagazineSize) * (1.0 + MagazineBonus));
    }

    void EquipWeapon(URogueWeaponDefinition NewDefinition)
    {
        Definition = NewDefinition;
        AmmoInMag = GetEffectiveMagazineSize();
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

    // Full spread cone angle in degrees, factoring heat + movement + focus (light ADS tightens it).
    float GetSpreadDegrees(bool bMoving, bool bFocusing) const
    {
        if (Definition == nullptr)
            return 0.0;

        float Heat = Math::Clamp(CurrentHeat, 0.0, 1.0);
        float Spread;
        if (Definition.HeatToSpreadCurve.Num() >= 2)
            Spread = EvalHeatToSpread(Heat);
        else
            Spread = Math::Lerp(Definition.SpreadMinDegrees, Definition.SpreadMaxDegrees, Heat);
        if (bMoving)
            Spread *= Definition.MovingSpreadMultiplier;
        if (bFocusing)
            Spread *= Definition.FocusSpreadMultiplier;
        return Spread;
    }

    // Piecewise-linear eval of Definition.HeatToSpreadCurve (points sorted by X at authoring time).
    private float EvalHeatToSpread(float Heat) const
    {
        const TArray<FVector2D>& Pts = Definition.HeatToSpreadCurve;
        if (Heat <= Pts[0].X)
            return Pts[0].Y;
        for (int i = 1; i < Pts.Num(); i++)
        {
            if (Heat <= Pts[i].X)
            {
                float T = (Heat - Pts[i - 1].X) / Math::Max(Pts[i].X - Pts[i - 1].X, 0.001);
                return Math::Lerp(Pts[i - 1].Y, Pts[i].Y, T);
            }
        }
        return Pts[Pts.Num() - 1].Y;
    }

    // Called right after a shot resolves (server). Spends ammo, heats up, starts the refire gate.
    void NotifyFired()
    {
        if (Definition == nullptr)
            return;

        if (AmmoInMag > 0)
            AmmoInMag -= 1;
        // Fire-rate upgrades shrink the refire gate (works for semi and full-auto alike).
        RefireCooldown = Definition.FireInterval / (1.0 + FireRateBonus);
        TimeSinceLastShot = 0.0;
        CurrentHeat = Math::Min(1.0, CurrentHeat + Definition.HeatPerShot);

        if (AmmoInMag <= 0)
            StartReload();
    }

    void StartReload()
    {
        if (Definition == nullptr || bIsReloading)
            return;
        if (AmmoInMag >= GetEffectiveMagazineSize())
            return;

        bIsReloading = true;
        ReloadRemaining = Definition.ReloadSeconds / (1.0 + ReloadSpeedBonus);
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
                AmmoInMag = GetEffectiveMagazineSize();
                bIsReloading = false;
            }
        }
        else if (TimeSinceLastShot >= Definition.SpreadRecoveryDelay && CurrentHeat > 0.0)
        {
            CurrentHeat = Math::Max(0.0, CurrentHeat - Definition.HeatCooldownPerSecond * DeltaSeconds);
        }
    }
}
