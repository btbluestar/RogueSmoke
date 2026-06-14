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
