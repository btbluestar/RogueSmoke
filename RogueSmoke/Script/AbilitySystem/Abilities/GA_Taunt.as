// GA_Taunt.as
// SETUP half of the signature synergy (D-0008), now a GAS ability. Pulls nearby enemies into a
// knot around the caster and flags them "Clustered" so payoff abilities reward density.
class UGA_Taunt : UGA_RogueAbility
{
    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float Radius = 800.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float PullStrength = 1200.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float ClusterDuration = 3.0;

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
                Combat.PullEnemiesToward(Location, Radius, PullStrength, ClusterDuration);  // SETUP, half 1
                Combat.MarkClustered(Location, Radius, ClusterDuration);                    // SETUP, half 2
            }
        }

        // Cosmetic cue (replace Print with a GameplayCue asset). Predicted on the local client.
        Print("TAUNT: enemies pulled + marked Clustered", 2.0);

        EndAbility();
    }
}
