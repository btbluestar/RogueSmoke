// RaidLayoutValidator.as
// Pure-geometric validation battery (design spec §9). The subset that needs NO world — counts,
// distances, bounds, blue-noise separation, offset-power-position. Navmesh connectivity + jump-
// reachability + escape-proof trajectory checks need stamped geometry and live in Plan 2.
// Server runs this; failure drives the deterministic reroll in RaidGen::GenerateValidated.

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
        Check(R, bBoundsOk, "in-bounds");
        Check(R, bCoverCountOk, "cover-count");
        Check(R, bCoverSepOk, "cover-separation");

        R.bOk = (R.PassCount == R.Total);
        return R;
    }
}
