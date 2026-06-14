// RaidTerrain.as
// Deterministic terrain heightfield (design spec §8 step 1). Heights come from an integer hash of
// (seed, tileX, tileY), box-blur smoothed and quantized to integer levels — NO float interpolation
// on the height result, so terrain is bit-identical on host + clients (spec §2). Pure: no world reads.

namespace RaidTerrain
{
    // 32-bit integer avalanche hash of three ints. Wrap on overflow is deterministic (two's-complement),
    // exactly like RaidGen's salt arithmetic. Returns a well-mixed int.
    int Hash3(int seed, int x, int y)
    {
        int h = seed * 374761393 + x * 668265263 + y * 2246822519;
        h = (h ^ (h >> 13)) * 1274126177;
        h = h ^ (h >> 16);
        return h;
    }

    // A lattice level in [0, levels) for grid point (gx,gy). Deterministic; integer-only.
    int LatticeLevel(int seed, int gx, int gy, int levels)
    {
        int h = Hash3(seed, gx, gy);
        int m = h & 0x7FFFFFFF;   // mask sign bit -> non-negative
        return m % levels;
    }

    // Raw two-octave level for a tile: a low-frequency "rolling" component (lattice at 1/4 res) plus
    // a fine detail component, summed and kept in [0, levels). Integer-only.
    int RawLevel(int seed, int i, int j, int levels)
    {
        int coarse = LatticeLevel(seed, i / 4, j / 4, levels);          // big shapes
        int fine   = LatticeLevel(seed ^ 0x5A17, i, j, levels);          // per-tile detail
        // Weight coarse heavier (smoother terrain): (3*coarse + fine) / 4.
        int v = (coarse * 3 + fine) / 4;
        if (v < 0) v = 0;
        if (v >= levels) v = levels - 1;
        return v;
    }

    // One integer box-blur pass over a GridDim x GridDim level array (3x3 average, clamped edges).
    // Smooths single-tile spikes into walkable terrain. Pure integer -> deterministic.
    TArray<int> BlurOnce(const TArray<int>& In, int dim)
    {
        TArray<int> Out;
        for (int idx = 0; idx < In.Num(); idx++)
            Out.Add(0);
        for (int j = 0; j < dim; j++)
        {
            for (int i = 0; i < dim; i++)
            {
                int sum = 0;
                int cnt = 0;
                for (int dj = -1; dj <= 1; dj++)
                {
                    for (int di = -1; di <= 1; di++)
                    {
                        int ni = i + di;
                        int nj = j + dj;
                        if (ni < 0 || nj < 0 || ni >= dim || nj >= dim)
                            continue;
                        sum += In[nj * dim + ni];
                        cnt += 1;
                    }
                }
                Out[j * dim + i] = sum / cnt;   // integer average
            }
        }
        return Out;
    }
}
