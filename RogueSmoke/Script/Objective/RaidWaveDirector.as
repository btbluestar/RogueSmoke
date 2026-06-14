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

    // --- Plan 3 time-spine (only active when ElapsedSeconds > 0; default keeps legacy behavior) ---
    UPROPERTY()
    float FodderPerSecond = 0.03;        // rising tide: extra fodder per elapsed second

    UPROPERTY()
    float EliteSpike1Time = 180.0;       // first elite-spike shoulder (s)

    UPROPERTY()
    float EliteSpike2Time = 390.0;       // second shoulder (s)

    UPROPERTY()
    float MiniBossTime = 480.0;          // climactic mini-boss (s)

    UPROPERTY()
    float SpikeWindow = 6.0;             // a spike fires for waves within +/- this of its time

    UPROPERTY()
    float RelaxLullMultiplier = 1.8;     // wave interval stretches this much during a post-spike lull
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

    // Time-spine spikes (Plan 3). 0 = normal wave; 1/2 = elite-spike shoulders; bSpawnMiniBoss at the climax.
    UPROPERTY()
    int SpikeTier = 0;

    UPROPERTY()
    bool bSpawnMiniBoss = false;
}

namespace RaidDirector
{
    FWavePlan ComputeWavePlan(int TeamLevel, int WaveIndex, int NumPlayers, const FDirectorTunables& T,
                              float ElapsedSeconds = 0.0)
    {
        FWavePlan Plan;

        float Size = float(T.BasePerWave)
                   + float(WaveIndex) * T.EscalationPerWave
                   + float(TeamLevel) * T.FodderPerTeamLevel
                   + ElapsedSeconds * T.FodderPerSecond;          // Plan 3: time-based rising tide
        Size *= 1.0 + T.PlayerCountWaveScale * float(Math::Max(NumPlayers - 1, 0));
        Plan.FodderCount = Math::Clamp(int(Size), 1, T.MaxPerWave);

        Plan.Interval = Math::Max(T.MinInterval,
                                  T.BaseInterval - float(TeamLevel) * T.IntervalReductionPerLevel);

        // Legacy team-level elite cadence (unchanged when ElapsedSeconds == 0).
        if (TeamLevel >= T.EliteInjectStartLevel)
        {
            int Cadence = (TeamLevel >= T.EliteInjectFastLevel) ? 2 : 3;
            if (WaveIndex % Cadence == 0)
                Plan.EliteInjectIndex = (WaveIndex / Cadence) % 2;
        }

        // Plan 3 designer spikes keyed off elapsed time. A spike forces an elite injection and a
        // post-spike relax lull (stretched interval). Deterministic — no clock read, no RNG.
        if (ElapsedSeconds > 0.0)
        {
            if (Math::Abs(ElapsedSeconds - T.EliteSpike1Time) <= T.SpikeWindow)
                Plan.SpikeTier = 1;
            else if (Math::Abs(ElapsedSeconds - T.EliteSpike2Time) <= T.SpikeWindow)
                Plan.SpikeTier = 2;

            if (Plan.SpikeTier > 0)
            {
                Plan.EliteInjectIndex = Math::Max(Plan.EliteInjectIndex, 0);   // guarantee an elite at a shoulder
                Plan.Interval *= T.RelaxLullMultiplier;                        // breathe after the spike
            }

            if (Math::Abs(ElapsedSeconds - T.MiniBossTime) <= T.SpikeWindow)
            {
                Plan.bSpawnMiniBoss = true;
                Plan.Interval *= T.RelaxLullMultiplier;
            }
        }

        return Plan;
    }
}
