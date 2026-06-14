// Carapace.as
// Shield elite — the synergy anchor (GDD §5.1): the unit worth taunting + clustering. Tanky and slow,
// with a telegraphed radial slam, so the Vanguard taunt -> Bombardier barrage combo now lands on an
// enemy that actually fights back. Pure tuning over AAttackingElite (the C++ base owns the targeting/
// telegraph loop and the default radial attack); counts toward the "clear the arena" objective.
class ACarapace : AAttackingElite
{
    default MaxHealthOverride = 320.0;
    default MoveSpeed = 150.0;
    default PreferredRange = 150.0;
    default AttackRange = 250.0;
    default AttackDamage = 22.0;
    default AttackRadius = 260.0;       // heavy slam — hits everyone clustered around it
    default AttackInterval = 3.0;
    default TelegraphSeconds = 0.9;     // slow, very readable wind-up
    default BodyScale = FVector(1.7, 1.7, 2.2);
    default BodyColor = FLinearColor(0.20, 0.40, 0.90, 1.0);   // steel-blue shield/tank
}
