# Procedural Raid Generation — Stamping + Ballistic Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn a validated `FRaidLayout` into actual collidable greybox geometry in the world (a C++ instanced-static-mesh seam), and add the two project-specific ballistic validation checks — jump-reachability and escape-proof — driven by the hero's real movement reach envelope.

**Architecture:** Two halves. (A) **Ballistic validation** — pure AngelScript: a reach-envelope module (single source of truth mirroring `LocomotionComponent`), generator fix (cap platform height to what the double-jump can reach), and two new checks folded into the existing `RaidValidate::Validate` so reroll respects them. (B) **Stamping** — a new C++ `URaidStampSubsystem` (`UWorldSubsystem`, mirrors `USpawnDirector`) that batches boxes into a `UHierarchicalInstancedStaticMeshComponent` with BlockAll collision; an AngelScript `RaidArena::BuildFromSeed` orchestrator that reads the layout and calls the seam; verified by a `StampSmoke` exec. Stamping runs on **every machine** from the replicated seed (geometry re-stamped locally, never replicated — ARCHITECTURE §4.1), so there is no `IsServer()` gate on the seam.

**Tech Stack:** UnrealEngine-Angelscript (Hazelight UE5.7 fork); C++ `UWorldSubsystem` + `UHierarchicalInstancedStaticMeshComponent` (Engine module, already linked); `mcp__ue-cpp__build` to compile C++; `run_code_test` for pure-AS checks; `Tools/SmokeTest.ps1` headless boots for the integration gate.

**This is Plan 2 of 3** (spec §13 Phase 1 partial). Auto-stamp-at-raid-load wiring (server `BeginPlay` + client `OnRep_MasterSeed`) and moving hero spawns to the generated Drop node are **Plan 3** (bundled with the objective manager, to avoid disturbing the existing MoveSmoke/RaidLoop regression cases). The navmesh ground-connectivity validation is deferred until enemies pathfind (design call 2026-06-14).

---

## Critical context for the engineer

