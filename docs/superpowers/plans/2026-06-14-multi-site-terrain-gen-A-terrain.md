# Multi-Site Terrain Gen — Plan A: Generated Terrain Floor

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the single flat floor slab with a deterministic, seeded **generated terrain floor** (a quantized height-tile grid) that the existing single-site arena sits on — flattened/slope-clamped under the play area, wilder at the rim — proven by `run_code_test` and the headless `GenSmoke` regression.

**Architecture:** Pure AngelScript in `Script/Generation/`. A new `RaidTerrain` namespace builds an `FRaidTerrain` heightfield from the master seed using **integer hash lattice noise with integer box-blur smoothing and quantized heights** — no float interpolation on the height result, so the discrete tile heights are bit-identical on every machine (the determinism-safe pattern from the design spec §2/§8). `RaidStamper` stamps the tiles via the existing C++ ISM seam (`StampBoxesColored`, brown tint) instead of the flat slab. A new `slope-walkable` validation check folds into the existing `RaidValidate::Validate`. **No new C++.** This is **Plan A of 3** (terrain); multi-site partition is Plan B, the runtime objective/director is Plan C.

**Tech Stack:** UnrealEngine-Angelscript (Hazelight UE5.7 fork); `FRandomStream` only for seed entropy (terrain heights are integer-hash, not float-noise); verification via `as-helper run_code_test` (compile + run) and `Tools/SmokeTest.ps1` (headless `-ExecCmds=GenSmoke`, asserts `[GenSmoke] RESULT n/n`).

---

## Design notes the engineer must know

- **Determinism via quantization, not floats.** Terrain heights are computed by an **integer hash** of `(seed, tileX, tileY)` mapped to an integer "level", **box-blurred with integer averaging**, then multiplied by a fixed step. The height result never passes through float interpolation, so two's-complement integer math (which is bit-identical everywhere — the existing generator already relies on this, see `RaidGen` salt-wrap comment) guarantees identical terrain on host + clients. Do **not** introduce `Math::Cos`/bilinear-lerp into the *height* result.
- **AngelScript int is 32-bit;** integer multiply/add wrap on overflow, and that wrap is deterministic — intended, exactly like `RaidGen::kArenaSalt`. Use `>>` and `&` for hashing.
- **No `import` needed** — script symbols are globally visible (e.g. `RaidGen`, `RaidValidate`, `FRaidLayout`, `RaidReach` are usable from any `.as`). Do not add import lines.
- **Existing types you build on** (already in the repo):
  - `FRaidLayout` (`RaidLayout.as`): has `Seed`, `bValid`, `HalfExtent`, `Drop`, `MainSites`, `Extraction`.
  - `FRaidGenConfig` (`RaidLevelGenerator.as`): the tuning struct; `HalfExtent = 2500`, `BoundaryMargin = 250`.
  - `RaidGen::Generate(int Seed, const FRaidGenConfig& Cfg) -> FRaidLayout` and `GenerateValidated(...)`.
  - `RaidValidate::Validate(const FRaidLayout& L, const FRaidGenConfig& Cfg) -> FRaidValidationResult` (counts pass/total; `Check(R, bPass, "label")`).
  - `RaidStamper.as` `RaidArena::StampLayout(...)`: currently stamps a floor slab batch, walls, platforms, cover, markers via `Stamp.StampBoxesColored(Centers, Sizes, FLinearColor)`.
- **The GenSmoke exec** lives in `Script/Player/RaidPlayerController.as` as `UFUNCTION(Exec) void GenSmoke()`. It builds layouts and `Print`s `[GenSmoke] RESULT <pass>/<total>`. `SmokeTest.ps1` greps that line. **Every task that adds an assertion bumps the RESULT denominator; the SmokeTest `Expect` string must be updated in lockstep.** The current expected is `[GenSmoke] RESULT 6/6`.
- **AngelScript idioms here:** `struct` + `UPROPERTY()` members with defaults; `namespace`; `TArray<int> A; A.Add(x); A[i];` (no `reserve` needed); integer math; `Math::Abs`, `Math::Max`, `Math::Min`, `Math::Clamp`, `Math::RoundToInt(float)`; `f"text {x}"`; `Print(msg, seconds)`.
- **Verification reality:** `run_code_test` is the fast inner loop (compiles all `.as`, runs script tests, returns exit 0 on success). `SmokeTest.ps1` boots `/Game/Levels/RaidArena` headless with `-ExecCmds=GenSmoke` and greps the RESULT line. No editor/PIE needed.
- **Branch:** `multi-site-terrain-gen` (already checked out). Commit after every task. Never commit broken `.as` compilation.

