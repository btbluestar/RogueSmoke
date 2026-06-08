// ClusterableElite.as
// The "worth-taunting" elite (D-0003) — a concrete AEliteEnemyBase (C++) with a visible
// body so you can SEE the pull and the detonation during the Actor-only combo (SETUP §5.4).
// Health, cluster state, registration, and pull-steering all come from the C++ base.
class AClusterableElite : AEliteEnemyBase
{
    UPROPERTY(DefaultComponent, RootComponent)
    UStaticMeshComponent Mesh;
    default Mesh.RelativeScale3D = FVector(1.0, 1.0, 2.0);

    UPROPERTY(EditAnywhere, Category = "Debug")
    bool bShowDebug = true;

    // C++ base already ticks (PrimaryActorTick enabled), so this script Tick fires too.
    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (!RaidDebug::bEnabled || !bShowDebug)
            return;

        // Green = currently Clustered (was taunted); White = normal, taunt-able.
        FLinearColor Color = IsClustered() ? FLinearColor::Green : FLinearColor::White;
        System::DrawDebugSphere(GetActorLocation() + FVector(0.0, 0.0, 150.0), 35.0, 8, Color, 0.0, 2.0);

        if (Health != nullptr)
            System::DrawDebugString(GetActorLocation() + FVector(0.0, 0.0, 210.0),
                f"HP {int(Health.Health)}/{int(Health.MaxHealth)}", nullptr, Color, 0.0);
    }
}
