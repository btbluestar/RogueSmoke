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

    // Blockout color-coding (grayboxing convention: one hue per gameplay function, unlit so it reads
    // without lighting). Palette aligned to red=danger / green=go / blue=player / yellow=exit and
    // floor-quiet/objects-shout; see docs research brief. One StampBoxesColored batch per type.
    const FLinearColor kFloorColor   = FLinearColor(0.604, 0.627, 0.651, 1.0);  // neutral gray — quiet base
    const FLinearColor kWallColor    = FLinearColor(0.200, 0.216, 0.239, 1.0);  // dark — out-of-bounds blocker
    const FLinearColor kCoverColor   = FLinearColor(0.153, 0.761, 0.761, 1.0);  // cyan — tactical cover
    const FLinearColor kPlatformColor= FLinearColor(0.949, 0.639, 0.235, 1.0);  // amber — high ground / verticality
    const FLinearColor kMawColor     = FLinearColor(0.878, 0.141, 0.369, 1.0);  // red-magenta — swarm source (danger)
    const FLinearColor kHoldColor    = FLinearColor(0.247, 0.749, 0.314, 1.0);  // green — hold / objective anchor
    const FLinearColor kDropColor    = FLinearColor(0.176, 0.498, 0.941, 1.0);  // blue — player drop-in
    const FLinearColor kExtractColor = FLinearColor(0.961, 0.851, 0.039, 1.0);  // yellow — extraction beacon

    // Build the color-coded box batches (floor / walls / platforms / cover + landmark markers) and
    // stamp each as its own tinted ISM. Returns total instances stamped.
    int StampLayout(URaidStampSubsystem Stamp, const FRaidLayout& L, const FRaidGenConfig& Cfg)
    {
        int Total = 0;
        float Span = 2.0 * Cfg.HalfExtent;
        float H = Cfg.WallHeight;
        float Ex = Cfg.HalfExtent;
        float Th = 50.0;

        // Floor — one thin slab over the footprint.
        {
            TArray<FVector> C; TArray<FVector> S;
            C.Add(FVector(0.0, 0.0, -10.0)); S.Add(FVector(Span, Span, 20.0));
            Total += Stamp.StampBoxesColored(C, S, kFloorColor);
        }

        // Four boundary walls (escape-proof height).
        {
            TArray<FVector> C; TArray<FVector> S;
            C.Add(FVector( Ex, 0.0, H * 0.5)); S.Add(FVector(Th, Span, H));
            C.Add(FVector(-Ex, 0.0, H * 0.5)); S.Add(FVector(Th, Span, H));
            C.Add(FVector(0.0,  Ex, H * 0.5)); S.Add(FVector(Span, Th, H));
            C.Add(FVector(0.0, -Ex, H * 0.5)); S.Add(FVector(Span, Th, H));
            Total += Stamp.StampBoxesColored(C, S, kWallColor);
        }

        // Platforms (HighGround, amber) and cover monoliths (cyan), collected across all sites.
        TArray<FVector> PlatC; TArray<FVector> PlatS;
        TArray<FVector> CovC;  TArray<FVector> CovS;
        for (FRaidSite S : L.MainSites)
        {
            for (FRaidNode N : S.Nodes)
            {
                if (N.Slot != ERaidSlotType::HighGround)
                    continue;
                PlatC.Add(FVector(N.Location.X, N.Location.Y, N.Location.Z * 0.5));
                PlatS.Add(FVector(400.0, 400.0, N.Location.Z));
            }
            for (FRaidCover C : S.Cover)
            {
                CovC.Add(FVector(C.Location.X, C.Location.Y, 60.0));
                CovS.Add(FVector(C.Radius * 2.0, C.Radius * 2.0, 120.0));
            }
        }
        Total += Stamp.StampBoxesColored(PlatC, PlatS, kPlatformColor);
        Total += Stamp.StampBoxesColored(CovC,  CovS,  kCoverColor);

        // Landmark marker pillars so the key nodes are legible at a glance (these aren't blockers;
        // they label where the Maw / hold / drop / extraction sit on the generated layout).
        Total += StampMarker(Stamp, SlotLocation(L, ERaidSlotType::CombatCore), kMawColor,    600.0);
        Total += StampMarker(Stamp, SlotLocation(L, ERaidSlotType::HoldAnchor), kHoldColor,    500.0);
        Total += StampMarker(Stamp, L.Drop.Center,                              kDropColor,    400.0);
        Total += StampMarker(Stamp, L.Extraction.Center,                        kExtractColor, 700.0);
        return Total;
    }

    // One tinted vertical pillar (200×200×Height) centered on a landmark location.
    int StampMarker(URaidStampSubsystem Stamp, FVector Loc, FLinearColor Color, float Height)
    {
        TArray<FVector> C; TArray<FVector> S;
        C.Add(FVector(Loc.X, Loc.Y, Height * 0.5));
        S.Add(FVector(200.0, 200.0, Height));
        return Stamp.StampBoxesColored(C, S, Color);
    }

    // First node of a slot anywhere in the main sites (falls back to origin if absent).
    FVector SlotLocation(const FRaidLayout& L, ERaidSlotType Slot)
    {
        for (FRaidSite S : L.MainSites)
        {
            for (FRaidNode N : S.Nodes)
            {
                if (N.Slot == Slot)
                    return N.Location;
            }
        }
        return FVector::ZeroVector;
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
