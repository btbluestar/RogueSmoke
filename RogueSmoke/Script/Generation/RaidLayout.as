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
    FRaidTerrain Terrain;

    UPROPERTY()
    FRaidSite Drop;

    UPROPERTY()
    TArray<FRaidSite> MainSites;

    UPROPERTY()
    FRaidSite Extraction;
}
