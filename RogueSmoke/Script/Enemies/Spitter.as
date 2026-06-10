// Spitter.as
// Ranged special: holds at distance and lobs an arcing acid glob (ASpitterProjectile) after a telegraph,
// to punish turtling and force repositioning. Hangs back via a large PreferredRange; the wind-up is the
// dodge window, and the glob lands at the target's position AT FIRE TIME so moving dodges it. The shot is
// line-of-sight gated, so breaking LoS (cover) also denies it.
class ASpitter : AAttackingElite
{
    default MaxHealthOverride = 70.0;
    default MoveSpeed = 190.0;
    default PreferredRange = 750.0;     // kite: stop well short and shoot
    default AttackRange = 1000.0;
    default AttackDamage = 12.0;
    default AttackRadius = 0.0;         // the projectile applies its own splash
    default AttackInterval = 2.2;
    default TelegraphSeconds = 0.6;
    default BodyScale = FVector(0.9, 0.9, 1.5);

    UPROPERTY(EditDefaultsOnly, Category = "Enemy")
    float ProjectileSpeed = 1500.0;

    // Small splash so a near-miss still grazes, but moving out of the landing spot avoids it.
    UPROPERTY(EditDefaultsOnly, Category = "Enemy")
    float ProjectileSplash = 180.0;

    UFUNCTION(BlueprintOverride)
    void PerformAttack()
    {
        APawn Hero = GetCurrentTarget();
        if (Hero == nullptr)
            return;

        UCombatSubsystem Combat = UCombatSubsystem::Get();
        if (Combat == nullptr)
            return;

        // Fire from the upper body. The base LoS gate doesn't run once we override PerformAttack, so keep
        // the counterplay here: a blocked shot whiffs (no glob) instead of curving through cover.
        FVector Muzzle = GetActorLocation() + FVector(0.0, 0.0, 90.0);
        if (!Combat.HasLineOfSightToActor(Muzzle, Hero, this))
            return;

        ASpitterProjectile Glob = Cast<ASpitterProjectile>(SpawnActor(ASpitterProjectile, Muzzle, FRotator()));
        if (Glob != nullptr)
            Glob.Launch(Hero.GetActorLocation(), ProjectileSpeed, AttackDamage, ProjectileSplash, this);
    }
}
