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
class UMaterialInterface;

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

    /**
     * Like StampBoxes, but all boxes in this batch share one tint (blockout color-coding). Each call
     * makes its own ISM + dynamic instance of M_Graybox so the function color reads regardless of
     * lighting. Call once per element type (floor / walls / cover / platform / landmark markers).
     */
    UFUNCTION(BlueprintCallable, Category="Generation")
    int32 StampBoxesColored(const TArray<FVector>& Centers, const TArray<FVector>& WorldSizes, FLinearColor Color);

    /** Tear down everything stamped (ISM components + holder) — call before a re-stamp. */
    UFUNCTION(BlueprintCallable, Category="Generation")
    void ClearStamps();

private:
    UStaticMesh* GetCubeMesh();
    UMaterialInterface* GetGrayboxMaterial();
    AActor* EnsureHolder();

    UPROPERTY()
    TObjectPtr<UStaticMesh> CubeMesh;

    UPROPERTY()
    TObjectPtr<UMaterialInterface> GrayboxMaterial;

    UPROPERTY()
    TObjectPtr<AActor> Holder;

    UPROPERTY()
    TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> Components;
};