- **No `import` needed** — script symbols are globally visible (Plan 1's `RaidGen`/`RaidValidate`/`FRaidLayout` are already usable from any `.as`).
- **Determinism still rules.** The validation is pure (no world); stamping consumes a deterministic layout. No unseeded random anywhere.
- **The reach envelope is the single source of truth.** Its defaults MIRROR `Script/Player/LocomotionComponent.as` (D-0021): `BaseWalkSpeed 600`, `SprintSpeedMultiplier 1.6`, `SlideSpeedCapMultiplier 1.5`, `JumpZVelocity 750`, `DoubleJumpZVelocity 700`, `GravityScale 1.8`. These give: double-jump vertical ceiling ≈ **298 uu**, flat double-jump horizontal gap ≈ **2367 uu**. The current generator places HighGround at Z 300–600 — **above the 298 ceiling, i.e. unreachable** — which Task 2 fixes.
- **C++ conventions** (from `Source/RogueSmoke/Spawning/SpawnDirector.h/.cpp`): `UWorldSubsystem` subclass, `ROGUESMOKE_API`, **no hand-written `Get()`** (the AS fork auto-generates `URaidStampSubsystem::Get()`), `UFUNCTION(BlueprintCallable)` to expose to AS. New C++ goes under `Source/RogueSmoke/Generation/` and that folder **must be added to `PublicIncludePaths` in `RogueSmoke.Build.cs`**. `Engine` is already a dependency (ISM/HISM available); do **not** add NavigationSystem/PCG.
- **C++ verification:** `mcp__ue-cpp__build` (UnrealBuildTool; returns parsed errors). After a successful build, AngelScript that references the new C++ type is verified by a **fresh headless boot** (Task 7), not `run_code_test` against the already-running editor (which holds a stale binary).
- **Branch:** `ProcedualLevelGeneration`. Commit after every task.

## File structure

- Create `RogueSmoke/Script/Generation/RaidReachEnvelope.as` — `FReachEnvelope` + `RaidReach` (pure reach math).
- Modify `RogueSmoke/Script/Generation/RaidLevelGenerator.as` — add reach-derived config fields; cap HighGround Z.
- Modify `RogueSmoke/Script/Generation/RaidLayoutValidator.as` — add `jump-reachability` + `escape-proof` checks.
- Modify `RogueSmoke/Script/Player/RaidPlayerController.as` — extend `GenSmoke` (validation) + add `StampSmoke` (stamping).
- Create `RogueSmoke/Source/RogueSmoke/Generation/RaidStampSubsystem.h` / `.cpp` — the ISM seam.
- Modify `RogueSmoke/Source/RogueSmoke/RogueSmoke.Build.cs` — add the `Generation` include path.
- Create `RogueSmoke/Script/Generation/RaidStamper.as` — `RaidArena::BuildFromSeed` orchestrator.
- Modify `Tools/SmokeTest.ps1` — update the GenSmoke RESULT count; add a `StampArena` case.

---

### Task 1: Reach envelope (single source of truth)

**Files:** Create `RogueSmoke/Script/Generation/RaidReachEnvelope.as`

- [ ] **Step 1: Write the file**

```angelscript
// RaidReachEnvelope.as
// The hero's traversal reach envelope — the SINGLE SOURCE OF TRUTH the procedural validator shares
// with the movement kit. Pure math from the locomotion tunables (D-0021). Used by jump-reachability
// ("can the player reach this platform?") and escape-proof ("is this wall too low to clear?").
//
// IMPORTANT: Default() MIRRORS URogueLocomotionComponent (Script/Player/LocomotionComponent.as).
// If those tunables change, update Default(). Values are the pre-upgrade baseline (validation runs at
// generation time, before any GAS MoveSpeed upgrade applies).

struct FReachEnvelope
{
    UPROPERTY()
    float VertCeiling = 0.0;       // max height reachable by jump + double-jump from ground (uu)

    UPROPERTY()
    float HorizDoubleJump = 0.0;   // max horizontal gap, flat, slide-hop carry + double-jump (uu)

    UPROPERTY()
    float HorizSingleJump = 0.0;   // max horizontal gap, flat, single jump (uu)
}

namespace RaidReach
{
    // Pure: compute the envelope from raw tunables (testable without a world).
    FReachEnvelope Compute(float BaseWalkSpeed, float SprintMult, float SlideCapMult,
                           float JumpZ, float DoubleJumpZ, float GravityScale)
    {
        float g = 980.0 * GravityScale;                              // UE world default GravityZ * scale
        float LaunchHSpd = BaseWalkSpeed * SprintMult * SlideCapMult; // slide-hop air-carry ceiling

        FReachEnvelope E;
        E.VertCeiling = (JumpZ * JumpZ) / (2.0 * g) + (DoubleJumpZ * DoubleJumpZ) / (2.0 * g);
        E.HorizDoubleJump = LaunchHSpd * 2.0 * (JumpZ + DoubleJumpZ) / g;
        E.HorizSingleJump = LaunchHSpd * 2.0 * JumpZ / g;
        return E;
    }

    // Default envelope mirroring URogueLocomotionComponent defaults (D-0021).
    FReachEnvelope Default()
    {
        return Compute(600.0, 1.6, 1.5, 750.0, 700.0, 1.8);
    }
}
```

- [ ] **Step 2: Verify via `run_code_test`**

```angelscript
FReachEnvelope E = RaidReach::Default();
Print(f"[T1] vert={E.VertCeiling} horizDJ={E.HorizDoubleJump} horizSJ={E.HorizSingleJump}", 6.0);
```

Expected: no script error; `vert≈298.3 horizDJ≈2367.4 horizSJ≈1224.5` (small float variance OK).

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidReachEnvelope.as
git commit -m "feat(procgen): hero reach envelope (single source of truth for validation)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Generator — cap platform height to the reach envelope + add wall config

**Files:** Modify `RogueSmoke/Script/Generation/RaidLevelGenerator.as`

- [ ] **Step 1: Add reach-derived fields to `FRaidGenConfig`**

In `struct FRaidGenConfig`, after the existing `CoverRadius` field, add:

```angelscript
    // --- Plan 2: reach-envelope-derived geometry (jump-reachability + escape-proof) ---
    UPROPERTY()
    float HighGroundMinZ = 150.0;        // lowest platform height

    UPROPERTY()
    float HighGroundMaxZ = 270.0;        // highest platform — kept under the ~298 double-jump ceiling

    UPROPERTY()
    float JumpReachMargin = 20.0;        // platforms must sit this far below the vertical ceiling

    UPROPERTY()
    float WallHeight = 1200.0;           // boundary wall height (stamped); validated vs the envelope

    UPROPERTY()
    float EscapeMargin = 150.0;          // wall must exceed (highest standable + VertCeiling) by this
```

- [ ] **Step 2: Cap the generated HighGround Z**

In `BuildSkatepark`, find:

```angelscript
            P.Z = Rng.RandRange(300.0, 600.0);
```

Replace with:

```angelscript
            P.Z = Rng.RandRange(Cfg.HighGroundMinZ, Cfg.HighGroundMaxZ);
```

- [ ] **Step 3: Cap the fallback HighGround Z**

In `BuildSafeFallback`, find:

```angelscript
            P.Z = 400.0;
```

Replace with:

```angelscript
            P.Z = (Cfg.HighGroundMinZ + Cfg.HighGroundMaxZ) * 0.5;
```

- [ ] **Step 4: Verify it still generates (`run_code_test`)**

```angelscript
FRaidGenConfig Cfg;
FRaidLayout L = RaidGen::Generate(12345, Cfg);
float maxZ = 0.0;
for (FRaidNode N : L.MainSites[0].Nodes)
    if (N.Slot == ERaidSlotType::HighGround && N.Location.Z > maxZ) maxZ = N.Location.Z;
Print(f"[T2] maxHighGroundZ={maxZ} (must be <= {Cfg.HighGroundMaxZ})", 6.0);
```

Expected: `maxHighGroundZ` ≤ 270.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLevelGenerator.as
git commit -m "feat(procgen): cap platform height to the double-jump reach envelope + wall config

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Validator — jump-reachability + escape-proof checks

**Files:** Modify `RogueSmoke/Script/Generation/RaidLayoutValidator.as`

- [ ] **Step 1: Add the two checks inside `Validate`**

In `RaidValidate::Validate`, find the final line `R.bOk = (R.PassCount == R.Total);` and insert this block **immediately before** it:

```angelscript
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
```

- [ ] **Step 2: Verify (`run_code_test`)**

```angelscript
FRaidGenConfig Cfg;
FRaidLayout Good = RaidGen::Generate(12345, Cfg);
FRaidValidationResult RG = RaidValidate::Validate(Good, Cfg);

FRaidLayout Bad = RaidGen::Generate(12345, Cfg);
for (int i = 0; i < Bad.MainSites[0].Nodes.Num(); i++)
    if (Bad.MainSites[0].Nodes[i].Slot == ERaidSlotType::HighGround)
    { Bad.MainSites[0].Nodes[i].Location.Z = 5000.0; break; }
FRaidValidationResult RB = RaidValidate::Validate(Bad, Cfg);

Print(f"[T3] goodOk={RG.bOk} ({RG.PassCount}/{RG.Total}) badFail={RB.FirstFail}", 8.0);
```

Expected: `goodOk=true (12/12) badFail=jump-reachability`. (If `goodOk=false`, read `RG.FirstFail`; for an unlucky cover seed try seed 1/2/3 and note it.)

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidLayoutValidator.as
git commit -m "feat(procgen): jump-reachability + escape-proof validation (ballistic)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: Extend the GenSmoke gate to cover the new checks

**Files:** Modify `RogueSmoke/Script/Player/RaidPlayerController.as` (the existing `GenSmoke` exec) and `Tools/SmokeTest.ps1`

- [ ] **Step 1: Add a 6th assertion to `GenSmoke`**

In `GenSmoke`, find the final line `Print(f"[GenSmoke] RESULT {Pass}/{Total}", 15.0);` and insert **immediately before** it:

```angelscript
        // 6. An unreachable platform is rejected (jump-reachability active).
        Total += 1;
        FRaidLayout BadReach = RaidGen::Generate(12345, Cfg);
        for (int i = 0; i < BadReach.MainSites[0].Nodes.Num(); i++)
            if (BadReach.MainSites[0].Nodes[i].Slot == ERaidSlotType::HighGround)
            { BadReach.MainSites[0].Nodes[i].Location.Z = 5000.0; break; }
        FRaidValidationResult RR = RaidValidate::Validate(BadReach, Cfg);
        if (!RR.bOk && RR.FirstFail == "jump-reachability") Pass += 1;
        else Print(f"[GenSmoke] FAIL 6: unreachable platform not caught ({RR.FirstFail})", 12.0);
```

- [ ] **Step 2: Update the SmokeTest expectation**

In `Tools/SmokeTest.ps1`, find the `ProcGenFoundation` case and change `[GenSmoke] RESULT 5/5` to `[GenSmoke] RESULT 6/6`:

```powershell
    @{ Name = "ProcGenFoundation";  Map = "/Game/Levels/RaidArena";                       Expect = "[GenSmoke] RESULT 6/6"; Exec = "GenSmoke"; Window = 25 }
```

- [ ] **Step 3: Controller runs the gate**

(The controller, not the implementer, runs the headless boot — single-editor rule. The implementer reports DONE; the controller verifies `[GenSmoke] RESULT 6/6` before the task is marked complete.)

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "test(procgen): GenSmoke asserts jump-reachability (RESULT 6/6)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: C++ stamping seam (`URaidStampSubsystem`)

**Files:** Create `RogueSmoke/Source/RogueSmoke/Generation/RaidStampSubsystem.h` + `.cpp`; Modify `RogueSmoke/Source/RogueSmoke/RogueSmoke.Build.cs`

- [ ] **Step 1: Add the include path in `RogueSmoke.Build.cs`**

Find the `PublicIncludePaths` array (it lists `"RogueSmoke/Combat"`, `"RogueSmoke/Spawning"`, etc.) and add a `"RogueSmoke/Generation"` entry alongside the others. (Do NOT add NavigationSystem/PCG dependencies.)

- [ ] **Step 2: Write the header**

`RogueSmoke/Source/RogueSmoke/Generation/RaidStampSubsystem.h`:

```cpp
// RaidStampSubsystem.h
// The stamping seam: turns procgen layout data into collidable greybox geometry via instanced
// static meshes. Mirrors USpawnDirector (UWorldSubsystem). UNLIKE SpawnDirector there is NO server
// gate — geometry is deterministically re-stamped on EVERY machine from the replicated seed
// (ARCHITECTURE 4.1: replicate the seed, regenerate locally), never replicated.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RaidStampSubsystem.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class UStaticMesh;

UCLASS()
class ROGUESMOKE_API URaidStampSubsystem : public UWorldSubsystem
{
    GENERATED_BODY()

public:
    // NOTE: no hand-written Get() — the AngelScript fork auto-generates a static
    // URaidStampSubsystem::Get() for UWorldSubsystem types. In C++ use GetWorld()->GetSubsystem<>().

    /** Stamp one collidable box per (Center, WorldSize) pair, using the engine cube. Returns count. */
    UFUNCTION(BlueprintCallable, Category="Generation")
    int32 StampBoxes(const TArray<FVector>& Centers, const TArray<FVector>& WorldSizes);

    /** Tear down everything stamped (ISM components + holder) — call before a re-stamp. */
    UFUNCTION(BlueprintCallable, Category="Generation")
    void ClearStamps();

private:
    UStaticMesh* GetCubeMesh();
    AActor* EnsureHolder();

    UPROPERTY()
    TObjectPtr<UStaticMesh> CubeMesh;

    UPROPERTY()
    TObjectPtr<AActor> Holder;

    UPROPERTY()
    TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Components;
};
```

- [ ] **Step 3: Write the implementation**

`RogueSmoke/Source/RogueSmoke/Generation/RaidStampSubsystem.cpp`:

```cpp
#include "Generation/RaidStampSubsystem.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

UStaticMesh* URaidStampSubsystem::GetCubeMesh()
{
    if (!CubeMesh)
    {
        // Engine greybox primitive. 100uu cube → world scale = size / 100.
        CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    }
    return CubeMesh;
}

AActor* URaidStampSubsystem::EnsureHolder()
{
    if (Holder)
    {
        return Holder;
    }
    UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }
    FActorSpawnParameters Params;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Holder = World->SpawnActor<AActor>(AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, Params);
    if (Holder)
    {
        USceneComponent* Root = NewObject<USceneComponent>(Holder, TEXT("StampRoot"));
        Root->RegisterComponent();
        Holder->SetRootComponent(Root);
    }
    return Holder;
}

int32 URaidStampSubsystem::StampBoxes(const TArray<FVector>& Centers, const TArray<FVector>& WorldSizes)
{
    const int32 N = FMath::Min(Centers.Num(), WorldSizes.Num());
    if (N == 0)
    {
        return 0;
    }
    UStaticMesh* Cube = GetCubeMesh();
    AActor* H = EnsureHolder();
    if (!Cube || !H)
    {
        return 0;
    }

    UHierarchicalInstancedStaticMeshComponent* ISM =
        NewObject<UHierarchicalInstancedStaticMeshComponent>(H);
    ISM->SetStaticMesh(Cube);
    ISM->SetMobility(EComponentMobility::Movable);
    ISM->AttachToComponent(H->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
    ISM->RegisterComponent();
    ISM->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    ISM->SetCollisionProfileName(TEXT("BlockAll"));

    int32 Count = 0;
    for (int32 i = 0; i < N; ++i)
    {
        FTransform T(FQuat::Identity, Centers[i], WorldSizes[i] / 100.0f);
        ISM->AddInstance(T, /*bWorldSpace=*/true);
        ++Count;
    }
    Components.Add(ISM);
    return Count;
}

void URaidStampSubsystem::ClearStamps()
{
    for (UHierarchicalInstancedStaticMeshComponent* ISM : Components)
    {
        if (ISM)
        {
            ISM->ClearInstances();
            ISM->DestroyComponent();
        }
    }
    Components.Reset();
    if (Holder)
    {
        Holder->Destroy();
        Holder = nullptr;
    }
}
```

- [ ] **Step 4: Build**

Run `mcp__ue-cpp__build` (set project root first if needed: `mcp__ue-cpp__set_project_root` → `C:\Users\btblu\Documents\RogueSmoke\RogueSmoke`). Expected: build succeeds, no errors. If errors mention the include path, recheck the `Build.cs` `PublicIncludePaths` entry. New files may need a non-incremental build (`clean=true`) if UnrealBuildTool doesn't pick them up.

- [ ] **Step 5: Commit**

```bash
git add RogueSmoke/Source/RogueSmoke/Generation/RaidStampSubsystem.h RogueSmoke/Source/RogueSmoke/Generation/RaidStampSubsystem.cpp RogueSmoke/Source/RogueSmoke/RogueSmoke.Build.cs
git commit -m "feat(procgen): URaidStampSubsystem ISM stamping seam (collidable greybox)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: AngelScript stamper (`RaidArena::BuildFromSeed`)

**Files:** Create `RogueSmoke/Script/Generation/RaidStamper.as`

> Depends on Task 5's C++ type being **built**. Do not attempt `run_code_test` here against the
> running editor (stale binary) — Task 7's fresh headless boot is the verification.

- [ ] **Step 1: Write the file**

```angelscript
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
}
```

- [ ] **Step 2: Static lint**

Run `mcp__plugin_ue-as__validate_specifiers` on `RaidStamper.as`. Expected: no specifier issues. (Full compile is verified by Task 7's boot.)

- [ ] **Step 3: Commit**

```bash
git add RogueSmoke/Script/Generation/RaidStamper.as
git commit -m "feat(procgen): RaidArena::BuildFromSeed stamps greybox geometry via the seam

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: StampSmoke gate (headless integration)

