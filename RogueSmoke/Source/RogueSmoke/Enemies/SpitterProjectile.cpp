// SpitterProjectile.cpp

#include "Enemies/SpitterProjectile.h"
#include "Combat/CombatSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/World.h"

ASpitterProjectile::ASpitterProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicateMovement(true);

	// Self-contained visible glob (like the elites' cube): an engine sphere, no collision — the flight and
	// the hit are driven in Tick, so nothing else needs to block it.
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	SetRootComponent(Body);

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
	Body->SetRelativeScale3D(FVector(0.35f));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

void ASpitterProjectile::BeginPlay()
{
	Super::BeginPlay();
}

void ASpitterProjectile::Launch(FVector ImpactPoint, float InSpeed, float InDamage, float InSplash, AActor* InShooter)
{
	StartLoc = GetActorLocation();
	TargetLoc = ImpactPoint;
	Speed = FMath::Max(1.f, InSpeed);
	HitDamage = InDamage;
	Splash = InSplash;
	Shooter = InShooter;
	TravelTime = FMath::Max(0.15f, FVector::Dist(StartLoc, TargetLoc) / Speed);
	Elapsed = 0.f;
	bLaunched = true;
}

void ASpitterProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Server flies it; clients receive the position via replicated movement.
	if (!HasAuthority() || !bLaunched || bResolved)
	{
		return;
	}

	Elapsed += DeltaSeconds;
	const float Alpha = FMath::Clamp(Elapsed / TravelTime, 0.f, 1.f);
	FVector Pos = FMath::Lerp(StartLoc, TargetLoc, Alpha);
	Pos.Z += FMath::Sin(Alpha * PI) * ArcHeight; // cosmetic lob
	SetActorLocation(Pos, /*bSweep=*/false);

	if (Alpha >= 1.f)
	{
		Resolve();
	}
}

void ASpitterProjectile::Resolve()
{
	bResolved = true;
	if (UCombatSubsystem* Combat = GetWorld() ? GetWorld()->GetSubsystem<UCombatSubsystem>() : nullptr)
	{
		Combat->ApplyRadialDamageToPlayers(GetActorLocation(), Splash, HitDamage, Shooter.Get());
	}
	Destroy();
}
