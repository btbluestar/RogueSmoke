// GA_Barrage.as
// PAYOFF half of the signature synergy (D-0008), now a GAS ability. Radial damage that rewards
// density: Clustered enemies (set by Taunt) eat a bonus multiplier. v3 (D-0020): damage scales
// via BarrageDamageBonus; BarrageSalvoCount adds echo strikes (Twin Salvo); BarrageCarpet turns
// the strike into a telegraphed strip marching along the caster's facing (Carpet Bombing).
class UGA_Barrage : UGA_RogueAbility
{
    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float Radius = 600.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float BaseDamage = 40.0;

    // The synergy payoff: clustered enemies take this multiple of damage.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage")
    float ClusterBonusMultiplier = 2.0;

    // Twin Salvo: each extra strike echoes at the same center for a damage fraction.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Salvo")
    float SalvoDelay = 0.4;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Salvo")
    float EchoDamageFraction = 0.6;

    // Carpet Bombing: pads march along the caster's facing, each telegraphed before impact.
    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    int CarpetSteps = 5;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    float CarpetStepDistance = 300.0;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    float CarpetStepInterval = 0.25;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    float CarpetDamageFraction = 0.6;

    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    float CarpetRadiusFraction = 0.6;

    // Telegraph lead in step-timer ticks: rings and blasts share one clock (GDD §10 readability).
    UPROPERTY(EditDefaultsOnly, Category = "Barrage|Carpet")
    int CarpetTelegraphLeadTicks = 2;

    private FVector StrikeCenter;
    private FVector CarpetDir;
    private int SalvoStrikesRemaining = 0;
    private int CarpetTick = 0;
    private float EffRadius = 0.0;
    private float EffDamage = 0.0;
    private float EffCluster = 0.0;

    UFUNCTION(BlueprintOverride)
    void ActivateAbility()
    {
        if (!CommitAbility())
        {
            EndAbility();
            return;
        }

        if (HasAuthority())
        {
            UCombatSubsystem Combat = UCombatSubsystem::Get();
            if (Combat != nullptr)
            {
                // Upgrades add to these via URogueCombatSet (Chain Detonation, High Explosives...).
                StrikeCenter = GetActivationLocation();
                EffRadius = Radius + GetCombatAttribute(n"BarrageRadiusBonus");
                EffDamage = BaseDamage * (1.0 + GetCombatAttribute(n"BarrageDamageBonus"));
                EffCluster = ClusterBonusMultiplier + GetCombatAttribute(n"BarrageClusterBonus");

                // Carpet Bombing replaces the single strike (and any salvo) with the strip.
                if (GetCombatAttribute(n"BarrageCarpet") >= 1.0)
                {
                    AActor Avatar = GetAvatarActorFromActorInfo();
                    CarpetDir = Avatar != nullptr ? Avatar.GetActorForwardVector() : FVector(1.0, 0.0, 0.0);
                    CarpetDir.Z = 0.0;
                    CarpetDir = CarpetDir.GetSafeNormal();
                    CarpetTick = 0;
                    System::SetTimer(this, n"CarpetStep", CarpetStepInterval, true);
                    Print("BARRAGE: CARPET BOMBING — strip inbound", 2.0);
                    return;   // EndAbility deferred to the last pad
                }

                Strike(Combat, StrikeCenter, EffRadius, EffDamage);

                SalvoStrikesRemaining = int(GetCombatAttribute(n"BarrageSalvoCount"));
                if (SalvoStrikesRemaining > 0)
                {
                    System::SetTimer(this, n"SalvoStrike", SalvoDelay, true);
                    return;   // EndAbility deferred to the last echo
                }
            }
        }

        EndAbility();
    }

    private void Strike(UCombatSubsystem Combat, FVector Center, float StrikeRadius, float Damage)
    {
        int HitCount = Combat.CountEnemiesInSphere(Center, StrikeRadius);
        float Dealt = Combat.ApplyRadialDamage(Center, StrikeRadius, Damage, EffCluster,
                                               GetAvatarActorFromActorInfo());
        Print(f"BARRAGE hit {HitCount} enemies for {Dealt} total", 2.0);
    }

    UFUNCTION()
    private void SalvoStrike()
    {
        SalvoStrikesRemaining -= 1;
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr)
            Strike(Combat, StrikeCenter, EffRadius, EffDamage * EchoDamageFraction);
        if (SalvoStrikesRemaining <= 0)
        {
            System::ClearTimer(this, "SalvoStrike");
            EndAbility();
        }
    }

    // One clock: tick T telegraphs pad T and detonates pad T - CarpetTelegraphLeadTicks, so the
    // ring's fill reaches the edge exactly at impact (the telegraph contract, GDD §10).
    UFUNCTION()
    private void CarpetStep()
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
        {
            System::ClearTimer(this, "CarpetStep");
            EndAbility();
            return;
        }

        float PadRadius = EffRadius * CarpetRadiusFraction;
        if (CarpetTick < CarpetSteps)
            Combat.ShowTelegraphZone(PadCenter(CarpetTick), PadRadius, CarpetStepInterval * float(CarpetTelegraphLeadTicks));

        int BlastIdx = CarpetTick - CarpetTelegraphLeadTicks;
        if (BlastIdx >= 0 && BlastIdx < CarpetSteps)
            Strike(Combat, PadCenter(BlastIdx), PadRadius, EffDamage * CarpetDamageFraction);

        CarpetTick += 1;
        if (CarpetTick >= CarpetSteps + CarpetTelegraphLeadTicks)
        {
            System::ClearTimer(this, "CarpetStep");
            EndAbility();
        }
    }

    private FVector PadCenter(int StepIdx) const
    {
        return StrikeCenter + CarpetDir * CarpetStepDistance * float(StepIdx + 1);
    }
}
