// Spitter.as
// Ranged special: holds at distance and lobs an acid glob (a telegraphed single-target hit) to punish
// turtling and force repositioning. Hangs back via a large PreferredRange; the wind-up is the dodge
// window. (An arcing projectile + a line-of-sight check are a feel upgrade — see SUPERPOWERS_HANDOFF.)
class ASpitter : AAttackingElite
{
    default MaxHealthOverride = 70.0;
    default MoveSpeed = 190.0;
    default PreferredRange = 750.0;     // kite: stop well short and shoot
    default AttackRange = 1000.0;
    default AttackDamage = 12.0;
    default AttackRadius = 0.0;         // single-target ranged
    default AttackInterval = 2.2;
    default TelegraphSeconds = 0.6;
    default BodyScale = FVector(0.9, 0.9, 1.5);
}
