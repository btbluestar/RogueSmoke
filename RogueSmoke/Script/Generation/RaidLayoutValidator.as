// RaidLayoutValidator.as
// Geometric + ballistic validation battery (design spec §9). World-free: counts, distances, bounds,
// blue-noise separation, offset-power-position, plus the ballistic jump-reachability + escape-proof
// checks (Plan 2, derived from the hero reach envelope). Only navmesh ground-connectivity is still
// deferred (until enemies pathfind). Server runs this; failure drives RaidGen::GenerateValidated's reroll.

struct FRaidValidationResult
{
    UPROPERTY()
    bool bOk = false;

    UPROPERTY()
    int PassCount = 0;

    UPROPERTY()
    int Total = 0;

    UPROPERTY()
    FString FirstFail = "";
}

namespace RaidValidate
{
    // Count nodes of a given slot across a site.
    int CountSlot(const FRaidSite& S, ERaidSlotType Slot)
    {
        int N = 0;
        for (FRaidNode Node : S.Nodes)
        {
            if (Node.Slot == Slot) N += 1;
        }
        return N;
    }

    bool InBounds(FVector P, float Limit)
    {
        return Math::Abs(P.X) <= Limit && Math::Abs(P.Y) <= Limit;
    }

    void Check(FRaidValidationResult& R, bool bPass, FString Label)
    {
        R.Total += 1;
        if (bPass)
            R.PassCount += 1;
        else if (R.FirstFail == "")
            R.FirstFail = Label;
    }

    FRaidValidationResult Validate(const FRaidLayout& L, const FRaidGenConfig& Cfg)
    {
        FRaidValidationResult R;
        float Limit = Cfg.HalfExtent - Cfg.BoundaryMargin + 0.5;   // +epsilon for edge-placed anchors

        // 1. Anchors present with correct types.
        Check(R, L.Drop.Type == ERaidSiteType::Drop && L.Extraction.Type == ERaidSiteType::Extraction,
              "anchors");

        // 2. Drop <-> Extraction separation >= frac * footprint diagonal (a journey across the box).
        float Diagonal = 2.0 * Cfg.HalfExtent * Math::Sqrt(2.0);
        float Sep = (L.Drop.Center - L.Extraction.Center).Size();
        Check(R, Sep >= Cfg.DropExtractMinFrac * Diagonal, "drop-extract-separation");

        // 3. At least one main objective site.
        Check(R, L.MainSites.Num() >= 1, "has-main-site");

        bool bCoreOk = true;
        bool bHoldOk = true;
        bool bOffsetOk = true;
        bool bHighGroundOk = true;
        bool bBoundsOk = true;
        bool bCoverCountOk = true;
        bool bCoverSepOk = true;

        for (FRaidSite S : L.MainSites)
        {
            // 4. Exactly one CombatCore + one HoldAnchor.
            if (CountSlot(S, ERaidSlotType::CombatCore) != 1) bCoreOk = false;
            if (CountSlot(S, ERaidSlotType::HoldAnchor) != 1) bHoldOk = false;

            // 5. HoldAnchor is offset from the core (offset power position).
            FVector Core = S.Center;
            FVector Hold = S.Center;
            for (FRaidNode Node : S.Nodes)
            {
                if (Node.Slot == ERaidSlotType::CombatCore) Core = Node.Location;
                if (Node.Slot == ERaidSlotType::HoldAnchor) Hold = Node.Location;
            }
            if ((Hold - Core).Size() < Cfg.HoldAnchorMinOffset - 0.5) bOffsetOk = false;

            // 6. Contestable verticality present.
            if (CountSlot(S, ERaidSlotType::HighGround) < Cfg.MinHighGround) bHighGroundOk = false;

            // 7. All nodes + cover inside the bounds margin (escape-proof keep-in heuristic).
            for (FRaidNode Node : S.Nodes)
            {
                if (!InBounds(Node.Location, Limit)) bBoundsOk = false;
            }
            for (FRaidCover C : S.Cover)
            {
                if (!InBounds(C.Location, Limit)) bBoundsOk = false;
            }

            // 8a. Cover count in range.
            if (S.Cover.Num() < Cfg.CoverMin || S.Cover.Num() > Cfg.CoverMax) bCoverCountOk = false;

            // 8b. Blue-noise: no two covers closer than the min separation.
            for (int i = 0; i < S.Cover.Num(); i++)
            {
                for (int j = i + 1; j < S.Cover.Num(); j++)
                {
                    if ((S.Cover[i].Location - S.Cover[j].Location).Size() < Cfg.CoverMinSeparation - 0.5)
                        bCoverSepOk = false;
                }
            }
        }

        Check(R, bCoreOk, "one-combat-core");
        Check(R, bHoldOk, "one-hold-anchor");
        Check(R, bOffsetOk, "hold-anchor-offset");
        Check(R, bHighGroundOk, "min-high-ground");
        // The Drop/Extraction anchors must also stay inside the bounds margin (not just main sites).
        if (!InBounds(L.Drop.Center, Limit)) bBoundsOk = false;
        if (!InBounds(L.Extraction.Center, Limit)) bBoundsOk = false;
        Check(R, bBoundsOk, "in-bounds");
        Check(R, bCoverCountOk, "cover-count");
        Check(R, bCoverSepOk, "cover-separation");

        // --- Plan 2 ballistic checks (design spec §9 #1, #2; #6 partial) ---
        FReachEnvelope Env = RaidReach::Default();
        float MaxStandableZ = 0.0;
        bool bReachOk = true;
        for (FRaidSite S : L.MainSites)
        {
            FVector CenterFlat = S.Center;
            CenterFlat.Z = 0.0;
            for (FRaidNode Node : S.Nodes)
            {
                if (Node.Slot != ERaidSlotType::HighGround)
                    continue;
                if (Node.Location.Z > MaxStandableZ)
                    MaxStandableZ = Node.Location.Z;
                // Vertically reachable by double-jump from the ground below.
                if (Node.Location.Z > Env.VertCeiling - Cfg.JumpReachMargin)
                    bReachOk = false;
                // Horizontally reachable across the floor (slide-hop + double-jump).
                FVector Flat = Node.Location;
                Flat.Z = 0.0;
                if ((Flat - CenterFlat).Size() > Env.HorizDoubleJump)
                    bReachOk = false;
            }
        }
        Check(R, bReachOk, "jump-reachability");

        // Escape-proof: the boundary wall must out-reach the highest standable point.
        Check(R, Cfg.WallHeight >= MaxStandableZ + Env.VertCeiling + Cfg.EscapeMargin, "escape-proof");

        R.bOk = (R.PassCount == R.Total);
        return R;
    }
}
