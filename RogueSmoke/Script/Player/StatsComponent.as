// StatsComponent.as
// The tunable character stat block for players. Lives in AngelScript on purpose: these are
// the numbers you tune constantly, and hot-reload lets you do it live in PIE (D-0002,
// "script the decisions"). Elites use the lean C++ UHealthComponent in the hot path; players
// get this richer, iteration-friendly block.
//
// Authority: health/shield mutation is server-only (CODING_STANDARDS §4.4). Current values
// replicate so teammates' HUDs can read them. Tunable defaults are EditAnywhere so designers
// (and BP/script subclasses) override them per hero.
//
// Damage model: incoming damage is reduced by Armor, then soaked by Shield, then taken from
// Health. Outgoing-damage / utility stats (DamageMultiplier, CritChance, Luck, ...) are read
// by the systems that use them (abilities, loot rolls) — they're not applied here.
class UStatsComponent : UActorComponent
{
    default bReplicates = true;

    // ============================ SURVIVAL ============================
    UPROPERTY(EditAnywhere, Category = "Stats|Survival")
    float MaxHealth = 100.0;

    UPROPERTY(Replicated, ReplicatedUsing = OnRep_Health, BlueprintReadOnly, Category = "Stats|Survival")
    float Health = 100.0;

    // HP per second, applied only after HealthRegenDelay has elapsed since the last damage.
    UPROPERTY(EditAnywhere, Category = "Stats|Survival")
    float HealthRegenPerSecond = 1.0;

    UPROPERTY(EditAnywhere, Category = "Stats|Survival")
    float HealthRegenDelay = 3.0;

    // Damage reduction. For Armor >= 0: mitigated% = Armor / (Armor + 100) (RoR2-style) —
    // 100 armor = 50% reduction, diminishing. Negative armor amplifies incoming damage.
    UPROPERTY(EditAnywhere, Category = "Stats|Survival")
    float Armor = 0.0;

    // Regenerating overshield, absorbed before Health. Leave MaxShield = 0 to disable.
    UPROPERTY(EditAnywhere, Category = "Stats|Survival")
    float MaxShield = 0.0;

    UPROPERTY(Replicated, ReplicatedUsing = OnRep_Shield, BlueprintReadOnly, Category = "Stats|Survival")
    float Shield = 0.0;

    UPROPERTY(EditAnywhere, Category = "Stats|Survival")
    float ShieldRechargeDelay = 5.0;

    UPROPERTY(EditAnywhere, Category = "Stats|Survival")
    float ShieldRechargePerSecond = 10.0;

    // ============================ OFFENSE =============================
    UPROPERTY(EditAnywhere, Category = "Stats|Offense")
    float DamageMultiplier = 1.0;          // scales outgoing weapon/attack damage

    UPROPERTY(EditAnywhere, Category = "Stats|Offense")
    float AbilityPowerMultiplier = 1.0;    // scales ability damage / effect magnitude

    UPROPERTY(EditAnywhere, Category = "Stats|Offense")
    float AttackSpeedMultiplier = 1.0;

    UPROPERTY(EditAnywhere, Category = "Stats|Offense", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float CritChance = 0.05;               // 0..1

    UPROPERTY(EditAnywhere, Category = "Stats|Offense")
    float CritMultiplier = 2.0;

    // 0..0.9: shortens ability cooldowns. Synergy-relevant (a faster taunt→barrage loop).
    UPROPERTY(EditAnywhere, Category = "Stats|Offense", meta = (ClampMin = "0.0", ClampMax = "0.9"))
    float CooldownReduction = 0.0;

    // ============================ MOBILITY ============================
    // Applied to the owning Character's MaxWalkSpeed on spawn / when refreshed.
    UPROPERTY(EditAnywhere, Category = "Stats|Mobility")
    float MoveSpeed = 600.0;

    // ============================ UTILITY =============================
    UPROPERTY(EditAnywhere, Category = "Stats|Utility")
    float PickupRadius = 200.0;

    // Biases upgrade/loot rolls — applied through the seeded RNG, never unseeded (D-0007).
    UPROPERTY(EditAnywhere, Category = "Stats|Utility")
    float Luck = 0.0;

    // ============================== CO-OP =============================
    UPROPERTY(EditAnywhere, Category = "Stats|Co-op")
    float ReviveSpeedMultiplier = 1.0;     // how fast you revive a downed ally

    // ============================== STATE =============================
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Stats|State")
    bool bIsDead = false;

    private float TimeSinceDamage = 999.0;  // server-side regen/recharge gate

    // ---------------------------------------------------------------- lifecycle
    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        Health = MaxHealth;
        Shield = MaxShield;
        ApplyMoveSpeed();
    }

