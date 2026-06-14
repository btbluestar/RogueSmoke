# Procedural Raid Generation — Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A deterministic, seeded layout generator that produces a validated `FRaidLayout` (a Skatepark objective site + a shared drop site + a separate extraction site) from one integer master seed, proven by `run_code_test` and a headless `GenSmoke` regression case.

**Architecture:** Pure AngelScript in `Script/Generation/` — no world reads, no geometry stamping yet. A data model (`FRaidLayout`), a seeded generator (`RaidGen::Generate` / `GenerateValidated`), and a pure-geometric validation battery (`RaidValidate::Validate`) with deterministic reroll + a known-good fallback. Verified headlessly so it never touches the wedge-prone interactive editor. This is **Plan 1 of 3** for the MVP (spec §13 Phase 0 + the pure-geometric slice of the §9 validation battery); stamping/PCG and objective/director evolution are Plans 2 and 3.

**Tech Stack:** UnrealEngine-Angelscript (Hazelight UE5.7 fork); `FRandomStream` for determinism; project verification via `as-helper run_code_test` (compile + run) and `Tools/SmokeTest.ps1` (headless `-ExecCmds` boot, asserts `[GenSmoke] RESULT n/n`).

---

## Design notes the engineer must know

- **Determinism is the whole point** (CODING_STANDARDS §5, D-0007). Every random draw goes through a
  `FRandomStream` seeded off the master seed. Same seed → identical layout on every machine. **No**
  unseeded random, no wall-clock, no actor-iteration-order, no hash-order.
- **This layer is pure data + math.** It must compile and run under `run_code_test` *without a world*
  (no `GetWorld()`, no `SpawnActor`, no subsystem calls). That is why it is independently testable.
- **Cross-file symbols need no `import`** in this project — script symbols are globally visible (see
  `RaidObjective.as` using `RaidDirector::ComputeWavePlan` / `FWavePlan` with no import line). Do not
  add `import` statements.
- **AngelScript idioms in use here:** `struct` with `UPROPERTY()` members + default initializers
  (mirror `FDirectorTunables` in `RaidWaveDirector.as`); `namespace Foo { ... }` free functions
  (mirror `RaidDirector`); `FRandomStream Rng(intSeed)` constructor, `Rng.RandRange(min,max)` (int
  and float overloads), `Math::Cos/Sin/Sqrt/Max/Min/Clamp`, `FVector`, `(A-B).Size()`,
  `f"format {x}"` strings, `n"Name"` names, `Print(msg, seconds)`.
- **Verification reality:** `run_code_test` is the fast inner loop (does it compile + run a snippet
  without a script error, and Print the expected line). `SmokeTest.ps1` is the regression gate (boots
  `/Game/Levels/RaidArena` headless with `-ExecCmds=GenSmoke`, greps the log for the RESULT line).
  **No new level asset or editor work is required for this plan** — we reuse the existing RaidArena
  map purely as a host for the exec.
- **Branch:** work on `ProcedualLevelGeneration` (already checked out). Commit after every task.

## File structure

- Create `RogueSmoke/Script/Generation/RaidLayout.as` — the data model: enums + structs
  (`FRaidNode`, `FRaidCover`, `FRaidSite`, `FRaidLayout`). One responsibility: the data contract.
- Create `RogueSmoke/Script/Generation/RaidLevelGenerator.as` — `FRaidGenConfig` + the
  `RaidGen` namespace: `Generate`, `GenerateValidated`, `LayoutsEqual`, and private build helpers.
- Create `RogueSmoke/Script/Generation/RaidLayoutValidator.as` — `FRaidValidationResult` + the
  `RaidValidate` namespace: the pure-geometric §9 checks (the subset not needing a world).
- Modify `RogueSmoke/Script/Player/RaidPlayerController.as` — add one `UFUNCTION(Exec) void GenSmoke()`
  (mirrors the existing `MoveSmoke` exec at ~line 717).
- Modify `Tools/SmokeTest.ps1` — add a `ProcGenFoundation` case.

---

### Task 1: The data model (`FRaidLayout`)

**Files:**
- Create: `RogueSmoke/Script/Generation/RaidLayout.as`

- [ ] **Step 1: Write the data model file**

