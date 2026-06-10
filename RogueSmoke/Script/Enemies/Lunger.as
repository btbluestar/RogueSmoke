// Lunger.as
// Charger / gap-closer: hangs at mid-range, then telegraphs and lunges at the target to break kiting and
// threaten the backline. Counterplay: dodge the lunge with the slide (D-0015). The lunge is now a smooth
// multi-frame dash (AAttackingElite::StartDash) rather than a teleport; the base applies the bite on
// contact during the slide (DashContactRange).
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
    default BodyColor = FLinearColor(0.85, 0.20, 0.85, 1.0);   // magenta fast charger
    default DashContactRange = 220.0;   // bite window during the leap

    // DashSpeed * DashDuration ~= leap distance (~650u, matching the old pop) but spread over frames so
    // the slide is dodgeable with the slide move instead of an instant blink.
    UPROPERTY(EditDefaultsOnly, Category = "Enemy")
    float DashSpeed = 1700.0;

    UPROPERTY(EditDefaultsOnly, Category = "Enemy")
    float DashDuration = 0.38;

    UFUNCTION(BlueprintOverride)
    void PerformAttack()
    {
        APawn Hero = GetCurrentTarget();
        if (Hero == nullptr)
            return;

        FVector ToTarget = Hero.GetActorLocation() - GetActorLocation();
        StartDash(ToTarget, DashSpeed, DashDuration);
    }
}
