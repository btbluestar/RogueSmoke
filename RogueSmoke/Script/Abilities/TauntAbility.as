// TauntAbility.as
// The SETUP half of the signature synergy (D-0008). Pulls nearby enemies into a knot
// around the caster and flags them "Clustered" so payoff abilities reward density.
class UTauntAbilityComponent : UAbilityComponent
{
    default Cooldown = 8.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float Radius = 800.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float PullStrength = 1200.0;

    UPROPERTY(EditDefaultsOnly, Category = "Taunt")
    float ClusterDuration = 3.0;

    void ExecuteOnServer(FVector Location) override
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
            return;

        // SETUP, half 1: drag nearby enemies into a tight knot around the caster.
        Combat.PullEnemiesToward(Location, Radius, PullStrength, ClusterDuration);

        // SETUP, half 2: flag them "Clustered" so payoff abilities reward density.
        Combat.MarkClustered(Location, Radius, ClusterDuration);
    }

    void PlayCosmetics(FVector Location) override
    {
        // Distinct "SETUP READY" cue (GDD §5.3 / §10). Replace with VFX/SFX.
        Print("TAUNT: enemies pulled + marked Clustered", 2.0);

        LastTauntLocation = Location;            // debug: the "knot" enemies are pulled to
        TauntMarkerRemaining = ClusterDuration;
    }

    float GetDebugRadius() const override { return Radius; }

    // ---- debug: show the pull destination for the cluster duration ----
    private FVector LastTauntLocation;
    private float TauntMarkerRemaining = 0.0;

    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        Super::Tick(DeltaSeconds);   // base: cooldown + ability-range debug sphere

        if (TauntMarkerRemaining > 0.0)
        {
            TauntMarkerRemaining -= DeltaSeconds;
            if (RaidDebug::bEnabled && bShowDebug)
                System::DrawDebugSphere(LastTauntLocation, 60.0, 12, FLinearColor::Yellow, 0.0, 3.0);
        }
    }
}