```angelscript
// RaidLayout.as
// The data contract between procedural generation and the rest of the raid (design spec §5, §11).
// PURE DATA — no world reads, no logic. Produced by RaidGen::Generate (RaidLevelGenerator.as),
// consumed later by stamping / objectives / the wave director. Deterministic per master seed (D-0007).

enum ERaidArchetype
{
    Skatepark,      // MVP anchor: asymmetric verticality — the movement + combat showcase
    CombatBowl,
    Figure8,
    TieredPit,
    OpenSprawl
}

enum ERaidSiteType
{
    Drop,           // shared squad insertion (one per raid)
    MainObjective,  // a typed task site (2-3 per raid; 1 in MVP)
    Extraction,     // separate, ~opposite site; inert until mains complete
    Secondary       // optional POI (later)
}

enum ERaidObjectiveType
{
    None,           // Drop / Extraction sites carry no task
    HoldAndChannel, // MVP objective: stand in a node's radius until a bar fills
    ActivateAndDefend,
    CollectAndDeposit,
    DestroyStructure,
    EliminateTarget
}

enum ERaidSlotType
{
    Entrance,
    CombatCore,     // the Maw — primary swarm source + landmark
    FlankLoop,
    HighGround,
    HoldAnchor,     // where the mini-boss falls / the defend point
    Exit
}

struct FRaidNode
{
    UPROPERTY()
    ERaidSlotType Slot = ERaidSlotType::CombatCore;

    UPROPERTY()
    FVector Location = FVector::ZeroVector;

    // Wave-director intensity cap for this node's role (design spec §8), 0..1.
    UPROPERTY()
    float IntensityCap = 0.0;
}

struct FRaidCover
{
    UPROPERTY()
    FVector Location = FVector::ZeroVector;

    UPROPERTY()
    float Radius = 120.0;   // footprint radius; the Poisson min-separation key
}

struct FRaidSite
{
    UPROPERTY()
    ERaidSiteType Type = ERaidSiteType::MainObjective;

    UPROPERTY()
    ERaidObjectiveType Objective = ERaidObjectiveType::None;

    UPROPERTY()
    ERaidArchetype Archetype = ERaidArchetype::Skatepark;

    UPROPERTY()
    FVector Center = FVector::ZeroVector;

    UPROPERTY()
    TArray<FRaidNode> Nodes;

    UPROPERTY()
    TArray<FRaidCover> Cover;
}

struct FRaidLayout
{
    UPROPERTY()
    int Seed = 0;

    // Set true by GenerateValidated once the layout passes the battery (or is the safe fallback).
    UPROPERTY()
    bool bValid = false;

    // Half-extent of the square playable footprint (uu). Fixed for MVP (dial #7 = a later plan).
    UPROPERTY()
    float HalfExtent = 2500.0;

    UPROPERTY()
    FRaidSite Drop;

    UPROPERTY()
    TArray<FRaidSite> MainSites;

    UPROPERTY()
    FRaidSite Extraction;
}
```

- [ ] **Step 2: Verify it compiles**

Run the as-helper `run_code_test` MCP tool with this body:

```angelscript
FRaidLayout L;
L.Seed = 7;
FRaidNode N;
N.Slot = ERaidSlotType::HoldAnchor;
L.MainSites.Add(FRaidSite());
L.MainSites[0].Nodes.Add(N);
Print(f"[T1] seed={L.Seed} sites={L.MainSites.Num()} nodes={L.MainSites[0].Nodes.Num()} slot={N.Slot}", 5.0);
```

Expected: no compile/script error; output contains `[T1] seed=7 sites=1 nodes=1 slot=`.
(If `run_code_test` reports the editor is wedged, fall back to a clean `mcp__ue-cpp__build` is **not**
needed here — this is script-only; instead retry once, then proceed; the SmokeTest in Task 6 is the
real gate.)

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLayout.as
git commit -m "feat(procgen): FRaidLayout data model (sites/nodes/cover)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: The seeded generator (`RaidGen::Generate`)

**Files:**
- Create: `RogueSmoke/Script/Generation/RaidLevelGenerator.as`

- [ ] **Step 1: Write the generator file**

```angelscript
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
```

- [ ] **Step 2: Verify determinism + variety via `run_code_test`**

Run the as-helper `run_code_test` tool with this body:

```angelscript
FRaidGenConfig Cfg;
FRaidLayout A = RaidGen::Generate(12345, Cfg);
FRaidLayout B = RaidGen::Generate(12345, Cfg);
FRaidLayout C = RaidGen::Generate(99999, Cfg);
bool same = RaidGen::LayoutsEqual(A, B);
bool diff = !RaidGen::LayoutsEqual(A, C);
Print(f"[T2] sameSeedEqual={same} diffSeedDiffer={diff} nodes={A.MainSites[0].Nodes.Num()} cover={A.MainSites[0].Cover.Num()}", 8.0);
```