## File structure

- **Modify** `RogueSmoke/Script/Generation/RaidLayout.as` — add `FRaidTerrain` struct; add a `Terrain` field to `FRaidLayout`.
- **Create** `RogueSmoke/Script/Generation/RaidTerrain.as` — the `RaidTerrain` namespace: integer hash, lattice level, smoothed/quantized tile height, `Generate`, `WorldHeightAt`, `FlattenDisc`, `MaxSlopeInDisc`.
- **Modify** `RogueSmoke/Script/Generation/RaidLevelGenerator.as` — terrain config fields on `FRaidGenConfig`; call `RaidTerrain::Generate` + `FlattenDisc` inside `Generate`; set ground-node Z to terrain height; build terrain in `BuildSafeFallback` too.
- **Modify** `RogueSmoke/Script/Generation/RaidLayoutValidator.as` — add the `slope-walkable` check.
- **Modify** `RogueSmoke/Script/Generation/RaidStamper.as` — replace the floor-slab batch with a `StampTerrain` tile pass; raise ground elements onto terrain height.
- **Modify** `RogueSmoke/Script/Player/RaidPlayerController.as` — extend `GenSmoke` with terrain-present, determinism, and slope assertions.
- **Modify** `Tools/SmokeTest.ps1` — bump the `GenSmoke` expected RESULT count.

---

### Task 1: `FRaidTerrain` data model

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidLayout.as` (append struct before `FRaidLayout`; add field inside `FRaidLayout`)

- [ ] **Step 1: Add the `FRaidTerrain` struct** (insert immediately above `struct FRaidLayout`)

```angelscript
// A quantized height-tile grid (design spec §5/§8). PURE DATA. Heights are integer "levels" *
// StepUU, so two identical seeds yield byte-identical terrain on every machine (no float interp).
// Grid is GridDim x GridDim tiles, each TileSize uu, centered on the footprint origin.
struct FRaidTerrain
{
    UPROPERTY()
    int GridDim = 20;          // tiles per side (GridDim*GridDim tiles total)

    UPROPERTY()
    float TileSize = 250.0;    // uu per tile edge

    UPROPERTY()
    float StepUU = 25.0;       // uu per height level (quantization step)

    // Row-major heights in LEVELS (multiply by StepUU for uu). Size == GridDim*GridDim.
    UPROPERTY()
    TArray<int> Heights;
}
```

- [ ] **Step 2: Add a `Terrain` field to `FRaidLayout`** (inside the struct, after `HalfExtent`)

```angelscript
    UPROPERTY()
    FRaidTerrain Terrain;
```

- [ ] **Step 3: Compile-check**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors. (No behavior yet — just the new data.)

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLayout.as
git commit -m "feat(procgen): FRaidTerrain heightfield data model"
```

---

### Task 2: Integer hash + lattice level (the determinism core)

**Files:**
- Create: `RogueSmoke/Script/Generation/RaidTerrain.as`

- [ ] **Step 1: Create the file with the integer hash + a self-test**

```angelscript
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
```

- [ ] **Step 2: Compile-check**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidTerrain.as
git commit -m "feat(procgen): deterministic integer hash + lattice level for terrain"
```

---

### Task 3: Smoothed, quantized tile height (fBm-ish, integer)

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidTerrain.as`

- [ ] **Step 1: Add coarse+fine layering and an integer box-blur**

Add inside `namespace RaidTerrain`:

```angelscript
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
```

- [ ] **Step 2: Compile-check**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidTerrain.as
git commit -m "feat(procgen): two-octave raw level + integer box-blur"
```

---

### Task 4: `Generate` the heightfield + `WorldHeightAt`

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidTerrain.as`

- [ ] **Step 1: Add `Generate` and world-space sampling**

Add inside `namespace RaidTerrain`. The terrain salt is a distinct prime so the terrain channel is decorrelated from the layout/elite streams.

