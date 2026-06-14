// RaidLevelGenerator.as
// Deterministic, seeded layout generation (design spec §5, §8). PURE: no world reads. Draws from a
// FRandomStream salted off the master seed so it never perturbs the run's master stream
// (CODING_STANDARDS §5), exactly like RaidObjective's fodder placement. MVP scope: one Skatepark
// objective site + a shared drop + a separate extraction. Archetype pool + DataAsset-driven modules
// come in later plans; the Skatepark template is hardcoded here for the foundation slice.

struct FRaidGenConfig
{
    UPROPERTY()
    float HalfExtent = 2500.0;           // ~50x50m playable footprint (4-player baseline)

    UPROPERTY()
    float BoundaryMargin = 250.0;        // keep nodes this far inside the bounds (escape-proof heuristic)

    UPROPERTY()
    float DropExtractMinFrac = 0.6;      // dist(drop,extract) >= frac * footprint diagonal

    UPROPERTY()
    int HighGroundCount = 3;             // contestable verticality platforms placed

    UPROPERTY()
    int MinHighGround = 2;               // validator floor

    UPROPERTY()
    float SiteRadius = 1600.0;           // Skatepark footprint radius

    UPROPERTY()
    float HoldAnchorMinOffset = 600.0;   // HoldAnchor offset from the core (offset power position)

    UPROPERTY()
    int CoverMin = 6;

    UPROPERTY()
    int CoverMax = 14;

    UPROPERTY()
    float CoverMinSeparation = 350.0;    // blue-noise min spacing (also the core keep-clear radius)

    UPROPERTY()
    float CoverRadius = 120.0;
}

namespace RaidGen
{
    const float PI = 3.14159265;
    const int kArenaSalt = 104729;       // prime; salts the generation stream off the master seed

    // Roll a full layout from a seed. Deterministic: same (Seed, Cfg) -> identical FRaidLayout.
    FRaidLayout Generate(int Seed, const FRaidGenConfig& Cfg)
    {
        FRaidLayout L;
        L.Seed = Seed;
        L.HalfExtent = Cfg.HalfExtent;

        FRandomStream Rng(Seed + kArenaSalt);

        float In = Cfg.HalfExtent - Cfg.BoundaryMargin;

        // Drop on one of four edge midpoints; Extraction on the opposite edge (max distance).
        int DropEdge = Rng.RandRange(0, 3);
        int ExtEdge = (DropEdge + 2) % 4;
        L.Drop = MakeAnchorSite(ERaidSiteType::Drop, ERaidSlotType::Entrance, EdgeMidpoint(DropEdge, In));
        L.Extraction = MakeAnchorSite(ERaidSiteType::Extraction, ERaidSlotType::Exit, EdgeMidpoint(ExtEdge, In));

        // One Skatepark main site near center, bounded jitter.
        FVector SiteCenter = FVector(Rng.RandRange(-300.0, 300.0), Rng.RandRange(-300.0, 300.0), 0.0);
        L.MainSites.Add(BuildSkatepark(SiteCenter, Cfg, Rng));

        return L;
    }

    // --- helpers ---

    FVector EdgeMidpoint(int Edge, float In)
    {
        if (Edge == 0) return FVector( In, 0.0, 0.0);
        if (Edge == 1) return FVector(0.0,  In, 0.0);
        if (Edge == 2) return FVector(-In, 0.0, 0.0);
        return FVector(0.0, -In, 0.0);
    }

    FRaidNode MakeNode(ERaidSlotType Slot, FVector Loc, float Cap)
    {
        FRaidNode N;
        N.Slot = Slot;
        N.Location = Loc;
        N.IntensityCap = Cap;
        return N;
    }

    FRaidSite MakeAnchorSite(ERaidSiteType Type, ERaidSlotType Slot, FVector Loc)
    {
        FRaidSite S;
        S.Type = Type;
        S.Objective = ERaidObjectiveType::None;
        S.Center = Loc;
        S.Nodes.Add(MakeNode(Slot, Loc, 0.2));
        return S;
    }

