// RaidCombatDirector.as
// The combat director (attack tokens). STATELESS namespace — all per-enemy token state lives on the
// elite (AAttackingElite). Server-only; called from RaidObjective on a ~0.25s throttle. Grants a limited
// number of "tokens" per player: only Engaged token-holders run the full attack loop; the rest hold at a
// ring as Background. Promotion = nearest + spread + archetype variety; release on attack/death/lost/timeout.

namespace RaidCombatDirector
{
    // One scheduling pass. TokensPerPlayer active attackers per living player; an Engaged elite is released
    // when it has attacked / died / lost its target / been Engaged longer than TimeoutSeconds; a Background
    // elite is eligible again only after CooldownSeconds and within EngagementRange of its target.
    void Tick(int TokensPerPlayer, float TimeoutSeconds, float CooldownSeconds, float EngagementRange)
    {
        TArray<AAttackingElite> Elites;
        GetAllActorsOfClass(Elites);

        TArray<AHeroCharacter> Heroes;
        GetAllActorsOfClass(Heroes);

        // 1) RELEASE: hand finished tokens back.
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken())
                continue;
            if (E.GetEngageState() != EEngageState::Engaged)
                continue;
            bool bRelease = !E.IsAlive()
                || E.HasAttackedThisEngagement()
                || E.GetCurrentTarget() == nullptr
                || E.GetTimeInEngageState() >= TimeoutSeconds;
            if (bRelease)
                E.SetEngageState(EEngageState::Background);
        }

        // 2) PROMOTE: per living player, fill free token slots with the best eligible Background elites.
        // Note: if an Engaged elite re-targets a different player mid-engagement (AcquireTarget runs every
        // tick), it is NOT released — it's simply re-counted against the new player's budget here, so a
        // player may transiently sit at budget+1 until an existing attacker releases. Self-correcting.
        for (AHeroCharacter H : Heroes)
        {
            if (H == nullptr || H.IsIncapacitated())
                continue;
            APawn HeroPawn = Cast<APawn>(H);
            if (HeroPawn == nullptr)
                continue;

            int Used = CountEngagedTargeting(Elites, HeroPawn);
            int Free = TokensPerPlayer - Used;
            // Guard against an unbounded loop if nothing eligible remains.
            while (Free > 0)
            {
                AAttackingElite Best = PickBest(Elites, HeroPawn, H.GetActorLocation(), CooldownSeconds, EngagementRange);
                if (Best == nullptr)
                    break;
                Best.SetEngageState(EEngageState::Engaged);
                Free -= 1;
            }
        }
    }

    // Engaged token-users currently targeting HeroPawn.
    int CountEngagedTargeting(const TArray<AAttackingElite>& Elites, APawn HeroPawn)
    {
        int N = 0;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() == EEngageState::Engaged && E.GetCurrentTarget() == HeroPawn)
                N += 1;
        }
        return N;
    }

    // Highest-scoring eligible Background elite targeting HeroPawn, or null. Score: nearer is better,
    // minus a penalty per already-Engaged elite of the same archetype (variety) and per one on the same
    // side of the player (spread, ~within 60 degrees).
    AAttackingElite PickBest(const TArray<AAttackingElite>& Elites, APawn HeroPawn, FVector HeroLoc,
                             float CooldownSeconds, float EngagementRange)
    {
        AAttackingElite Best = nullptr;
        float BestScore = -100000.0;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() != EEngageState::Background)
                continue;
            if (E.GetCurrentTarget() != HeroPawn)
                continue;
            if (E.GetTimeInEngageState() < CooldownSeconds)   // still on re-eligibility cooldown
                continue;
            float Dist = (E.GetActorLocation() - HeroLoc).Size();
            if (Dist > EngagementRange)                        // too far to bother promoting yet
                continue;

            float Score = -Dist;                               // nearer better
            Score -= 600.0 * float(SameArchetypeEngaged(Elites, HeroPawn, E.GetClass()));   // variety
            Score -= 400.0 * float(SameSideEngaged(Elites, HeroPawn, HeroLoc, E.GetActorLocation()));  // spread

            if (Score > BestScore)
            {
                BestScore = Score;
                Best = E;
            }
        }
        return Best;
    }

    // Count of Engaged token-users targeting HeroPawn whose class == Cls (variety penalty).
    int SameArchetypeEngaged(const TArray<AAttackingElite>& Elites, APawn HeroPawn, UClass Cls)
    {
        int N = 0;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() == EEngageState::Engaged && E.GetCurrentTarget() == HeroPawn && E.GetClass() == Cls)
                N += 1;
        }
        return N;
    }

    // Count of Engaged token-users targeting HeroPawn within ~60 degrees of CandidateLoc, as seen from the
    // hero (spread penalty so attackers surround instead of stacking one side).
    int SameSideEngaged(const TArray<AAttackingElite>& Elites, APawn HeroPawn, FVector HeroLoc, FVector CandidateLoc)
    {
        FVector CandDir = CandidateLoc - HeroLoc;
        CandDir.Z = 0.0;
        if (CandDir.SizeSquared() < 1.0)
            return 0;
        CandDir = CandDir.GetSafeNormal();

        int N = 0;
        for (AAttackingElite E : Elites)
        {
            if (E == nullptr || !E.GetUsesAttackToken() || !E.IsAlive())
                continue;
            if (E.GetEngageState() != EEngageState::Engaged || E.GetCurrentTarget() != HeroPawn)
                continue;
            FVector D = E.GetActorLocation() - HeroLoc;
            D.Z = 0.0;
            if (D.SizeSquared() < 1.0)
                continue;
            D = D.GetSafeNormal();
            if (CandDir.DotProduct(D) > 0.5)   // cos(60deg) — same side
                N += 1;
        }
        return N;
    }
}
