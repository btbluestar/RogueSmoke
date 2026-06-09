// Bloater.as
// Exploder / suicide bomber: rushes in, swells (telegraph), and detonates a big radial blast on contact —
// and on death, so shooting it just moves the blast away from you. Punishes letting it reach you, and adds
// a risk twist to clustering (a taunted Bloater that pops mid-cluster). The blast fires from OnDeath, so a
// single death = a single explosion whether it reached you or was shot first (pool-safe; no extra flag —
// HealthComponent guards OnDeath re-entry and re-arms it on ResetHealth).
class ABloater : AAttackingElite
{
    default MaxHealthOverride = 60.0;
    default MoveSpeed = 175.0;
    default PreferredRange = 60.0;      // gets right on top of you
    default AttackRange = 170.0;
    default AttackDamage = 40.0;        // hurts
    default AttackRadius = 320.0;       // large blast
    default AttackInterval = 1.0;
    default TelegraphSeconds = 0.7;     // the swell tell
    default BodyScale = FVector(1.4, 1.4, 1.4);

    UFUNCTION(BlueprintOverride)
    void BeginPlay()
    {
        // The C++ AAttackingElite::BeginPlay already ran (HP override + body scale); here we just arm the
        // death blast. BeginPlay fires once per actor lifetime (pool reuse goes through Activate), so this
        // binds exactly once.
        if (HasAuthority() && Health != nullptr)
            Health.OnDeath.AddUFunction(this, n"OnDied");
    }

    // Reached a player after the wind-up: commit suicide. Death (below) is what actually detonates, so the
    // contact blast and the shot-before-contact blast are the same single code path.
    UFUNCTION(BlueprintOverride)
    void PerformAttack()
    {
        if (Health != nullptr && Health.Health > 0.0)
            Health.ApplyDamage(99999.0, this);
    }

    UFUNCTION()
    void OnDied(AActor DeadActor)
    {
        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat != nullptr)
            Combat.ApplyRadialDamageToPlayers(GetActorLocation(), AttackRadius, AttackDamage, this);
    }
}