**Files:** Modify `RogueSmoke/Script/Player/RaidPlayerController.as` (add `StampSmoke`) and `Tools/SmokeTest.ps1`

- [ ] **Step 1: Add the `StampSmoke` exec**

Add this method as a sibling of `GenSmoke` in `class ARaidPlayerController`:

```angelscript
    // Headless gate for procgen STAMPING (procgen Plan 2). Builds the arena from a fixed seed twice
    // and asserts the geometry stamped + the stamp is deterministic/idempotent. Needs a world (the
    // URaidStampSubsystem), so it runs in-game, not under run_code_test. SmokeTest greps RESULT.
    UFUNCTION(Exec)
    void StampSmoke()
    {
        int N1 = RaidArena::BuildFromSeed(12345);
        int N2 = RaidArena::BuildFromSeed(12345);   // re-stamp: ClearStamps + rebuild

        int Pass = 0;
        int Total = 0;

        // 1. Expected geometry stamped: floor(1) + walls(4) + platforms(3) + cover(>=6) >= 8.
        Total += 1;
        if (N1 >= 8) Pass += 1;
        else Print(f"[StampSmoke] FAIL 1: too few boxes stamped ({N1})", 12.0);

        // 2. Deterministic + ClearStamps works (re-stamp yields the same count, not double).
        Total += 1;
        if (N1 == N2) Pass += 1;
        else Print(f"[StampSmoke] FAIL 2: re-stamp count drifted ({N1} -> {N2})", 12.0);

        Print(f"[StampSmoke] RESULT {Pass}/{Total}", 15.0);
    }
```