```angelscript
    const int kTerrainSalt = 2750159;   // prime; decorrelates terrain from RaidGen (1000003) / roster

    // Build the heightfield for a seed. levels = how many quantized height steps the rim can reach.
    FRaidTerrain Generate(int seed, int gridDim, float tileSize, float stepUU, int levels)
    {
        FRaidTerrain T;
        T.GridDim = gridDim;
        T.TileSize = tileSize;
        T.StepUU = stepUU;

        int s = seed + kTerrainSalt;
        TArray<int> raw;
        for (int j = 0; j < gridDim; j++)
            for (int i = 0; i < gridDim; i++)
                raw.Add(RawLevel(s, i, j, levels));

        // Two blur passes -> gentle, walkable rolling terrain (still integer/deterministic).
        TArray<int> blurred = BlurOnce(raw, gridDim);
        blurred = BlurOnce(blurred, gridDim);
        T.Heights = blurred;
        return T;
    }

    // Grid index (clamped) for a world XY, given the footprint is centered on origin with
    // total span = GridDim*TileSize.
    int TileIndexAxis(const FRaidTerrain& T, float world)
    {
        float span = float(T.GridDim) * T.TileSize;
        float local = world + span * 0.5;                 // shift origin to corner
        int idx = int(Math::Floor(local / T.TileSize));
        if (idx < 0) idx = 0;
        if (idx >= T.GridDim) idx = T.GridDim - 1;
        return idx;
    }

    // World-space terrain height (uu) under an XY position.
    float WorldHeightAt(const FRaidTerrain& T, float x, float y)
    {
        if (T.Heights.Num() == 0)
            return 0.0;
        int i = TileIndexAxis(T, x);
        int j = TileIndexAxis(T, y);
        return float(T.Heights[j * T.GridDim + i]) * T.StepUU;
    }

    // Center of tile (i,j) in world XY.
    FVector TileCenter(const FRaidTerrain& T, int i, int j)
    {
        float span = float(T.GridDim) * T.TileSize;
        float x = -span * 0.5 + (float(i) + 0.5) * T.TileSize;
        float y = -span * 0.5 + (float(j) + 0.5) * T.TileSize;
        return FVector(x, y, 0.0);
    }
```

- [ ] **Step 2: Compile-check**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidTerrain.as
git commit -m "feat(procgen): terrain Generate + WorldHeightAt sampling"
```

---

### Task 5: Flatten + slope-clamp under a play disc

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidTerrain.as`

- [ ] **Step 1: Add `FlattenDisc` and `MaxSlopeInDisc`**

`FlattenDisc` pulls tile heights toward a target plane inside a radius (smooth falloff via integer weight), then a slope-clamp pass caps neighbor deltas so the play area is walkable. `MaxSlopeInDisc` is the validator's measurement.

```angelscript
    // Flatten terrain toward targetLevel inside worldRadius of (cx,cy): full flatten within innerFrac,
    // blended to natural out to the radius. Integer weights -> deterministic. Mutates T.Heights.
    void FlattenDisc(FRaidTerrain& T, float cx, float cy, int targetLevel, float worldRadius, float innerFrac)
    {
        float inner = worldRadius * innerFrac;
        for (int j = 0; j < T.GridDim; j++)
        {
            for (int i = 0; i < T.GridDim; i++)
            {
                FVector c = TileCenter(T, i, j);
                float d = Math::Sqrt((c.X - cx) * (c.X - cx) + (c.Y - cy) * (c.Y - cy));
                if (d > worldRadius)
                    continue;
                int cur = T.Heights[j * T.GridDim + i];
                // weight 100 (full target) at/under inner -> 0 (natural) at the radius edge.
                int w = 100;
                if (d > inner && worldRadius > inner)
                    w = int(100.0 * (1.0 - (d - inner) / (worldRadius - inner)));
                int blended = (targetLevel * w + cur * (100 - w)) / 100;
                T.Heights[j * T.GridDim + i] = blended;
            }
        }
        // Slope-clamp inside the disc: limit neighbor height delta to 1 level per tile.
        for (int pass = 0; pass < 4; pass++)
        {
            for (int j = 0; j < T.GridDim; j++)
            {
                for (int i = 0; i < T.GridDim; i++)
                {
                    FVector c = TileCenter(T, i, j);
                    float d = Math::Sqrt((c.X - cx) * (c.X - cx) + (c.Y - cy) * (c.Y - cy));
                    if (d > worldRadius)
                        continue;
                    int h = T.Heights[j * T.GridDim + i];
                    if (i + 1 < T.GridDim)
                    {
                        int r = T.Heights[j * T.GridDim + (i + 1)];
                        if (Math::Abs(h - r) > 1)
                            T.Heights[j * T.GridDim + (i + 1)] = (h > r) ? h - 1 : h + 1;
                    }
                    if (j + 1 < T.GridDim)
                    {
                        int dn = T.Heights[(j + 1) * T.GridDim + i];
                        if (Math::Abs(h - dn) > 1)
                            T.Heights[(j + 1) * T.GridDim + i] = (h > dn) ? h - 1 : h + 1;
                    }
                }
            }
        }
    }

    // Largest 4-neighbour height delta (in LEVELS) among tiles whose center is within worldRadius of
    // (cx,cy). The validator's slope-walkable measurement.
    int MaxSlopeInDisc(const FRaidTerrain& T, float cx, float cy, float worldRadius)
    {
        int maxDelta = 0;
        for (int j = 0; j < T.GridDim; j++)
        {
            for (int i = 0; i < T.GridDim; i++)
            {
                FVector c = TileCenter(T, i, j);
                float d = Math::Sqrt((c.X - cx) * (c.X - cx) + (c.Y - cy) * (c.Y - cy));
                if (d > worldRadius)
                    continue;
                int h = T.Heights[j * T.GridDim + i];
                if (i + 1 < T.GridDim)
                    maxDelta = Math::Max(maxDelta, Math::Abs(h - T.Heights[j * T.GridDim + (i + 1)]));
                if (j + 1 < T.GridDim)
                    maxDelta = Math::Max(maxDelta, Math::Abs(h - T.Heights[(j + 1) * T.GridDim + i]));
            }
        }
        return maxDelta;
    }
```

