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
}
