// TelegraphZoneFX.cpp

#include "VFX/TelegraphZoneFX.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// The engine cylinder is 100uu across (radius 50): scale XY by R/50 to reach a world radius.
	constexpr float MeshRadius = 50.f;
	constexpr float DiscThickness = 0.02f;   // flattened to a 2uu disc
	constexpr float BurstSeconds = 0.35f;
}

ATelegraphZoneFX::ATelegraphZoneFX()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;          // ZONE mode default; SpawnLocalBurst turns it off pre-spawn
	SetReplicatingMovement(false); // static after spawn; the spawn transform is enough

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Outline = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Outline"));
	Fill = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Fill"));
	SetupDisc(Outline);
	SetupDisc(Fill);
	Fill->SetRelativeLocation(FVector(0.f, 0.f, 3.f)); // above the outline so both read
}

void ATelegraphZoneFX::SetupDisc(UStaticMeshComponent* Disc)
{
	Disc->SetupAttachment(GetRootComponent());
	Disc->SetRelativeLocation(FVector(0.f, 0.f, 2.f));
	Disc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Disc->SetCastShadow(false);
	Disc->SetMobility(EComponentMobility::Movable);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded())
	{
		Disc->SetStaticMesh(CylinderMesh.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RingMat(TEXT("/Game/VFX/M_TelegraphRing.M_TelegraphRing"));
	if (RingMat.Succeeded())
	{
		Disc->SetMaterial(0, RingMat.Object);
	}
}

void ATelegraphZoneFX::BeginPlay()
{
	Super::BeginPlay();

	OutlineMID = Outline->CreateDynamicMaterialInstance(0);
	FillMID = Fill->CreateDynamicMaterialInstance(0);

	if (bBurst)
	{
		Outline->SetVisibility(false);
		if (FillMID)
		{
			FillMID->SetVectorParameterValue(FName(TEXT("Color")), BurstColor);
		}
	}
	else
	{
		if (OutlineMID)
		{
			OutlineMID->SetVectorParameterValue(FName(TEXT("Color")), FLinearColor(1.f, 0.35f, 0.05f));
			OutlineMID->SetScalarParameterValue(FName(TEXT("Opacity")), 0.18f);
		}
		if (FillMID)
		{
			FillMID->SetVectorParameterValue(FName(TEXT("Color")), FLinearColor(1.f, 0.08f, 0.03f));
		}
	}
}

void ATelegraphZoneFX::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	Elapsed += DeltaSeconds;
	const float Dur = FMath::Max(bBurst ? BurstSeconds : Duration, 0.05f);
	const float Progress = FMath::Clamp(Elapsed / Dur, 0.f, 1.f);
	const float OuterScale = FMath::Max(Radius, 1.f) / MeshRadius;

	if (bBurst)
	{
		// Death burst: expand to Radius while fading out.
		Fill->SetRelativeScale3D(FVector(OuterScale * Progress, OuterScale * Progress, DiscThickness));
		if (FillMID)
		{
			FillMID->SetScalarParameterValue(FName(TEXT("Opacity")), 0.5f * (1.f - Progress));
		}
		return;
	}

	// Danger zone: the fill touches the outline exactly at impact — clear the ring before it does.
	// Scales run every tick (not just BeginPlay) so late-arriving initial replication still lands.
	Outline->SetRelativeScale3D(FVector(OuterScale, OuterScale, DiscThickness));
	Fill->SetRelativeScale3D(FVector(OuterScale * Progress, OuterScale * Progress, DiscThickness));
	if (FillMID)
	{
		FillMID->SetScalarParameterValue(FName(TEXT("Opacity")), 0.22f + 0.18f * Progress);
	}
}

ATelegraphZoneFX* ATelegraphZoneFX::SpawnZone(UWorld* World, const FVector& Center, float InRadius, float InDuration)
{
	if (World == nullptr)
	{
		return nullptr;
	}
	const FTransform SpawnT(FRotator::ZeroRotator, Center);
	ATelegraphZoneFX* Zone = World->SpawnActorDeferred<ATelegraphZoneFX>(StaticClass(), SpawnT);
	if (Zone == nullptr)
	{
		return nullptr;
	}
	Zone->Radius = FMath::Max(InRadius, 1.f);
	Zone->Duration = FMath::Max(InDuration, 0.05f);
	// Server lifespan ends the zone at impact; the Destroy replicates to clients.
	Zone->InitialLifeSpan = Zone->Duration + 0.05f;
	Zone->FinishSpawning(SpawnT);
	return Zone;
}

void ATelegraphZoneFX::SpawnLocalBurst(UWorld* World, const FVector& Center, float InRadius, const FLinearColor& Color)
{
	if (World == nullptr)
	{
		return;
	}
	const FTransform SpawnT(FRotator::ZeroRotator, Center);
	ATelegraphZoneFX* Burst = World->SpawnActorDeferred<ATelegraphZoneFX>(StaticClass(), SpawnT);
	if (Burst == nullptr)
	{
		return;
	}
	Burst->bBurst = true;
	Burst->BurstColor = Color;
	Burst->Radius = FMath::Max(InRadius, 1.f);
	Burst->SetReplicates(false); // purely local: each machine spawns its own from its health cosmetics
	Burst->InitialLifeSpan = BurstSeconds + 0.1f;
	Burst->FinishSpawning(SpawnT);
}

void ATelegraphZoneFX::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ATelegraphZoneFX, Radius);
	DOREPLIFETIME(ATelegraphZoneFX, Duration);
}