- [ ] **Step 2: Compile-check**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidTerrain.as
git commit -m "feat(procgen): FlattenDisc + slope-clamp + MaxSlopeInDisc"
```

---

### Task 6: Wire terrain into generation + sit nodes on it

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidLevelGenerator.as` (`FRaidGenConfig`, `Generate`, `BuildSafeFallback`)

- [ ] **Step 1: Add terrain config fields to `FRaidGenConfig`** (after the last field, before the closing brace)

```angelscript
    // --- Plan A: terrain floor ---
    UPROPERTY()
    int TerrainGridDim = 20;             // tiles per side

    UPROPERTY()
    float TerrainTileSize = 250.0;       // uu per tile

    UPROPERTY()
    float TerrainStepUU = 25.0;          // uu per height level (quantization)

    UPROPERTY()
    int TerrainLevels = 8;               // max rim height = (Levels-1)*StepUU ~= 175uu

    UPROPERTY()
    int ZonePlaneLevel = 2;              // flattened play-plane height level

    UPROPERTY()
    float ZoneFlattenRadius = 1900.0;    // flatten disc radius around a site (> SiteRadius)

    UPROPERTY()
    float ZoneFlattenInnerFrac = 0.6;    // fully flat within this fraction of the radius

    UPROPERTY()
    int MaxZoneSlopeLevels = 1;          // validator: max neighbour delta inside a zone
```

- [ ] **Step 2: Build + flatten terrain inside `RaidGen::Generate`**

In `Generate`, after `L.HalfExtent = Cfg.HalfExtent;` and before `FRandomStream Rng(...)`, insert:

```angelscript
        // Terrain floor: deterministic heightfield, flattened under the main play area.
        L.Terrain = RaidTerrain::Generate(Seed, Cfg.TerrainGridDim, Cfg.TerrainTileSize,
                                          Cfg.TerrainStepUU, Cfg.TerrainLevels);
```

Then, at the very end of `Generate` (after `L.MainSites.Add(BuildSkatepark(...))`, before `return L;`), flatten under the site and drop ground nodes onto the terrain:

```angelscript
        // Flatten the terrain under the site, then sit ground nodes/cover on the (flattened) surface.
        RaidTerrain::FlattenDisc(L.Terrain, SiteCenter.X, SiteCenter.Y, Cfg.ZonePlaneLevel,
                                 Cfg.ZoneFlattenRadius, Cfg.ZoneFlattenInnerFrac);
        SitNodesOnTerrain(L, Cfg);
```

- [ ] **Step 3: Add the `SitNodesOnTerrain` helper** (inside `namespace RaidGen`, e.g. after `EdgeMidpoint`)