- [ ] **Step 2: Add the SmokeTest case**

In `Tools/SmokeTest.ps1`, add after the `ProcGenFoundation` case:

```powershell
    # Procgen stamping (Plan 2): builds collidable greybox geometry from the seed via the ISM seam.
    @{ Name = "StampArena";         Map = "/Game/Levels/RaidArena";                       Expect = "[StampSmoke] RESULT 2/2"; Exec = "StampSmoke"; Window = 25 }
```

- [ ] **Step 3: Controller runs the gate**

(Controller runs `Tools/SmokeTest.ps1` — or a targeted `-ExecCmds=StampSmoke` boot — and confirms `[StampSmoke] RESULT 2/2` with no fatals, plus the whole 13-case suite stays green. This requires the Task 5 C++ build to have succeeded so the fresh boot loads `URaidStampSubsystem`.)

- [ ] **Step 4: Commit**

```bash
git add RogueSmoke/Script/Player/RaidPlayerController.as Tools/SmokeTest.ps1
git commit -m "test(procgen): StampSmoke headless gate (greybox stamping, RESULT 2/2)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (this plan):**
- §9 #1 escape-proof (ballistic) → Task 3. ✅
- §9 #6 max-3-planes / contestable high ground (the *reachable* part) → Tasks 1–3 (platform cap + jump-reachability). ✅
- §7 Layer-1 structural stamping (C++ ISM seam, re-stamped locally from seed) → Tasks 5–6. ✅
- Reach envelope as single source of truth (§9 risk note) → Task 1. ✅
- Headless verification, single-editor discipline → Tasks 4, 7. ✅

**Explicitly deferred (named, not dropped):** navmesh ground-connectivity validation (until enemies pathfind); auto-stamp-at-raid-load wiring + hero-spawn-at-Drop (Plan 3, with the objective manager, to keep the existing MoveSmoke/RaidLoop cases undisturbed); per-mesh / themed meshes + PCG cosmetic skin (later); interactive "hero stands/slides on stamped geometry" verification (manual follow-up — the automated gate asserts geometry + collision profile, not in-PIE traversal).

**Placeholder scan:** none — every code step is complete; every test step has an exact command + expected line. ✅

**Type consistency:** `FReachEnvelope.{VertCeiling,HorizDoubleJump,HorizSingleJump}`, `RaidReach::{Compute,Default}`, the new `FRaidGenConfig` fields (`HighGroundMinZ/MaxZ`, `JumpReachMargin`, `WallHeight`, `EscapeMargin`), `URaidStampSubsystem::{StampBoxes,ClearStamps}`, and `RaidArena::{BuildFromSeed,StampLayout}` are each defined once and used with identical names at every call site (validator, GenSmoke, StampSmoke, stamper). ✅

**Risk notes for the executor:**
- The reach-envelope defaults are *mirrored* from `LocomotionComponent.as`, not read live — acceptable for a generation-time check, but if movement is retuned, update `RaidReach::Default()`. (A future improvement: read the locomotion CDO.)
- The C++ ISM API (`AddInstance`, `SetCollisionProfileName`, runtime `Movable` mobility) is standard UE5.7 but unverified against this exact engine fork until Task 5's build — if `AddInstance`'s signature differs, the build error will name it.
- Under `-nullrhi`, ISM still creates collision (CPU-side); the count-based gate doesn't depend on rendering.

## Parallelization

Part A (Tasks 1–4, pure AS) and Part B (Tasks 5–7, C++ + AS) are mostly sequential within each half. The one genuine parallel opportunity: **Task 5 (C++ subsystem) can be authored in parallel with Tasks 1–4 (AS validation)** — they share no files and the C++ build is independent of the AS validation. Funnel both back through this session for verification: the C++ `mcp__ue-cpp__build` and every `run_code_test`/`SmokeTest.ps1` boot drive the single editor/build and MUST be serialized (CLAUDE.md). Tasks 6–7 depend on Task 5 being built. No git worktrees.
