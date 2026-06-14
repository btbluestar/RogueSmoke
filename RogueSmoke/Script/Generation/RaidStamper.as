// RaidStamper.as
// Turns an FRaidLayout into collidable greybox geometry via the C++ URaidStampSubsystem (the ISM
// seam). Runs on EVERY machine from the replicated master seed — geometry is re-stamped locally,
// never replicated (ARCHITECTURE 4.1). Greybox = engine cube instances. Returns instances stamped.

namespace RaidArena
{
    // Generate (validated) + stamp the arena for a seed. Idempotent: clears prior stamps first.
    int BuildFromSeed(int Seed)
    {
        URaidStampSubsystem Stamp = URaidStampSubsystem::Get();
        if (Stamp == nullptr)
            return 0;
        Stamp.ClearStamps();

        FRaidGenConfig Cfg;
        FRaidLayout L = RaidGen::GenerateValidated(Seed, Cfg);
        int N = StampLayout(Stamp, L, Cfg);
        Print(f"[RaidArena] stamped {N} boxes for seed {Seed} (valid={L.bValid})", 5.0);
        return N;
    }

    // Build the box list (floor + 4 walls + platforms + cover) and stamp it in one batch.
    int StampLayout(URaidStampSubsystem Stamp, const FRaidLayout& L, const FRaidGenConfig& Cfg)
    {
        TArray<FVector> Centers;
        TArray<FVector> Sizes;
        float Span = 2.0 * Cfg.HalfExtent;

        // Floor — one thin slab over the footprint.
        Centers.Add(FVector(0.0, 0.0, -10.0));
        Sizes.Add(FVector(Span, Span, 20.0));

        // Four boundary walls (escape-proof height).
        float H = Cfg.WallHeight;
        float Ex = Cfg.HalfExtent;
        float Th = 50.0;
        Centers.Add(FVector( Ex, 0.0, H * 0.5)); Sizes.Add(FVector(Th, Span, H));
        Centers.Add(FVector(-Ex, 0.0, H * 0.5)); Sizes.Add(FVector(Th, Span, H));
        Centers.Add(FVector(0.0,  Ex, H * 0.5)); Sizes.Add(FVector(Span, Th, H));
        Centers.Add(FVector(0.0, -Ex, H * 0.5)); Sizes.Add(FVector(Span, Th, H));

        // Platforms (HighGround) + cover monoliths.
        for (FRaidSite S : L.MainSites)
        {
            for (FRaidNode N : S.Nodes)
            {
                if (N.Slot != ERaidSlotType::HighGround)
                    continue;
                Centers.Add(FVector(N.Location.X, N.Location.Y, N.Location.Z * 0.5));
                Sizes.Add(FVector(400.0, 400.0, N.Location.Z));
            }
            for (FRaidCover C : S.Cover)
            {
                Centers.Add(FVector(C.Location.X, C.Location.Y, 60.0));
                Sizes.Add(FVector(C.Radius * 2.0, C.Radius * 2.0, 120.0));
            }
        }

        return Stamp.StampBoxes(Centers, Sizes);
    }

    // Deterministic layout for a seed (pure; cheap). Consumers read Drop/MainSites/Extraction.
    FRaidLayout GetLayout(int Seed)
    {
        FRaidGenConfig Cfg;
        return RaidGen::GenerateValidated(Seed, Cfg);
    }

    // World location of the first node of a slot in a site (falls back to the site center).
    FVector NodeLocation(const FRaidSite& Site, ERaidSlotType Slot)
    {
        for (FRaidNode N : Site.Nodes)
        {
            if (N.Slot == Slot)
                return N.Location;
        }
        return Site.Center;
    }
}
