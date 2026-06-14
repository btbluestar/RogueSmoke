// RaidPartition.as
// Deterministic placement of objective-zone anchors inside the footprint (design spec §5/§8 step 2:
// reserve-then-populate + Poisson min-separation). PURE: draws only from the passed-in seeded stream,
// so the master seed reproduces the same anchors on every machine. No world reads, no import.

namespace RaidPartition
{
    // Place up to 'count' zone anchors inside +/-interior on each axis, each at least 'minSep' apart
    // and at least 'dropClearance' from every reserved point (drop/extraction). Rejection-sampled
    // (Bridson-style blue noise, same pattern as RaidGen::ScatterCover). Deterministic via Rng.
    // Returns the anchors actually placed (may be < count if the box is too tight — caller validates).
    TArray<FVector> PlaceZoneAnchors(FRandomStream& Rng, float interior, int count, float minSep,
                                     const TArray<FVector>& reserved, float dropClearance)
    {
        TArray<FVector> Out;
        int attempts = 0;
        while (Out.Num() < count && attempts < 400)
        {
            attempts += 1;
            FVector P = FVector(Rng.RandRange(-interior, interior),
                                Rng.RandRange(-interior, interior), 0.0);

            bool bOk = true;
            // Keep clear of reserved anchors (drop / extraction).
            for (int r = 0; r < reserved.Num(); r++)
            {
                FVector Rsv = reserved[r];
                if (FVector(P.X - Rsv.X, P.Y - Rsv.Y, 0.0).Size() < dropClearance) { bOk = false; break; }
            }
            // Min-separation from already-placed anchors.
            if (bOk)
            {
                for (int k = 0; k < Out.Num(); k++)
                {
                    if (FVector(P.X - Out[k].X, P.Y - Out[k].Y, 0.0).Size() < minSep) { bOk = false; break; }
                }
            }
            if (bOk)
                Out.Add(P);
        }
        return Out;
    }
}