Expected: no script error; output line `[T2] sameSeedEqual=true diffSeedDiffer=true nodes=8 cover=...`
(cover count is seed-dependent but constant for seed 12345; `nodes=8` = core+hold+3 highground+entrance+exit+... = 8).

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLevelGenerator.as
git commit -m "feat(procgen): seeded Skatepark layout generator (deterministic)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: The pure-geometric validation battery (`RaidValidate::Validate`)

**Files:**
- Create: `RogueSmoke/Script/Generation/RaidLayoutValidator.as`

Implements the subset of the §9 invariants that need no world (navmesh + jump-reachability checks
that need stamped geometry come in Plan 2). Returns a pass count so `GenSmoke` can print `n/n`.

- [ ] **Step 1: Write the validator file**

```angelscript
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
```

- [ ] **Step 2: Verify a generated layout passes and a broken one fails (`run_code_test`)**

```angelscript
FRaidGenConfig Cfg;
FRaidLayout Good = RaidGen::Generate(12345, Cfg);
FRaidValidationResult RG = RaidValidate::Validate(Good, Cfg);

// Break it: shove the extraction next to the drop (fails separation).
FRaidLayout Bad = RaidGen::Generate(12345, Cfg);
Bad.Extraction.Center = Bad.Drop.Center;
FRaidValidationResult RB = RaidValidate::Validate(Bad, Cfg);

Print(f"[T3] goodOk={RG.bOk} ({RG.PassCount}/{RG.Total}) badOk={RB.bOk} firstFail={RB.FirstFail}", 8.0);
```

Expected: `[T3] goodOk=true (10/10) badOk=false firstFail=drop-extract-separation`.
(If `goodOk=false`, read `RG.FirstFail` — most likely `cover-count` for an unlucky seed; that is a
*real* reroll trigger, handled in Task 4. For the test, if seed 12345 fails cover-count, change the
test seed to one that passes, e.g. try 1, 2, 3 — note the chosen seed in the commit.)

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLayoutValidator.as
git commit -m "feat(procgen): pure-geometric layout validation battery

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Validated generation with deterministic reroll + safe fallback

**Files:**
- Modify: `RogueSmoke/Script/Generation/RaidLevelGenerator.as` (append to the `RaidGen` namespace)

- [ ] **Step 1: Add `GenerateValidated` + `BuildSafeFallback` inside the `RaidGen` namespace**

Add these functions inside `namespace RaidGen { ... }`, after `LayoutsEqual`:

```angelscript
    // Roll until valid (deterministic salt advance), else fall back to a known-good layout so a run
    // never softlocks (design spec §9). Same (Seed, Cfg) -> identical result, including the reroll.
    FRaidLayout GenerateValidated(int Seed, const FRaidGenConfig& Cfg, int MaxRetries = 8)
    {
        for (int i = 0; i <= MaxRetries; i++)
        {
            FRaidLayout L = Generate(Seed + i * 7919, Cfg);   // 7919 prime: deterministic re-roll salt
            FRaidValidationResult Res = RaidValidate::Validate(L, Cfg);
            if (Res.bOk)
            {
                L.Seed = Seed;        // keep the caller's seed as the layout's identity
                L.bValid = true;
                return L;
            }
        }
        FRaidLayout Safe = BuildSafeFallback(Cfg);
        Safe.Seed = Seed;
        Safe.bValid = true;
        return Safe;
    }

    // A hand-built layout guaranteed to pass Validate for the default config — the never-softlock net.
    FRaidLayout BuildSafeFallback(const FRaidGenConfig& Cfg)
    {
        FRaidLayout L;
        L.HalfExtent = Cfg.HalfExtent;
        float In = Cfg.HalfExtent - Cfg.BoundaryMargin;

        L.Drop = MakeAnchorSite(ERaidSiteType::Drop, ERaidSlotType::Entrance, FVector(-In, 0.0, 0.0));
        L.Extraction = MakeAnchorSite(ERaidSiteType::Extraction, ERaidSlotType::Exit, FVector(In, 0.0, 0.0));

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
            P.Z = 400.0;
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
        return L;
    }
```

- [ ] **Step 2: Verify validated generation + reroll determinism + fallback (`run_code_test`)**

