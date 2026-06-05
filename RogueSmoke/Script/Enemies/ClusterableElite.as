// ClusterableElite.as
// The "worth-taunting" elite (D-0003) — a concrete AEliteEnemyBase (C++) with a visible
// body so you can SEE the pull and the detonation during the Actor-only combo (SETUP §5.4).
// Health, cluster state, registration, and pull-steering all come from the C++ base.
class AClusterableElite : AEliteEnemyBase
{
    UPROPERTY(DefaultComponent, RootComponent)
    UStaticMeshComponent Mesh;
    default Mesh.RelativeScale3D = FVector(1.0, 1.0, 2.0);
}
