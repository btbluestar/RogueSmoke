// EnemyTestStand.as
// A single-archetype debug spawner: drops a configurable batch of ONE enemy type around itself at
// BeginPlay through the REAL spawn seam (USpawnDirector), so the enemy behaves exactly as it does in a
// raid (registration, pooling, death, reset). Place one in an isolated level to debug/observe a single
// archetype — and as a manual regression check that a change didn't break it. Optional auto-respawn
// re-populates once the field is clear so you can keep watching without restarting Play. Server-only
// spawning; the label draws on every machine when debug draw is on.
//
// Used by the DL_Enemy_* debugging levels (one archetype each). Configure on the placed actor:
//   - elites  : set EliteClass (Carapace/Spitter/Bloater/Lunger/BroodMother), bFodder = false
//   - swarm   : set bFodder = true (spawns the Crawler/AFodderEnemy via the fodder seam)
class AEnemyTestStand : AActor
{
    default bReplicates = true;

    // The elite archetype to spawn. Ignored when bFodder is true.
    UPROPERTY(EditAnywhere, Category = "Enemy Test")
    TSubclassOf<AEliteEnemyBase> EliteClass;

    // Spawn the C++ swarm fodder (Crawler) through the fodder seam instead of an elite.
    UPROPERTY(EditAnywhere, Category = "Enemy Test")
    bool bFodder = false;

    UPROPERTY(EditAnywhere, Category = "Enemy Test")
    int Count = 3;

    UPROPERTY(EditAnywhere, Category = "Enemy Test")
    float SpawnRadius = 600.0;

    // Re-spawn the batch once every enemy in the world is dead, so the behavior keeps repeating.
    UPROPERTY(EditAnywhere, Category = "Enemy Test")
    bool bAutoRespawn = true;

    UPROPERTY(EditAnywhere, Category = "Enemy Test")
    float RespawnCheckInterval = 3.0;

    // Drawn above the stand (debug) so you can tell which archetype the level is testing.
    UPROPERTY(EditAnywhere, Category = "Enemy Test")
    FString Label = "ENEMY TEST";

    private float CheckTimer = 0.0;
    private bool bSpawnedOnce = false;

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        if (HasAuthority())
            SpawnBatch();
    }

    private void SpawnBatch()
    {
        USpawnDirector Director = USpawnDirector::Get();
        if (Director == nullptr)
            return;

        FVector C = GetActorLocation();
        if (bFodder)
        {
            Director.SpawnFodderWave(C, SpawnRadius, Count);
        }
        else if (EliteClass.Get() != nullptr)
        {
            int N = Math::Max(1, Count);
            for (int i = 0; i < N; i++)
            {
                float Angle = (2.0 * 3.14159265 * i) / N;
                FVector Off = FVector(Math::Cos(Angle), Math::Sin(Angle), 0.0) * SpawnRadius;
                Director.SpawnElite(EliteClass, C + Off, FRotator());
            }
        }
        bSpawnedOnce = true;
        Print(f"[EnemyTest] {Label}: spawned {Count}", 4.0);
    }

    UFUNCTION(BlueprintOverride)
    void Tick(float DeltaSeconds)
    {
        if (RaidDebug::bEnabled)
            System::DrawDebugString(GetActorLocation() + FVector(0.0, 0.0, 160.0), Label, nullptr, FLinearColor::Yellow, 0.0);

        if (!HasAuthority() || !bAutoRespawn || !bSpawnedOnce)
            return;

        CheckTimer += DeltaSeconds;
        if (CheckTimer < RespawnCheckInterval)
            return;
        CheckTimer = 0.0;

        // Respawn only once the whole field is clear (huge radius = "anywhere"); a single stand per level,
        // so this counts exactly what we spawned. CountEnemiesInSphere includes fodder + elites.
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr && Combat.CountEnemiesInSphere(GetActorLocation(), 1000000.0) == 0)
            SpawnBatch();
    }
}
