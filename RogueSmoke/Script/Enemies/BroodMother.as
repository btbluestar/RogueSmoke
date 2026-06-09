// BroodMother.as
// Mini-boss / raid anchor (GDD §4.4 boss): big HP, slow, and the climax of "clear the arena". Cycles
// three telegraphed attacks — a ranged spit, a summoned Crawler wave (calls the spawn seam), and an
// artillery AoE that lands at the target's CURRENT position so movement dodges it. Counts toward the
// objective (inherited bCountsAsObjectiveTarget = true), so the raid isn't clear until the boss is down.
// A boss healthbar + phase VFX are editor/content polish (see SUPERPOWERS_HANDOFF).
class ABroodMother : AAttackingElite
{
    default MaxHealthOverride = 1200.0;
    default MoveSpeed = 90.0;
    default PreferredRange = 600.0;     // hangs back; it summons + lobs rather than melees
    default AttackRange = 1500.0;       // long reach (ranged + artillery)
    default AttackDamage = 18.0;        // ranged spit damage
    default AttackInterval = 3.5;
    default TelegraphSeconds = 1.0;     // big, readable wind-ups
    default BodyScale = FVector(2.6, 2.6, 2.8);   // unmistakable silhouette

    UPROPERTY(EditDefaultsOnly, Category = "Boss")
    int SummonCount = 6;

    UPROPERTY(EditDefaultsOnly, Category = "Boss")
    float SummonRadius = 350.0;

    UPROPERTY(EditDefaultsOnly, Category = "Boss")
    float ArtilleryDamage = 30.0;

    UPROPERTY(EditDefaultsOnly, Category = "Boss")
    float ArtilleryRadius = 450.0;

    // Don't summon while at least this many enemies are already alive (so the boss can't flood the arena).
    UPROPERTY(EditDefaultsOnly, Category = "Boss")
    int MaxFieldEnemies = 40;

    // Cycles 0 -> 1 -> 2 across attacks so the boss reads as a rotation, not a spam.
    private int AttackPhase = 0;

    UFUNCTION(BlueprintOverride)
    void PerformAttack()
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        APawn Hero = GetCurrentTarget();

        if (AttackPhase == 1)
        {
            // Summon a Crawler wave around the boss (the spawn seam; Mass fodder later) — but only if the
            // field isn't already saturated, so the boss can't bury the arena in fodder.
            USpawnDirector Director = USpawnDirector::Get();
            bool bRoomToSpawn = Combat == nullptr
                || Combat.CountEnemiesInSphere(GetActorLocation(), 1000000.0) < MaxFieldEnemies;
            if (Director != nullptr && bRoomToSpawn)
                Director.SpawnFodderWave(GetActorLocation(), SummonRadius, SummonCount);
        }
        else if (AttackPhase == 2)
        {
            // Artillery: blast the target's CURRENT spot, so moving during the wind-up avoids it.
            if (Combat != nullptr && Hero != nullptr)
                Combat.ApplyRadialDamageToPlayers(Hero.GetActorLocation(), ArtilleryRadius, ArtilleryDamage, this);
        }
        else
        {
            // Ranged spit at the target.
            if (Combat != nullptr && Hero != nullptr)
                Combat.ApplyDamageToPlayer(Hero, AttackDamage, this);
        }

        AttackPhase = (AttackPhase + 1) % 3;
    }
}