Ground nodes (everything except HighGround, which carries an explicit Z offset) get their Z set to the terrain surface. HighGround keeps its platform Z but is *raised by* the surface beneath it so platforms stay reachable above the local ground. Drop/Extraction anchors sit on the surface too.

```angelscript
    // Place node/cover/anchor heights onto the terrain surface. HighGround platforms keep their
    // authored height but are lifted by the ground beneath them. Pure; mutates L in place.
    void SitNodesOnTerrain(FRaidLayout& L, const FRaidGenConfig& Cfg)
    {
        L.Drop.Center.Z = RaidTerrain::WorldHeightAt(L.Terrain, L.Drop.Center.X, L.Drop.Center.Y);
        L.Extraction.Center.Z = RaidTerrain::WorldHeightAt(L.Terrain, L.Extraction.Center.X, L.Extraction.Center.Y);
        for (int si = 0; si < L.MainSites.Num(); si++)
        {
            FVector c = L.MainSites[si].Center;
            L.MainSites[si].Center.Z = RaidTerrain::WorldHeightAt(L.Terrain, c.X, c.Y);
            for (int ni = 0; ni < L.MainSites[si].Nodes.Num(); ni++)
            {
                FVector p = L.MainSites[si].Nodes[ni].Location;
                float ground = RaidTerrain::WorldHeightAt(L.Terrain, p.X, p.Y);
                if (L.MainSites[si].Nodes[ni].Slot == ERaidSlotType::HighGround)
                    L.MainSites[si].Nodes[ni].Location.Z = p.Z + ground;   // platform above local ground
                else
                    L.MainSites[si].Nodes[ni].Location.Z = ground;
            }
            // Cover keeps its XY; its base rides the surface (Radius unchanged).
        }
    }
```

- [ ] **Step 4: Build terrain in `BuildSafeFallback` too** (so the fallback is also terrained)

In `BuildSafeFallback`, after `L.HalfExtent = Cfg.HalfExtent;`, insert:

```angelscript
        L.Terrain = RaidTerrain::Generate(0, Cfg.TerrainGridDim, Cfg.TerrainTileSize,
                                          Cfg.TerrainStepUU, Cfg.TerrainLevels);
        RaidTerrain::FlattenDisc(L.Terrain, 0.0, 0.0, Cfg.ZonePlaneLevel,
                                 Cfg.ZoneFlattenRadius, Cfg.ZoneFlattenInnerFrac);
```

and just before `L.MainSites.Add(S);` at the end, add `SitNodesOnTerrain` is not yet callable on `S` alone — instead, after `L.MainSites.Add(S);` and before `return L;`, call:

```angelscript
        SitNodesOnTerrain(L, Cfg);
```

- [ ] **Step 5: Compile-check**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 6: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLevelGenerator.as
git commit -m "feat(procgen): generate+flatten terrain and sit nodes on the surface"
```

---

### Task 7: `slope-walkable` validation check

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidLayoutValidator.as` (`Validate`)

- [ ] **Step 1: Add the slope check** (in `Validate`, after the `escape-proof` `Check(...)` line, before `R.bOk = ...`)

```angelscript
        // Slope-walkable: the flattened play disc around each site must not exceed the walkable
        // neighbour-delta cap (slide-hop + Mass pathing stay clean). Skips if terrain is empty.
        bool bSlopeOk = true;
        if (L.Terrain.Heights.Num() > 0)
        {
            for (FRaidSite S : L.MainSites)
            {
                int slope = RaidTerrain::MaxSlopeInDisc(L.Terrain, S.Center.X, S.Center.Y, Cfg.SiteRadius);
                if (slope > Cfg.MaxZoneSlopeLevels)
                    bSlopeOk = false;
            }
        }
        Check(R, bSlopeOk, "slope-walkable");
```

- [ ] **Step 2: Compile-check + confirm the count rose by 1**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors. (Validate now has 13 checks; reroll respects slope.)

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLayoutValidator.as
git commit -m "feat(procgen): slope-walkable validation over the flattened zone"
```

---

### Task 8: Stamp the terrain tiles (replace the floor slab)

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidStamper.as` (`StampLayout`, palette)

- [ ] **Step 1: Add a terrain tint constant** (next to the other `k*Color` consts)

