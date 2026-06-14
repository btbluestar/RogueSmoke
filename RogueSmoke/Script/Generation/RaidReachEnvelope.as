// RaidReachEnvelope.as
// The hero's traversal reach envelope — the SINGLE SOURCE OF TRUTH the procedural validator shares
// with the movement kit. Pure math from the locomotion tunables (D-0021). Used by jump-reachability
// ("can the player reach this platform?") and escape-proof ("is this wall too low to clear?").
//
// IMPORTANT: Default() MIRRORS URogueLocomotionComponent (Script/Player/LocomotionComponent.as).
// If those tunables change, update Default(). Values are the pre-upgrade baseline (validation runs at
// generation time, before any GAS MoveSpeed upgrade applies).

struct FReachEnvelope
{
    UPROPERTY()
    float VertCeiling = 0.0;       // max height reachable by jump + double-jump from ground (uu)

    UPROPERTY()
    float HorizDoubleJump = 0.0;   // max horizontal gap, flat, slide-hop carry + double-jump (uu)

    UPROPERTY()
    float HorizSingleJump = 0.0;   // max horizontal gap, flat, single jump (uu)
}

namespace RaidReach
{
    // Pure: compute the envelope from raw tunables (testable without a world).
    FReachEnvelope Compute(float BaseWalkSpeed, float SprintMult, float SlideCapMult,
                           float JumpZ, float DoubleJumpZ, float GravityScale)
    {
        float g = 980.0 * GravityScale;                              // UE world default GravityZ * scale
        float LaunchHSpd = BaseWalkSpeed * SprintMult * SlideCapMult; // slide-hop air-carry ceiling

        FReachEnvelope E;
        E.VertCeiling = (JumpZ * JumpZ) / (2.0 * g) + (DoubleJumpZ * DoubleJumpZ) / (2.0 * g);
        E.HorizDoubleJump = LaunchHSpd * 2.0 * (JumpZ + DoubleJumpZ) / g;
        E.HorizSingleJump = LaunchHSpd * 2.0 * JumpZ / g;
        return E;
    }

    // Default envelope mirroring URogueLocomotionComponent defaults (D-0021).
    FReachEnvelope Default()
    {
        return Compute(600.0, 1.6, 1.5, 750.0, 700.0, 1.8);
    }
}
