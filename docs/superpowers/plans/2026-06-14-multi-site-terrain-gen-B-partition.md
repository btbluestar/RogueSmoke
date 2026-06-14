# Multi-Site Terrain Gen — Plan B: Multi-Site Partition & Placement

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the single center Skatepark site with **2–3 seed-rolled objective zones** placed by reserve→Poisson→min-separation, each with its own flattened terrain disc, so the layout becomes a real multi-zone level on the generated terrain — proven by `run_code_test` and the headless `GenSmoke` regression.

**Architecture:** Pure AngelScript in `Script/Generation/`. A new `RaidPartition` namespace places 2–3 zone anchors by rejection-sampled Poisson (min-separation, drop/extraction exclusion), reusing the seeded `FRandomStream`. `RaidGen::Generate` loops the existing `BuildSkatepark` + `RaidTerrain::FlattenDisc` per anchor (drop/extraction already sit at opposite edge midpoints). Per-site footprint shrinks so multiple sites fit. New placement invariants fold into `RaidValidate::Validate`. **No new C++.** This is **Plan B of 3**; A (terrain) is done, C (runtime objective manager + director) is next.

> **Implementation note on Voronoi:** the design spec frames placement as "Poisson anchors → Voronoi partition into zone territories." For this greybox slice the *playable* realization of "each zone owns flat territory, lanes between stay wild" is achieved by **one `FlattenDisc` per zone anchor** (flat play discs separated by un-flattened terrain lanes). True Voronoi tile-assignment adds no gameplay value yet and is deliberately deferred (YAGNI). The min-separation guarantees the discs don't overlap.

**Tech Stack:** UnrealEngine-Angelscript (Hazelight UE5.7 fork); `FRandomStream` for seed entropy; verification via `as-helper run_code_test` and `Tools/SmokeTest.ps1` (`[GenSmoke] RESULT n/n`).

---

## Design notes the engineer must know

