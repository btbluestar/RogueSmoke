// Lunger.as
// Charger / gap-closer: hangs at mid-range, then telegraphs and lunges at the target to break kiting and
// threaten the backline. Counterplay: dodge the lunge with the slide (D-0015). The lunge is a forward pop
// for the MVP; a smooth dash arc over several frames is a feel upgrade (see SUPERPOWERS_HANDOFF).
class ALunger : AAttackingElite
{
    default MaxHealthOverride = 90.0;
    default MoveSpeed = 240.0;
    default PreferredRange = 500.0;     // sit at mid-range...
    default AttackRange = 650.0;        // ...then lunge once the target is within this
    default AttackDamage = 20.0;
    default AttackRadius = 0.0;
    default AttackInterval = 3.5;
    default TelegraphSeconds = 0.8;
    default BodyScale = FVector(0.8, 0.8, 1.7);

    UPROPERTY(EditDefaultsOnly, Category = "Enemy")
    float LungeDistance = 600.0;

    UPROPERTY(EditDefaultsOnly, Category = "Enemy")
    float LungeHitRange = 220.0;

    UFUNCTION(BlueprintOverride)
    void PerformAttack()
    {
        APawn Hero = GetCurrentTarget();
        if (Hero == nullptr)
            return;

        FVector Mine = GetActorLocation();
        FVector ToTarget = Hero.GetActorLocation() - Mine;
        ToTarget.Z = 0.0;
        FVector Dir = ToTarget.GetSafeNormal();
        if (Dir.IsNearlyZero())
            return;

        float Step = Math::Min(ToTarget.Size(), LungeDistance);
        SetActorLocation(Mine + Dir * Step);
        SetActorRotation(Dir.Rotation());

        // Bite if the lunge closed to contact.
        if ((Hero.GetActorLocation() - GetActorLocation()).Size() <= LungeHitRange)
        {
            UCombatSubsystem Combat = UCombatSubsystem::Get();
            if (Combat != nullptr)
                Combat.ApplyDamageToPlayer(Hero, AttackDamage, this);
        }
    }
}