```angelscript
    const FLinearColor kTerrainColor = FLinearColor(0.353, 0.275, 0.180, 1.0);  // brown — generated ground
```

- [ ] **Step 2: Replace the floor-slab batch with a terrain tile pass**

Find the floor batch in `StampLayout`:

```angelscript
        // Floor — one thin slab over the footprint.
        {
            TArray<FVector> C; TArray<FVector> S;
            C.Add(FVector(0.0, 0.0, -10.0)); S.Add(FVector(Span, Span, 20.0));
            Total += Stamp.StampBoxesColored(C, S, kFloorColor);
        }
```

Replace it with a terrain tile batch (each tile is a column whose top sits at its height):

```angelscript
        // Terrain floor — one brown column per tile, top at the tile height (stepped greybox terrain).
        {
            const FRaidTerrain T = L.Terrain;
            TArray<FVector> C; TArray<FVector> S;
            float colThick = 300.0;   // column thickness for solid collision under the surface
            for (int j = 0; j < T.GridDim; j++)
            {
                for (int i = 0; i < T.GridDim; i++)
                {
                    FVector center = RaidTerrain::TileCenter(T, i, j);
                    float h = float(T.Heights[j * T.GridDim + i]) * T.StepUU;
                    C.Add(FVector(center.X, center.Y, h - colThick * 0.5));   // top at h
                    S.Add(FVector(T.TileSize, T.TileSize, colThick));
                }
            }
            Total += Stamp.StampBoxesColored(C, S, kTerrainColor);
        }
```

- [ ] **Step 3: Compile-check**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidStamper.as
git commit -m "feat(procgen): stamp terrain tiles in place of the floor slab"
```

---

### Task 9: Extend `GenSmoke` (terrain present, determinism, slope)

**Files:**
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as` (`GenSmoke`)
- Modify: `Tools/SmokeTest.ps1` (the `ProcGenFoundation` case `Expect`)

- [ ] **Step 1: Read the current `GenSmoke` to anchor the edit**

Run: `Grep` for `void GenSmoke` in `RogueSmoke/Script/Player/RaidPlayerController.as` and read the function. It builds a layout, runs a series of `bool` checks, increments a pass counter, and `Print`s `[GenSmoke] RESULT <pass>/<total>` (currently 6/6). Each check is an `if`/counter pair.

- [ ] **Step 2: Add three terrain assertions inside `GenSmoke`**

Immediately before the line that prints the RESULT, add (using the same `pass`/`total` counter names the function already uses — read them in Step 1; the canonical names in this file are `Passed` and `Total`):

```angelscript
        // --- Plan A: terrain assertions ---
        FRaidGenConfig TCfg;
        FRaidLayout TL = RaidGen::GenerateValidated(20260614, TCfg);

        // (1) Terrain present: a full grid of heights.
        Total += 1;
        if (TL.Terrain.Heights.Num() == TCfg.TerrainGridDim * TCfg.TerrainGridDim)
            Passed += 1;
        else
            Print("[GenSmoke] FAIL terrain-present", 6.0);

        // (2) Determinism: same seed -> byte-identical terrain heights.
        FRaidLayout TL2 = RaidGen::GenerateValidated(20260614, TCfg);
        bool bSame = (TL.Terrain.Heights.Num() == TL2.Terrain.Heights.Num());
        if (bSame)
        {
            for (int i = 0; i < TL.Terrain.Heights.Num(); i++)
                if (TL.Terrain.Heights[i] != TL2.Terrain.Heights[i]) { bSame = false; break; }
        }
        Total += 1;
        if (bSame) Passed += 1; else Print("[GenSmoke] FAIL terrain-determinism", 6.0);

        // (3) Slope-walkable: the flattened play disc obeys the cap.
        int slope = RaidTerrain::MaxSlopeInDisc(TL.Terrain, TL.MainSites[0].Center.X,
                                                TL.MainSites[0].Center.Y, TCfg.SiteRadius);
        Total += 1;
        if (slope <= TCfg.MaxZoneSlopeLevels) Passed += 1;
        else Print(f"[GenSmoke] FAIL slope-walkable ({slope})", 6.0);
```

> If Step 1 shows the counters are named differently (e.g. `nPass`/`nTotal`), substitute those names. The assertion logic is unchanged.

- [ ] **Step 3: Bump the SmokeTest expectation**