- **Determinism:** all placement randomness goes through the existing `FRandomStream Rng(Seed + RaidGen::kArenaSalt)` already created in `RaidGen::Generate`. Reuse that stream (do not make a new one) so the existing per-retry reroll salt still works. Terrain heights remain integer-hash (untouched).
- **Sites must fit.** Today `FRaidGenConfig.SiteRadius = 1600` nearly fills the 5000×5000 footprint. Plan B reduces it to `1000` and adds `ZoneMinSeparation = 2200` (≈ 2× a site's footprint) so 2–3 discs never overlap. `ZoneFlattenRadius` drops to `1300` (site + margin) accordingly. These are the only behavior-affecting default changes.
- **Drop/extraction are unchanged** — they already land on opposite edge midpoints (`EdgeMidpoint`) in `Generate`. Zone anchors must avoid them by `ZoneDropClearance`.
- **`SitNodesOnTerrain` already loops `L.MainSites`** (Plan A) — it handles N sites with no change. `RaidStamper::StampLayout` already loops `L.MainSites` for platforms/cover, and `SlotLocation`/markers already scan all sites — so the **Maw/Hold markers will only mark the FIRST site's nodes** (that is fine for Plan B; per-site markers and the active-site logic come in Plan C). No stamper change needed in Plan B.
- **Existing types you build on:**
  - `RaidGen::Generate` / `GenerateValidated` / `BuildSafeFallback` / `BuildSkatepark(FVector Center, const FRaidGenConfig& Cfg, FRandomStream& Rng)` / `MakeAnchorSite` / `EdgeMidpoint` in `RaidLevelGenerator.as`.
  - `RaidTerrain::FlattenDisc(FRaidTerrain& T, float cx, float cy, int targetLevel, float worldRadius, float innerFrac)`.
  - `RaidValidate::Validate` with `Check(R, bPass, "label")`, `InBounds(P, Limit)`, `CountSlot`.
  - `FRaidLayout.MainSites` is a `TArray<FRaidSite>`.
- **No `import` statements** (symbols are globally visible).
- **Verification:** `run_code_test` (compile/run); `SmokeTest.ps1` `ProcGenFoundation` boots `RaidArena` headless with `-ExecCmds=GenSmoke` and greps `[GenSmoke] RESULT n/n` (currently `9/9` after Plan A).
- **Branch:** `multi-site-terrain-gen`. Commit after every task. Pure AngelScript — never run a C++ build or the interactive editor; one `run_code_test` at a time.

## File structure

- **Create** `RogueSmoke/Script/Generation/RaidPartition.as` — `RaidPartition::PlaceZoneAnchors` (Poisson rejection placement with exclusions).
- **Modify** `RogueSmoke/Script/Generation/RaidLevelGenerator.as` — multi-site config fields; reduce `SiteRadius`/`ZoneFlattenRadius`; multi-site loop in `Generate`; multi-site `BuildSafeFallback`.
- **Modify** `RogueSmoke/Script/Generation/RaidLayoutValidator.as` — zone-count, zone-separation, zone-drop-clearance checks.
- **Modify** `RogueSmoke/Script/Player/RaidPlayerController.as` — `GenSmoke` multi-site assertions.
- **Modify** `Tools/SmokeTest.ps1` — bump `GenSmoke` expected count.

---

### Task 1: `RaidPartition` — Poisson zone-anchor placement

**Files:**
- Create: `RogueSmoke/Script/Generation/RaidPartition.as`

- [ ] **Step 1: Create the file**

```angelscript
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
```

- [ ] **Step 2: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidPartition.as
git commit -m "feat(procgen): RaidPartition Poisson zone-anchor placement"
```

---

### Task 2: Multi-site config fields + smaller footprint

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidLevelGenerator.as` (`FRaidGenConfig`)

- [ ] **Step 1: Shrink per-site footprint defaults** — change two existing fields:

Change `float SiteRadius = 1600.0;` to:
```angelscript
    float SiteRadius = 1000.0;           // per-zone footprint radius (shrunk so 2-3 zones fit)
```
Change `float ZoneFlattenRadius = 1900.0;` to:
```angelscript
    float ZoneFlattenRadius = 1300.0;    // flatten disc radius around a site (> SiteRadius)
```

- [ ] **Step 2: Add multi-site fields** — append as the LAST fields inside `FRaidGenConfig` (after `MaxZoneSlopeLevels`, before the closing brace):

```angelscript
    // --- Plan B: multi-site placement ---
    UPROPERTY()
    int ZoneCountMin = 2;                // fewest objective zones per raid

    UPROPERTY()
    int ZoneCountMax = 3;                // most objective zones per raid

    UPROPERTY()
    float ZoneMinSeparation = 2200.0;    // min distance between zone centers (>= 2*SiteRadius)

    UPROPERTY()
    float ZoneAnchorMargin = 1400.0;     // keep anchors this far inside the footprint half-extent

    UPROPERTY()
    float ZoneDropClearance = 1500.0;    // keep zones this far from drop/extraction
```

- [ ] **Step 3: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLevelGenerator.as
git commit -m "feat(procgen): multi-site config + shrink per-zone footprint"
```

---

### Task 3: Multi-site loop in `RaidGen::Generate`

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidLevelGenerator.as` (`Generate`)

- [ ] **Step 1: Replace the single-site block** — find this exact block in `Generate`:

```angelscript
        // One Skatepark main site near center, bounded jitter.
        FVector SiteCenter = FVector(Rng.RandRange(-300.0, 300.0), Rng.RandRange(-300.0, 300.0), 0.0);
        L.MainSites.Add(BuildSkatepark(SiteCenter, Cfg, Rng));

        // Flatten the terrain under the site, then sit ground nodes/cover on the (flattened) surface.
        RaidTerrain::FlattenDisc(L.Terrain, SiteCenter.X, SiteCenter.Y, Cfg.ZonePlaneLevel,
                                 Cfg.ZoneFlattenRadius, Cfg.ZoneFlattenInnerFrac);
        SitNodesOnTerrain(L, Cfg);
```

Replace it with the multi-site placement loop:

```angelscript
        // 2-3 objective zones, placed by reserve-then-populate Poisson (drop+extraction reserved).
        TArray<FVector> Reserved;
        Reserved.Add(L.Drop.Center);
        Reserved.Add(L.Extraction.Center);
        int ZoneCount = Rng.RandRange(Cfg.ZoneCountMin, Cfg.ZoneCountMax);
        TArray<FVector> Anchors = RaidPartition::PlaceZoneAnchors(
            Rng, Cfg.ZoneAnchorMargin, ZoneCount, Cfg.ZoneMinSeparation, Reserved, Cfg.ZoneDropClearance);

        for (int z = 0; z < Anchors.Num(); z++)
        {
            FVector SiteCenter = Anchors[z];
            L.MainSites.Add(BuildSkatepark(SiteCenter, Cfg, Rng));
            // Flatten this zone's play disc.
            RaidTerrain::FlattenDisc(L.Terrain, SiteCenter.X, SiteCenter.Y, Cfg.ZonePlaneLevel,
                                     Cfg.ZoneFlattenRadius, Cfg.ZoneFlattenInnerFrac);
        }

        // Sit all nodes/cover/anchors on the (per-zone flattened) surface.
        SitNodesOnTerrain(L, Cfg);
```

(Note: if `PlaceZoneAnchors` returns fewer than `ZoneCountMin` anchors for a tight roll, the layout will have too few sites — the validator's zone-count check (Task 4) catches it and `GenerateValidated` rerolls; the safe fallback always has 2.)

- [ ] **Step 2: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLevelGenerator.as
git commit -m "feat(procgen): place 2-3 objective zones per raid (multi-site)"
```

---

### Task 4: Multi-site validation invariants

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidLayoutValidator.as` (`Validate`)

- [ ] **Step 1: Replace the `has-main-site` check with multi-site checks** — find:

```angelscript
        // 3. At least one main objective site.
        Check(R, L.MainSites.Num() >= 1, "has-main-site");
```

Replace with:

```angelscript
        // 3. Zone count in the configured range.
        Check(R, L.MainSites.Num() >= Cfg.ZoneCountMin && L.MainSites.Num() <= Cfg.ZoneCountMax,
              "zone-count");

        // 3b. Zone centers are min-separated (no overlapping play discs) and clear of drop/extraction.
        bool bZoneSepOk = true;
        bool bZoneClearOk = true;
        for (int a = 0; a < L.MainSites.Num(); a++)
        {
            FVector Ca = L.MainSites[a].Center;
            FVector Ca2 = FVector(Ca.X, Ca.Y, 0.0);
            if (FVector(Ca.X - L.Drop.Center.X, Ca.Y - L.Drop.Center.Y, 0.0).Size() < Cfg.ZoneDropClearance - 0.5)
                bZoneClearOk = false;
            if (FVector(Ca.X - L.Extraction.Center.X, Ca.Y - L.Extraction.Center.Y, 0.0).Size() < Cfg.ZoneDropClearance - 0.5)
                bZoneClearOk = false;
            for (int b = a + 1; b < L.MainSites.Num(); b++)
            {
                FVector Cb = L.MainSites[b].Center;
                if (FVector(Ca.X - Cb.X, Ca.Y - Cb.Y, 0.0).Size() < Cfg.ZoneMinSeparation - 0.5)
                    bZoneSepOk = false;
            }
        }
        Check(R, bZoneSepOk, "zone-separation");
        Check(R, bZoneClearOk, "zone-drop-clearance");
```

- [ ] **Step 2: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLayoutValidator.as
git commit -m "feat(procgen): zone-count + separation + drop-clearance validation"
```

---

### Task 5: Multi-site safe fallback

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidLevelGenerator.as` (`BuildSafeFallback`)

The fallback currently builds ONE center site. Make it build exactly **two** well-separated zones so it passes the new `zone-count` (>=2) and `zone-separation` checks.

- [ ] **Step 1: Replace the single fallback site** — find this block in `BuildSafeFallback` (the part that builds `S` and adds it):

```angelscript
        FRaidSite S;
        S.Type = ERaidSiteType::MainObjective;
        S.Objective = ERaidObjectiveType::HoldAndChannel;
        S.Archetype = ERaidArchetype::Skatepark;
        S.Center = FVector::ZeroVector;
        S.Nodes.Add(MakeNode(ERaidSlotType::CombatCore, FVector::ZeroVector, 0.8));
        S.Nodes.Add(MakeNode(ERaidSlotType::HoldAnchor, FVector(Cfg.HoldAnchorMinOffset + 100.0, 0.0, 0.0), 1.0));
        for (int i = 0; i < Cfg.HighGroundCount; i++)
        {
            float A = (2.0 * PI * float(i)) / float(Cfg.HighGroundCount);
            FVector P = FVector(Math::Cos(A), Math::Sin(A), 0.0) * (Cfg.SiteRadius * 0.7);
            P.Z = (Cfg.HighGroundMinZ + Cfg.HighGroundMaxZ) * 0.5;
            S.Nodes.Add(MakeNode(ERaidSlotType::HighGround, P, 0.5));
        }
        S.Nodes.Add(MakeNode(ERaidSlotType::Entrance, FVector(-Cfg.SiteRadius, 0.0, 0.0), 0.2));
        S.Nodes.Add(MakeNode(ERaidSlotType::Exit,     FVector( Cfg.SiteRadius, 0.0, 0.0), 0.2));
        // A ring of exactly CoverMin cover, spaced well above CoverMinSeparation.
        for (int i = 0; i < Cfg.CoverMin; i++)
        {
            float A = (2.0 * PI * float(i)) / float(Cfg.CoverMin);
            FRaidCover C;
            C.Location = FVector(Math::Cos(A), Math::Sin(A), 0.0) * (Cfg.SiteRadius * 0.55);
            C.Radius = Cfg.CoverRadius;
            S.Cover.Add(C);
        }
        L.MainSites.Add(S);
```

Replace it with two zones built by a local helper, offset on the Y axis by the min-separation:

```angelscript
        // Two well-separated fallback zones (passes zone-count>=2 and zone-separation).
        float Off = Cfg.ZoneMinSeparation * 0.5 + 50.0;
        L.MainSites.Add(BuildFallbackZone(FVector(0.0,  Off, 0.0), Cfg));
        L.MainSites.Add(BuildFallbackZone(FVector(0.0, -Off, 0.0), Cfg));
        // Flatten both fallback discs.
        RaidTerrain::FlattenDisc(L.Terrain, 0.0,  Off, Cfg.ZonePlaneLevel, Cfg.ZoneFlattenRadius, Cfg.ZoneFlattenInnerFrac);
        RaidTerrain::FlattenDisc(L.Terrain, 0.0, -Off, Cfg.ZonePlaneLevel, Cfg.ZoneFlattenRadius, Cfg.ZoneFlattenInnerFrac);
```

- [ ] **Step 2: Add the `BuildFallbackZone` helper** — add inside `namespace RaidGen` (e.g. right after `BuildSafeFallback`):

```angelscript
    // A deterministic, guaranteed-valid Skatepark zone centered at 'Center' for the safe fallback.
    FRaidSite BuildFallbackZone(FVector Center, const FRaidGenConfig& Cfg)
    {
        FRaidSite S;
        S.Type = ERaidSiteType::MainObjective;
        S.Objective = ERaidObjectiveType::HoldAndChannel;
        S.Archetype = ERaidArchetype::Skatepark;
        S.Center = Center;
        S.Nodes.Add(MakeNode(ERaidSlotType::CombatCore, Center, 0.8));
        S.Nodes.Add(MakeNode(ERaidSlotType::HoldAnchor, Center + FVector(Cfg.HoldAnchorMinOffset + 100.0, 0.0, 0.0), 1.0));
        for (int i = 0; i < Cfg.HighGroundCount; i++)
        {
            float A = (2.0 * PI * float(i)) / float(Cfg.HighGroundCount);
            FVector P = Center + FVector(Math::Cos(A), Math::Sin(A), 0.0) * (Cfg.SiteRadius * 0.7);
            P.Z = (Cfg.HighGroundMinZ + Cfg.HighGroundMaxZ) * 0.5;
            S.Nodes.Add(MakeNode(ERaidSlotType::HighGround, P, 0.5));
        }
        S.Nodes.Add(MakeNode(ERaidSlotType::Entrance, Center + FVector(-Cfg.SiteRadius, 0.0, 0.0), 0.2));
        S.Nodes.Add(MakeNode(ERaidSlotType::Exit,     Center + FVector( Cfg.SiteRadius, 0.0, 0.0), 0.2));
        for (int i = 0; i < Cfg.CoverMin; i++)
        {
            float A = (2.0 * PI * float(i)) / float(Cfg.CoverMin);
            FRaidCover C;
            C.Location = Center + FVector(Math::Cos(A), Math::Sin(A), 0.0) * (Cfg.SiteRadius * 0.55);
            C.Radius = Cfg.CoverRadius;
            S.Cover.Add(C);
        }
        return S;
    }
```

Note: the existing terrain build (`RaidTerrain::Generate` + a single center `FlattenDisc`) earlier in `BuildSafeFallback` from Plan A — **remove the now-redundant single center `FlattenDisc` line** there (the one flattening at `0.0, 0.0`), since the two zone discs above replace it. Keep the `RaidTerrain::Generate(0, ...)` line. The `SitNodesOnTerrain(L, Cfg)` call at the end of `BuildSafeFallback` stays.

- [ ] **Step 3: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLevelGenerator.as
git commit -m "feat(procgen): two-zone safe fallback (passes multi-site checks)"
```

---

### Task 6: `GenSmoke` multi-site assertions

**Files:**
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as` (`GenSmoke`)
- Modify: `Tools/SmokeTest.ps1`

- [ ] **Step 1: Read the current `GenSmoke`** — confirm the pass/total counter names (Plan A confirmed `Pass` / `Total`, `+= 1` style). Find the Plan-A terrain assertions block (it builds `TL`/`TL2` from `RaidGen::GenerateValidated(20260614, TCfg)`); add the new checks immediately AFTER it and before the RESULT print, reusing `TL`/`TCfg`.

- [ ] **Step 2: Add three multi-site assertions** (substitute the real counter names if they differ):

```angelscript
        // --- Plan B: multi-site assertions ---
        // (1) Zone count in range.
        Total += 1;
        if (TL.MainSites.Num() >= TCfg.ZoneCountMin && TL.MainSites.Num() <= TCfg.ZoneCountMax)
            Pass += 1;
        else
            Print(f"[GenSmoke] FAIL zone-count ({TL.MainSites.Num()})", 6.0);

        // (2) Zone centers min-separated.
        bool bSep = true;
        for (int za = 0; za < TL.MainSites.Num(); za++)
            for (int zb = za + 1; zb < TL.MainSites.Num(); zb++)
            {
                FVector Da = TL.MainSites[za].Center;
                FVector Db = TL.MainSites[zb].Center;
                if (FVector(Da.X - Db.X, Da.Y - Db.Y, 0.0).Size() < TCfg.ZoneMinSeparation - 0.5)
                    bSep = false;
            }
        Total += 1;
        if (bSep) Pass += 1; else Print("[GenSmoke] FAIL zone-separation", 6.0);

        // (3) Determinism: same seed -> same zone count + centers.
        FRaidLayout TL3 = RaidGen::GenerateValidated(20260614, TCfg);
        bool bMS = (TL3.MainSites.Num() == TL.MainSites.Num());
        if (bMS)
            for (int zc = 0; zc < TL.MainSites.Num(); zc++)
                if ((TL.MainSites[zc].Center - TL3.MainSites[zc].Center).Size() > 0.01) { bMS = false; break; }
        Total += 1;
        if (bMS) Pass += 1; else Print("[GenSmoke] FAIL multisite-determinism", 6.0);
```

- [ ] **Step 3: Bump the SmokeTest expectation** — in `Tools/SmokeTest.ps1`, change the `ProcGenFoundation` case `Expect = "[GenSmoke] RESULT 9/9"` to:

```powershell
    @{ Name = "ProcGenFoundation";  Map = "/Game/Levels/RaidArena";                       Expect = "[GenSmoke] RESULT 12/12"; Exec = "GenSmoke"; Window = 25 }
```

- [ ] **Step 4: Compile-check**

Run: `mcp__plugin_ue-as_as-helper__run_code_test` (filter `errors_only`)
Expected: `Exit code: 0 (PASS)`, 0 errors.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "test(procgen): GenSmoke asserts 2-3 zones + separation + determinism (12/12)"
```

---

### Task 7: Headless gate + visual proof

**Files:** none (verification only — run from the controller session, not a subagent, per the single-editor rule)

- [ ] **Step 1: GenSmoke 12/12** — boot `/Game/Levels/RaidArena` headless with `-ExecCmds="GenSmoke"`, ~25s, grep `[GenSmoke] RESULT 12/12`, no fatals.

- [ ] **Step 2: Stamp + loop still green** — boot `RaidArena` with `StampSmoke` (expect `[StampSmoke] RESULT 2/2`, `valid=true`) and `L_GenRaid` with `GenLoopSmoke` (expect `[GenLoopSmoke] VICTORY confirmed`). The loop must still reach victory with 2-3 zones (the objective today reads `MainSites[0]` — that still resolves; full multi-site objective logic is Plan C).

- [ ] **Step 3: Visual proof** — boot `L_GenRaid` with `-RenderOffScreen -ExecCmds="GenShots"`; open the newest PNGs in `Saved\Screenshots\WindowsEditor\`. Expected: 2-3 separated flat zones on the brown terrain, each with its own cover/platforms, lanes of un-flattened terrain between them.

- [ ] **Step 4: Tune if needed** — if a zone clips the rim or zones look cramped, adjust `ZoneAnchorMargin` / `ZoneMinSeparation` / `SiteRadius` and re-verify, then commit:
```bash
git add -A && git commit -m "tune(procgen): multi-site spacing"
```

---

## Self-review against the spec

- **Spec §4 decision 3 (2-3 sites, free order) + §5 (multi main sites):** Tasks 1-3 (placement + loop), Task 4 (zone-count). ✓
- **Spec §5 step 2 (reserve→Poisson→min-separation):** Task 1 (`PlaceZoneAnchors` with reserved + minSep), Task 3 (drop/extraction reserved). ✓
- **Spec §4 decision 6 (drop/extraction opposite ends):** unchanged from Plan A (`EdgeMidpoint` opposite edges) + Task 4 `zone-drop-clearance`. ✓
- **Spec §5 step 3 (flatten per zone) / terrain lanes between:** Task 3 (one `FlattenDisc` per anchor). Voronoi tile-assignment deferred (documented YAGNI). ✓
- **Spec §7 #1 zone min-separation, #3 no overlap, #2 drop↔extract far:** Task 4. ✓
- **Spec §8 testing (multi-site GenSmoke, suite stays green):** Tasks 6-7. ✓
- **Out of scope (Plan C):** the objective manager reading all sites, active-site director, per-site markers, last-site climax. Not here. ✓
- **Type consistency:** `RaidPartition::PlaceZoneAnchors(FRandomStream&, float, int, float, const TArray<FVector>&, float)` defined in Task 1, called in Task 3 with matching args; config fields `ZoneCountMin/Max`, `ZoneMinSeparation`, `ZoneAnchorMargin`, `ZoneDropClearance` defined in Task 2, used in Tasks 3/4/6; `BuildFallbackZone(FVector, const FRaidGenConfig&)` defined+called in Task 5. ✓
- **Risk:** if `ZoneMinSeparation`/`ZoneAnchorMargin` are too tight for 3 zones, `PlaceZoneAnchors` returns 2 and the reroll/ fallback handle it — Task 7 Step 4 tunes if 3-zone rolls are too rare. Defaults (margin 1400, sep 2200) fit 3 anchors in the ±1400 interior box (diagonal ~3960 > 2×2200? no — verify empirically in Task 7; loosen margin to 1600 or sep to 2000 if 3 never fits).
