// UpgradeTestRange.as
// The upgrade-testing firing range (DL_Upgrades). Spawns three labeled dummy formations through
// the REAL spawn seam (USpawnDirector) so targets behave exactly as raid enemies (registry,
// pooling, death, reset). Each formation is shaped for one class of weapon upgrade:
//   - SOLO    : one isolated dummy        -> damage / fire-rate DPS / burn / poison observation
//   - LINE    : a receding row of dummies -> pierce (stand at the range origin, shoot down the row)
//   - CLUSTER : a tight pack of dummies   -> chain arcs (inside the production 600uu chain radius)
// Dummies default to AClusterableElite (passive health bag — it never fights back, so readings
// aren't polluted by the player taking damage). Auto-respawn re-populates once the field is clear.
//
// Workflow (PIE or headless): `ListUpgrades` -> `GrantUpgrade <name>` (repeat to stack) -> shoot
// the matching formation. `UpgradeSmoke` runs the automated GE->attribute battery over the pool.
class AUpgradeTestRange : AActor
{
    default bReplicates = true;

    // The target-dummy archetype. Default is the passive ClusterableElite; swap to an attacking
    // elite on the placed instance if you want live-fire pressure while testing.
    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    TSubclassOf<AEliteEnemyBase> DummyClass = AClusterableElite;

    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    int LineCount = 4;

    // Gap between dummies along the LINE. Keep it bigger than a body so pierce kills are legible.
    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    float LineSpacing = 250.0;

    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    int ClusterCount = 5;

    // Cluster pack radius. Must stay under the production chain radius (600uu) so arcs connect.
    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    float ClusterRadius = 150.0;

    // How far downrange the formations sit from this actor (place the actor at the PlayerStart).
    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    float RangeDistance = 900.0;

    // Sideways offset separating the three lanes (SOLO left, LINE center, CLUSTER right).
    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    float LaneOffset = 700.0;

    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    bool bAutoRespawn = true;

    UPROPERTY(EditAnywhere, Category = "Upgrade Range")
    float RespawnCheckInterval = 3.0;

    private float CheckTimer = 0.0;
    private bool bSpawnedOnce = false;

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        if (HasAuthority())
            SpawnRange();
    }

    private FVector LaneAnchor(float Side) const
    {
        return GetActorLocation()
            + GetActorForwardVector() * RangeDistance
            + GetActorRightVector() * (Side * LaneOffset);
    }

    private void SpawnRange()
    {
        USpawnDirector Director = USpawnDirector::Get();
        if (Director == nullptr || DummyClass.Get() == nullptr)
            return;

        // SOLO lane (left): one isolated dummy.
        Director.SpawnElite(DummyClass, LaneAnchor(-1.0), FRotator());

        // LINE lane (center): a row receding straight downrange from the shooter.
        FVector Forward = GetActorForwardVector();
        for (int i = 0; i < LineCount; i++)
            Director.SpawnElite(DummyClass, LaneAnchor(0.0) + Forward * (LineSpacing * i), FRotator());

        // CLUSTER lane (right): a ring tight enough for production chain arcs.
        FVector ClusterCenter = LaneAnchor(1.0);
        for (int i = 0; i < ClusterCount; i++)
        {
            float Angle = (2.0 * 3.14159265 * i) / Math::Max(1, ClusterCount);
            FVector Off = FVector(Math::Cos(Angle), Math::Sin(Angle), 0.0) * ClusterRadius;
            Director.SpawnElite(DummyClass, ClusterCenter + Off, FRotator());
        }

        bSpawnedOnce = true;
        Print(f"[UpgradeTest] range ready: solo=1 line={LineCount} cluster={ClusterCount}", 5.0);
    }

    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (RaidDebug::bEnabled)
        {
            FVector Up = FVector(0.0, 0.0, 220.0);
            System::DrawDebugString(LaneAnchor(-1.0) + Up, "SOLO — damage / burn / poison", nullptr, FLinearColor::Yellow, 0.0);
            System::DrawDebugString(LaneAnchor(0.0) + Up, "LINE — pierce", nullptr, FLinearColor::Yellow, 0.0);
            System::DrawDebugString(LaneAnchor(1.0) + Up, "CLUSTER — chain", nullptr, FLinearColor::Yellow, 0.0);
        }

        if (!HasAuthority() || !bAutoRespawn || !bSpawnedOnce)
            return;

        CheckTimer += DeltaSeconds;
        if (CheckTimer < RespawnCheckInterval)
            return;
        CheckTimer = 0.0;

        // Re-populate only once the whole field is clear (one range per level, so the global
        // count is exactly what we spawned). Same pattern as EnemyTestStand.
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr && Combat.CountEnemiesInSphere(GetActorLocation(), 1000000.0) == 0)
            SpawnRange();
    }
}
