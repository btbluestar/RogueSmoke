// RaidWaveDirector.as
// The wave director's brain (D-0020): a PURE function of (team level, wave index, player count,
// tunables) — no world reads, no RNG, no clocks — so the same inputs give the same plan on every
// machine and in the DirectorReport battery. ARaidObjective owns the tunables and consumes plans.

struct FDirectorTunables
{
    UPROPERTY()
    float BaseInterval = 7.0;

    UPROPERTY()
    int BasePerWave = 8;

    UPROPERTY()
    float EscalationPerWave = 0.5;

    UPROPERTY()
    int MaxPerWave = 32;

    // v3 scaling: pressure follows the team's power curve (D-0018 levels).
    UPROPERTY()
    float FodderPerTeamLevel = 0.8;

    UPROPERTY()
    float IntervalReductionPerLevel = 0.35;

    UPROPERTY()
    float MinInterval = 3.5;

    UPROPERTY()
    int EliteInjectStartLevel = 4;

    UPROPERTY()
    int EliteInjectFastLevel = 8;

    UPROPERTY()
    float PlayerCountWaveScale = 0.5;
}

struct FWavePlan
{
    UPROPERTY()
    int FodderCount = 0;

    UPROPERTY()
    float Interval = 7.0;

    // Index into the objective's InjectRoster (rotated deterministically); -1 = no injection.
    UPROPERTY()
    int EliteInjectIndex = -1;
}

namespace RaidDirector
{
    FWavePlan ComputeWavePlan(int TeamLevel, int WaveIndex, int NumPlayers, const FDirectorTunables& T)
    {
        FWavePlan Plan;

        float Size = float(T.BasePerWave)
                   + float(WaveIndex) * T.EscalationPerWave
                   + float(TeamLevel) * T.FodderPerTeamLevel;
        Size *= 1.0 + T.PlayerCountWaveScale * float(Math::Max(NumPlayers - 1, 0));
        Plan.FodderCount = Math::Clamp(int(Size), 1, T.MaxPerWave);

        Plan.Interval = Math::Max(T.MinInterval,
                                  T.BaseInterval - float(TeamLevel) * T.IntervalReductionPerLevel);

        // Spice, not just volume: from the start level every 3rd wave carries an elite, every
        // 2nd from the fast level. Keyed to WaveIndex only — deterministic, no RNG to perturb.
        if (TeamLevel >= T.EliteInjectStartLevel)
        {
            int Cadence = (TeamLevel >= T.EliteInjectFastLevel) ? 2 : 3;
            if (WaveIndex % Cadence == 0)
                Plan.EliteInjectIndex = (WaveIndex / Cadence) % 2;
        }
        return Plan;
    }
}
