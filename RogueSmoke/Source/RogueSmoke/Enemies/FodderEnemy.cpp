// FodderEnemy.cpp

#include "Enemies/FodderEnemy.h"
#include "Combat/HealthComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

AFodderEnemy::AFodderEnemy()
{
	// Cheap visible body: a single static-mesh sphere as the root. No skeletal mesh, no
	// CharacterMovementComponent, no AIController — the things that make 150 agents expensive.
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	SetRootComponent(Body);
	Body->SetMobility(EComponentMobility::Movable);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		Body->SetStaticMesh(SphereMesh.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Mat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (Mat.Succeeded())
	{
		Body->SetMaterial(0, Mat.Object);
	}
	Body->SetRelativeScale3D(FVector(0.5f)); // small — reads as a swarm unit, not an elite

	// Query-only collision: block the hitscan Visibility channel (so FireHitscan can kill it),
	// but only overlap pawns/other fodder so the swarm packs together instead of bumping the hero.
	Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Body->SetCollisionObjectType(ECC_WorldDynamic);
	Body->SetCollisionResponseToAllChannels(ECR_Overlap);
	Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);

	// Fodder is NOT a raid-objective target: clearing fodder must not open extraction; only
	// elites gate that (see CombatSubsystem::GetEliteCount + RaidObjective). Base default is true.
	bCountsAsObjectiveTarget = false;

	// Cheap to kill. ResetHealth() on pool Activate() restores to this.
	if (Health != nullptr)
	{
		Health->MaxHealth = 20.f;
		Health->Health = 20.f;
	}
}

void AFodderEnemy::Tick(float DeltaSeconds)
{
	// Base Tick steers toward a taunt point while a pull is active (the synergy SETUP).
	Super::Tick(DeltaSeconds);

	if (!HasAuthority() || GetWorld() == nullptr)
	{
		return;
	}

	// While a pull owns our movement, let it; otherwise advance the swarm toward the players.
	const bool bBeingPulled = GetWorld()->GetTimeSeconds() < PullExpiresAtSeconds;
	if (!bBeingPulled)
	{
		SteerTowardNearestPlayer(DeltaSeconds);
	}
}

void AFodderEnemy::SteerTowardNearestPlayer(float DeltaSeconds)
{
	const AActor* Target = FindNearestPlayerPawn();
	if (Target == nullptr)
	{
		return;
	}

	const FVector Mine = GetActorLocation();
	FVector ToTarget = Target->GetActorLocation() - Mine;
	ToTarget.Z = 0.f; // stay on the ground plane; no flying

	const float Dist = ToTarget.Size();
	if (Dist <= StopDistance)
	{
		return;
	}

	const FVector Dir = ToTarget / Dist;
	// bSweep=false: cheap, and avoids the stuck-on-floor-penetration trap the elite pull stub hit.
	SetActorLocation(Mine + Dir * MoveSpeed * DeltaSeconds, /*bSweep=*/false);
	SetActorRotation(Dir.Rotation());
}

AActor* AFodderEnemy::FindNearestPlayerPawn() const
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	AActor* Nearest = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();
	const FVector Mine = GetActorLocation();

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (Pawn == nullptr)
		{
			continue;
		}
		const float DistSq = FVector::DistSquared(Pawn->GetActorLocation(), Mine);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Nearest = Pawn;
		}
	}
	return Nearest;
}