In `Tools/SmokeTest.ps1`, the `ProcGenFoundation` case currently has `Expect = "[GenSmoke] RESULT 6/6"`. Change it to:

```powershell
    @{ Name = "ProcGenFoundation";  Map = "/Game/Levels/RaidArena";                       Expect = "[GenSmoke] RESULT 9/9"; Exec = "GenSmoke"; Window = 25 }
```

- [ ] **Step 4: Compile-check**

Run: `as-helper run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "test(procgen): GenSmoke asserts terrain present + determinism + slope (9/9)"
```

---

### Task 10: Full headless gate + visual proof

**Files:** none (verification only)

- [ ] **Step 1: Run the ProcGen foundation case headless**

Run (PowerShell):
```powershell
& "F:\UEAS\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\RogueSmoke.uproject" `
  /Game/Levels/RaidArena -game -unattended -nullrhi -nosound -nosplash `
  -abslog="$env:TEMP\rs_smoke\genA.log" -ExecCmds="GenSmoke"
```
Wait ~25s, kill, then grep the log.
Expected: log contains `[GenSmoke] RESULT 9/9` and no `Fatal error` / `Script call stack` / `LogScript: Error`.

- [ ] **Step 2: Run the StampArena + GenLoopVictory cases (terrain must not break stamping/loop)**

Boot `/Game/Levels/RaidArena` with `-ExecCmds="StampSmoke"` (expect `[StampSmoke] RESULT 2/2`) and `/Game/Levels/L_GenRaid` with `-ExecCmds="GenLoopSmoke"` (expect `[GenLoopSmoke] VICTORY confirmed`), same boot pattern as Step 1. Both must still pass — the terrain stamps under the existing loop.
Expected: both breadcrumbs present, no fatals.

- [ ] **Step 3: Visual proof — colored GenShots over terrain**

Boot `/Game/Levels/L_GenRaid` with rendering and `GenShots`:
```powershell
& "F:\UEAS\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" `
  "C:\Users\btblu\Documents\RogueSmoke\RogueSmoke\RogueSmoke.uproject" `
  /Game/Levels/L_GenRaid -game -RenderOffScreen -unattended -nosound -nosplash `
  -ResX=1600 -ResY=900 -abslog="$env:TEMP\rs_smoke\genAshots.log" -ExecCmds="GenShots"
```
Open the newest PNGs in `RogueSmoke\Saved\Screenshots\WindowsEditor\`.
Expected: the 45° shot shows brown stepped terrain under the colored zone markers (not a flat slab).

- [ ] **Step 4: Final commit (if any tuning was needed)**

If Steps 1–3 required tuning a config default (e.g., `TerrainLevels`, `ZonePlaneLevel`), commit it:
```bash
git add -A
git commit -m "tune(procgen): terrain defaults for slope-walkable + readable rim"
```

---

## Self-review against the spec

- **Spec §5 step 1 (terrain) + §8 (integer hash-noise, quantized, stamped via seam):** Tasks 2–4 (hash/level/blur/Generate), Task 8 (stamp via `StampBoxesColored`). ✓
- **Spec §5 step 3 flatten + §7 #5 slope-walkable:** Task 5 (`FlattenDisc`/`MaxSlopeInDisc`), Task 7 (validator check). ✓
- **Spec §2 determinism (integer math, no float interp on height):** Task 2 note + Task 9 determinism assertion. ✓
- **Spec §6 data model (`FRaidTerrain`):** Task 1. ✓
- **Spec §8 testing (GenSmoke terrain assertions, suite stays green):** Tasks 9–10. ✓
- **Out of scope for Plan A (correctly deferred to B/C):** multi-site partition, Voronoi, drop/extraction ends, objective manager, director. Not present here. ✓
- **Type consistency:** `FRaidTerrain` fields (`GridDim`/`TileSize`/`StepUU`/`Heights`) used identically in Tasks 1/4/5/8; `RaidTerrain::WorldHeightAt`/`TileCenter`/`MaxSlopeInDisc`/`FlattenDisc`/`Generate` signatures match across Tasks 4–9; `FRaidGenConfig` terrain fields defined in Task 6 Step 1 and consumed in Tasks 6–9. ✓
- **Note:** Task 9 Step 2 depends on the actual `GenSmoke` pass/total counter names — Step 1 reads them first and Step 2 says to substitute. This is the one place the engineer must check the live code rather than copy verbatim.
