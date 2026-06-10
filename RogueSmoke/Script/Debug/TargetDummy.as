// TargetDummy.as
// Passive firing-range dummy (DL_Upgrades). Built on AAttackingElite so it's self-contained —
// visible cube body, Visibility-blocking collision (hitscan-able), replicated health, hit-flash +
// death-burst cues — but tuned to never move, attack, or telegraph. AClusterableElite is NOT
// suitable here: its Mesh component has no asset at the class level (assigned per-instance in
// editor levels), so seam traces pass straight through a code-spawned one.
class ATargetDummy : AAttackingElite
{
    default MaxHealthOverride = 200.0;
    default MoveSpeed = 0.0;            // never repositions
    default PreferredRange = 1000000.0; // "close enough" everywhere -> never approaches
    default AttackRange = 0.0;          // never in range -> never telegraphs or attacks
    default AttackDamage = 0.0;
    default BodyScale = FVector(1.0, 1.0, 2.0);
    default BodyColor = FLinearColor(0.2, 0.75, 0.85, 1.0);   // training-range teal
}