```angelscript
FRaidGenConfig Cfg;
FRaidLayout V1 = RaidGen::GenerateValidated(12345, Cfg);
FRaidLayout V2 = RaidGen::GenerateValidated(12345, Cfg);
FRaidValidationResult RV = RaidValidate::Validate(V1, Cfg);
bool repro = RaidGen::LayoutsEqual(V1, V2);

// Fallback sanity: the safe layout must itself validate.
FRaidLayout Safe = RaidGen::BuildSafeFallback(Cfg);
FRaidValidationResult RS = RaidValidate::Validate(Safe, Cfg);

Print(f"[T4] valid={V1.bValid} ok={RV.bOk} repro={repro} fallbackOk={RS.bOk} ({RS.PassCount}/{RS.Total})", 8.0);
```

Expected: `[T4] valid=true ok=true repro=true fallbackOk=true (10/10)`.

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLevelGenerator.as
git commit -m "feat(procgen): validated generation with deterministic reroll + safe fallback

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: The `GenSmoke` headless exec

**Files:**
- Modify: `RogueSmoke/Script/Player/RaidPlayerController.as` (add one exec, mirroring `MoveSmoke`)

- [ ] **Step 1: Read the existing `MoveSmoke` exec for the house pattern**

Open `RogueSmoke/Script/Player/RaidPlayerController.as` around line 717 (`UFUNCTION(Exec) void MoveSmoke()`).
Note: smoke execs are `UFUNCTION(Exec)` on this class and print `[Tag] RESULT pass/total`. `GenSmoke`
has **no pawn dependency** (it tests pure functions), so it needs no retry-poll — it runs immediately.

- [ ] **Step 2: Add the `GenSmoke` exec**

Add this method to `class ARaidPlayerController` (anywhere among the other `UFUNCTION(Exec)` methods,
e.g. right after `MoveSmoke`):

```angelscript
    // Headless determinism + validation gate for procedural generation (procgen Plan 1). Pure: no
    // pawn/world needed, so it runs immediately under -ExecCmds. SmokeTest.ps1 greps the RESULT line.
    UFUNCTION(Exec)
    void GenSmoke()
    {
        FRaidGenConfig Cfg;
        int Pass = 0;
        int Total = 0;

        // 1. Same seed -> identical layout.
        Total += 1;
        FRaidLayout A = RaidGen::Generate(12345, Cfg);
        FRaidLayout B = RaidGen::Generate(12345, Cfg);
        if (RaidGen::LayoutsEqual(A, B)) Pass += 1;
        else Print("[GenSmoke] FAIL 1: same seed diverged", 12.0);

        // 2. Different seed -> different layout.
        Total += 1;
        FRaidLayout C = RaidGen::Generate(99999, Cfg);
        if (!RaidGen::LayoutsEqual(A, C)) Pass += 1;
        else Print("[GenSmoke] FAIL 2: different seeds identical", 12.0);

        // 3. Validated layout passes the full battery.
        Total += 1;
        FRaidLayout V = RaidGen::GenerateValidated(12345, Cfg);
        FRaidValidationResult RV = RaidValidate::Validate(V, Cfg);
        if (V.bValid && RV.bOk) Pass += 1;
        else Print(f"[GenSmoke] FAIL 3: validated invalid ({RV.PassCount}/{RV.Total} {RV.FirstFail})", 12.0);

        // 4. Validated generation is reproducible (reroll determinism).
        Total += 1;
        FRaidLayout V2 = RaidGen::GenerateValidated(12345, Cfg);
        if (RaidGen::LayoutsEqual(V, V2)) Pass += 1;
        else Print("[GenSmoke] FAIL 4: GenerateValidated not reproducible", 12.0);

        // 5. The safe fallback itself validates (never-softlock net).
        Total += 1;
        FRaidLayout Safe = RaidGen::BuildSafeFallback(Cfg);
        FRaidValidationResult RS = RaidValidate::Validate(Safe, Cfg);
        if (RS.bOk) Pass += 1;
        else Print(f"[GenSmoke] FAIL 5: fallback invalid ({RS.PassCount}/{RS.Total} {RS.FirstFail})", 12.0);

        Print(f"[GenSmoke] RESULT {Pass}/{Total}", 15.0);
    }
```

- [ ] **Step 3: Verify the file compiles (`run_code_test` or as-review)**

Run the as-helper `run_code_test` tool with body `Print("[T5] compile ok", 3.0);` — it loads/compiles
all scripts including the edited controller; expected: no compile error mentioning
`RaidPlayerController.as`. (Alternatively run `mcp__plugin_ue-as__review_file` on the controller.)

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as
git commit -m "test(procgen): GenSmoke headless determinism+validation exec

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: Wire the headless regression case + run it