    // Overriding Tick is what enables ticking in the fork (no bCanEverTick default).
    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (!HasAuthority() || bIsDead)
            return;

        TimeSinceDamage += DeltaSeconds;

        if (HealthRegenPerSecond > 0.0 && Health < MaxHealth && TimeSinceDamage >= HealthRegenDelay)
            SetHealth(Math::Min(MaxHealth, Health + HealthRegenPerSecond * DeltaSeconds));

        if (MaxShield > 0.0 && Shield < MaxShield && TimeSinceDamage >= ShieldRechargeDelay)
            Shield = Math::Min(MaxShield, Shield + ShieldRechargePerSecond * DeltaSeconds);
    }

    // ---------------------------------------------------------------- damage / heal
    // Server-authoritative. Returns damage actually removed from Health.
    UFUNCTION(BlueprintCallable, Category = "Stats")
    float ApplyDamage(float RawAmount, AActor DamageInstigator)
    {
        if (!HasAuthority() || bIsDead || RawAmount <= 0.0)
            return 0.0;

        float Amount = GetMitigatedDamage(RawAmount);
        TimeSinceDamage = 0.0;

        if (Shield > 0.0)                       // shield soaks first
        {
            float Absorbed = Math::Min(Shield, Amount);
            Shield -= Absorbed;
            Amount -= Absorbed;
        }

        float Before = Health;
        SetHealth(Math::Max(0.0, Health - Amount));
        float Dealt = Before - Health;

        OnDamaged(Dealt, DamageInstigator);

        if (Health <= 0.0 && !bIsDead)
        {
            bIsDead = true;
            OnDeath(DamageInstigator);
        }
        return Dealt;
    }

    UFUNCTION(BlueprintCallable, Category = "Stats")
    void Heal(float Amount)
    {
        if (!HasAuthority() || bIsDead || Amount <= 0.0)
            return;
        SetHealth(Math::Min(MaxHealth, Health + Amount));
    }

    // Reset for spawn / respawn / revive.
    UFUNCTION(BlueprintCallable, Category = "Stats")
    void RestoreToFull()
    {
        if (!HasAuthority())
            return;
        bIsDead = false;
        SetHealth(MaxHealth);
        Shield = MaxShield;
        TimeSinceDamage = 999.0;
    }

    // ---------------------------------------------------------------- queries
    UFUNCTION(BlueprintPure, Category = "Stats")
    float GetMitigatedDamage(float Raw) const
    {
        if (Armor >= 0.0)
            return Raw * (100.0 / (100.0 + Armor));
        return Raw * (2.0 - 100.0 / (100.0 - Armor));   // negative armor amplifies
    }

    UFUNCTION(BlueprintPure, Category = "Stats")
    float GetHealthPercent() const
    {
        return MaxHealth > 0.0 ? Health / MaxHealth : 0.0;
    }

    // Cooldown after CooldownReduction — abilities call this so the CDR stat matters.
    UFUNCTION(BlueprintPure, Category = "Stats")
    float GetEffectiveCooldown(float BaseCooldown) const
    {
        return BaseCooldown * (1.0 - Math::Clamp(CooldownReduction, 0.0, 0.9));
    }

    UFUNCTION(BlueprintPure, Category = "Stats")
    bool IsDead() const { return bIsDead; }

    // ---------------------------------------------------------------- helpers
    bool HasAuthority() const
    {
        AActor Owner = GetOwner();
        return Owner != nullptr && Owner.HasAuthority();
    }

    private void SetHealth(float NewHealth)
    {
        Health = NewHealth;
        OnHealthChanged();
    }

    void ApplyMoveSpeed()
    {
        ACharacter Character = Cast<ACharacter>(GetOwner());
        if (Character != nullptr && Character.CharacterMovement != nullptr)
            Character.CharacterMovement.MaxWalkSpeed = MoveSpeed;
    }

    // Clients drive the HUD off these.
    UFUNCTION()
    void OnRep_Health() { OnHealthChanged(); }
    UFUNCTION()
    void OnRep_Shield() { }

    // ---------------------------------------------------------------- overridable hooks
    // Plain methods = virtual in AngelScript. Subclass UStatsComponent (or react via OnRep)
    // to drive real UI/VFX/SFX; the Print is just so it's visible in the first playtest.
    void OnHealthChanged() {}
    void OnDamaged(float Amount, AActor DamageInstigator) {}
    void OnDeath(AActor Killer)
    {
        Print("HERO DOWN", 5.0);
    }
}
