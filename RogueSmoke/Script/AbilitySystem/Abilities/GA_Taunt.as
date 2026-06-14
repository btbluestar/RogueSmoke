// GA_Taunt.as
// SETUP half of the signature synergy (D-0008), now a GAS ability. Pulls nearby enemies into a
// knot around the caster and flags them "Clustered" so payoff abilities reward density.
// v3 (D-0020): radius/duration grow via URogueCombatSet attributes; TauntDamage makes the taunt
// hit (Concussive Taunt); TauntVortex turns it into a re-pulling vortex (Event Horizon).
class UGA_Taunt : UGA_RogueAbility
{
    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float Radius = 800.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float PullStrength = 1200.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float ClusterDuration = 3.0;

    // Event Horizon: re-pull + refresh Clustered every VortexInterval for VortexDuration.
    UPROPERTY(EditDefaultsOnly, Category = "Taunt|Vortex")
    float VortexDuration = 3.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt|Vortex")
    float VortexInterval = 0.5;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt|Vortex")
    float VortexStrengthFraction = 0.6;

    private FVector VortexCenter;
    private int VortexPulsesRemaining = 0;

    UFUNCTION(BlueprintOverride)
    void ActivateAbility()
    {
        // Commit pays cost + starts cooldown (via the assigned Cooldown GE). Bail if it fails.
        if (!CommitAbility())
        {
            EndAbility();
            return;
        }

        // Server-authoritative seam work only (clients predict the cosmetic, server does the pull).
        if (HasAuthority())
        {
            UCombatSubsystem Combat = UCombatSubsystem::Get();
            if (Combat != nullptr)
            {
                FVector Location = GetActivationLocation();
                TauntPulse(Combat, Location, PullStrength);

                // Concussive Taunt: the taunt now hits. AbilityPower-scaled; cluster multiplier
                // 1.0 so it never double-dips with Barrage's cluster payoff.
                float TauntDmg = GetCombatAttribute(n"TauntDamage");
                if (TauntDmg > 0.0)
                {
                    Combat.ApplyRadialDamage(Location, EffectiveRadius(),
                        TauntDmg * GetCombatAttribute(n"AbilityPower", 1.0), 1.0,
                        GetAvatarActorFromActorInfo());
                }

                // Event Horizon: keep pulsing on a timer; EndAbility deferred to the last pulse.
                if (GetCombatAttribute(n"TauntVortex") >= 1.0)
                {
                    VortexCenter = Location;
                    VortexPulsesRemaining = Math::Max(int(VortexDuration / VortexInterval), 1);
                    System::SetTimer(this, n"VortexPulse", VortexInterval, true);
                    Print("TAUNT: EVENT HORIZON — vortex active", 2.0);
                    return;
                }
            }
        }

        // Cosmetic cue (replace Print with a GameplayCue asset). Predicted on the local client.
        Print("TAUNT: enemies pulled + marked Clustered", 2.0);

        EndAbility();
    }

    private float EffectiveRadius() const
    {
        return Radius + GetCombatAttribute(n"TauntRadiusBonus");
    }

    private float EffectiveClusterDuration() const
    {
        return ClusterDuration + GetCombatAttribute(n"TauntClusterDurationBonus");
    }

    private void TauntPulse(UCombatSubsystem Combat, FVector Location, float Strength)
    {
        Combat.PullEnemiesToward(Location, EffectiveRadius(), Strength, EffectiveClusterDuration());  // SETUP, half 1
        Combat.MarkClustered(Location, EffectiveRadius(), EffectiveClusterDuration());                // SETUP, half 2
    }

    UFUNCTION()
    private void VortexPulse()
    {
        VortexPulsesRemaining -= 1;
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr)
            TauntPulse(Combat, VortexCenter, PullStrength * VortexStrengthFraction);
        if (VortexPulsesRemaining <= 0)
        {
            System::ClearTimer(this, "VortexPulse");
            EndAbility();
        }
    }
}