**Files:**
- Modify: `Tools/SmokeTest.ps1` (add one case to the `$Cases` array)

- [ ] **Step 1: Add the `ProcGenFoundation` case**

In `Tools/SmokeTest.ps1`, add this entry to the `$Cases = @( ... )` array (e.g. right after the
`RaidArena` case near line 35). It reuses the existing RaidArena map purely as an exec host:

```powershell
    @{ Name = "ProcGenFoundation"; Map = "/Game/Levels/RaidArena"; Expect = "[GenSmoke] RESULT 5/5"; Exec = "GenSmoke"; Window = 25 }
```

- [ ] **Step 2: Run the smoke test for just this case**

Run (PowerShell):

```powershell
& 'C:\Users\btblu\Documents\RogueSmoke\Tools\SmokeTest.ps1'
```

Expected: the printed table shows `ProcGenFoundation ... PASS`, and the run ends with
`SMOKE TEST PASSED`. If `ProcGenFoundation` FAILs, open `"$env:TEMP\rs_smoke\ProcGenFoundation.log"`
and grep for `[GenSmoke]` — a `FAIL n:` line names the failing check; fix the corresponding task.
(Note: a clean editor build should already exist; this plan adds no C++, so no rebuild is required —
but if the editor has never compiled the new scripts, the first headless boot compiles them.)

- [ ] **Step 3: Commit**

```bash
git add Tools/SmokeTest.ps1
git commit -m "test(procgen): SmokeTest case for ProcGen foundation (GenSmoke 5/5)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (this plan = Phase 0 + pure-geometric §9 subset):**
- §5 data contract (`FRaidLayout`) → Task 1. ✅
- §8 grammar layers A/B (archetype + typed slots with intensity caps) → encoded in Tasks 1-2 (slots,
  caps); module/scatter (C/D) partially (scatter = cover) ✅; Theme (E) deferred to Plan 2 (cosmetic).
- §7 determinism (seeded FRandomStream, server-owned) → Tasks 2,4 + GenSmoke determinism checks. ✅
- §9 invariants: the world-free subset (separation, one-core/one-hold, offset, min-high-ground,
  in-bounds, cover count/separation) → Task 3. Navmesh / jump-reachability / escape-proof-trajectory
  invariants **explicitly deferred to Plan 2** (need stamped geometry) — noted in Task 3 header. ✅
- Reroll + safe fallback (§9 never-softlock) → Task 4. ✅
- Determinism harness (`GenSmoke` + SmokeTest) → Tasks 5-6. ✅

**Deferred (named, not silently dropped):** geometry stamping + ISM seam, enabling/wiring PCG cosmetic
skin, world-dependent validation (navmesh/jump-reachability), the multi-objective manager, and the
raid-director escalation — these are **Plan 2 and Plan 3**, listed in the spec §13 roadmap.

**Placeholder scan:** no TBD/TODO; every code step has complete code; every test step has an exact
command + expected output line. ✅

**Type consistency:** `FRaidLayout`/`FRaidSite`/`FRaidNode`/`FRaidCover`/`FRaidGenConfig`/
`FRaidValidationResult` and the enums are defined once (Tasks 1,2,3) and used with identical
member/field names throughout (`.Nodes`, `.Cover`, `.Center`, `.IntensityCap`, `.bValid`, `.PassCount`,
`.FirstFail`). `RaidGen::{Generate, GenerateValidated, LayoutsEqual, BuildSafeFallback}` and
`RaidValidate::Validate` signatures match every call site (run_code_test snippets + GenSmoke). ✅

**Risk note for the executor:** the one place reality may bite is the exact `run_code_test` invocation
surface and whether seed 12345's cover roll lands in `[CoverMin,CoverMax]` (Task 3 Step 2 tells you how
to pick another seed if not). Neither affects the shipped code — `GenerateValidated` + the SmokeTest
RESULT line is the real gate, and it tolerates any seed via reroll/fallback.

## Parallelization

Tasks 1→4 are a single tightly-coupled pure-logic file group (data model → generator → validator →
reroll) and should be done **sequentially by one worker** — splitting them across agents would just
trade coordination cost for no isolation benefit. Tasks 5→6 depend on 1→4. All verification is
`run_code_test` (script compile/run) + one headless `SmokeTest.ps1` boot — **no interactive editor /
MCP / PIE**, so nothing here contends for the single editor (CLAUDE.md "one editor" rule). The real
parallelization payoff arrives in Plan 2 (independent module meshes + the PCG graph + C++ seam).