    FRaidSite BuildSkatepark(FVector Center, const FRaidGenConfig& Cfg, FRandomStream& Rng)
    {
        FRaidSite S;
        S.Type = ERaidSiteType::MainObjective;
        S.Objective = ERaidObjectiveType::HoldAndChannel;   // MVP objective
        S.Archetype = ERaidArchetype::Skatepark;
        S.Center = Center;

        // CombatCore (the Maw) at the site center.
        S.Nodes.Add(MakeNode(ERaidSlotType::CombatCore, Center, 0.8));

        // HoldAnchor offset from the core (offset power position; where the mini-boss falls).
        float HoldAngle = Rng.RandRange(0.0, 2.0 * PI);
        float HoldDist = Cfg.HoldAnchorMinOffset + Rng.RandRange(0.0, 300.0);
        FVector HoldPos = Center + FVector(Math::Cos(HoldAngle), Math::Sin(HoldAngle), 0.0) * HoldDist;
        S.Nodes.Add(MakeNode(ERaidSlotType::HoldAnchor, HoldPos, 1.0));

        // HighGround platforms ringing the core (1-storey tiers).
        for (int i = 0; i < Cfg.HighGroundCount; i++)
        {
            float A = (2.0 * PI * float(i)) / float(Cfg.HighGroundCount) + Rng.RandRange(-0.3, 0.3);
            float R = Cfg.SiteRadius * Rng.RandRange(0.5, 0.85);
            FVector P = Center + FVector(Math::Cos(A), Math::Sin(A), 0.0) * R;
            P.Z = Rng.RandRange(300.0, 600.0);
            S.Nodes.Add(MakeNode(ERaidSlotType::HighGround, P, 0.5));
        }

        // Entrance / Exit / Flank loop nodes (the traversal-loop skeleton).
        S.Nodes.Add(MakeNode(ERaidSlotType::Entrance, Center + FVector(-Cfg.SiteRadius, 0.0, 0.0), 0.2));
        S.Nodes.Add(MakeNode(ERaidSlotType::Exit,     Center + FVector( Cfg.SiteRadius, 0.0, 0.0), 0.2));
        S.Nodes.Add(MakeNode(ERaidSlotType::FlankLoop, Center + FVector(0.0,  Cfg.SiteRadius * 0.8, 0.0), 0.5));
        S.Nodes.Add(MakeNode(ERaidSlotType::FlankLoop, Center + FVector(0.0, -Cfg.SiteRadius * 0.8, 0.0), 0.5));

        // Cover scatter (blue-noise), keeping the central killbox clear.
        S.Cover = ScatterCover(Center, Cfg, Rng);
        return S;
    }

    TArray<FRaidCover> ScatterCover(FVector Center, const FRaidGenConfig& Cfg, FRandomStream& Rng)
    {
        TArray<FRaidCover> Out;
        int Target = Rng.RandRange(Cfg.CoverMin, Cfg.CoverMax);
        int Attempts = 0;
        while (Out.Num() < Target && Attempts < 200)
        {
            Attempts += 1;
            float A = Rng.RandRange(0.0, 2.0 * PI);
            float R = Cfg.SiteRadius * Math::Sqrt(Rng.RandRange(0.1, 1.0));
            FVector P = Center + FVector(Math::Cos(A), Math::Sin(A), 0.0) * R;

            // Keep the core readable: no cover inside the central keep-clear radius.
            if ((P - Center).Size() < Cfg.CoverMinSeparation)
                continue;

            bool bOk = true;
            for (FRaidCover C : Out)
            {
                if ((C.Location - P).Size() < Cfg.CoverMinSeparation) { bOk = false; break; }
            }
            if (!bOk)
                continue;

            FRaidCover Cv;
            Cv.Location = P;
            Cv.Radius = Cfg.CoverRadius;
            Out.Add(Cv);
        }
        return Out;
    }

    // Deterministic structural equality (epsilon on positions) — used by the determinism test.
    bool VecEq(FVector A, FVector B)
    {
        return (A - B).Size() < 0.01;
    }

    bool SitesEqual(const FRaidSite& A, const FRaidSite& B)
    {
        if (A.Type != B.Type || A.Objective != B.Objective || A.Archetype != B.Archetype)
            return false;
        if (!VecEq(A.Center, B.Center))
            return false;
        if (A.Nodes.Num() != B.Nodes.Num() || A.Cover.Num() != B.Cover.Num())
            return false;
        for (int i = 0; i < A.Nodes.Num(); i++)
        {
            if (A.Nodes[i].Slot != B.Nodes[i].Slot) return false;
            if (!VecEq(A.Nodes[i].Location, B.Nodes[i].Location)) return false;
        }
        for (int i = 0; i < A.Cover.Num(); i++)
        {
            if (!VecEq(A.Cover[i].Location, B.Cover[i].Location)) return false;
        }
        return true;
    }

    bool LayoutsEqual(const FRaidLayout& A, const FRaidLayout& B)
    {
        if (A.Seed != B.Seed) return false;
        if (Math::Abs(A.HalfExtent - B.HalfExtent) > 0.01) return false;
        if (A.MainSites.Num() != B.MainSites.Num()) return false;
        if (!SitesEqual(A.Drop, B.Drop)) return false;
        if (!SitesEqual(A.Extraction, B.Extraction)) return false;
        for (int i = 0; i < A.MainSites.Num(); i++)
        {
            if (!SitesEqual(A.MainSites[i], B.MainSites[i])) return false;
        }
        return true;
    }
}
