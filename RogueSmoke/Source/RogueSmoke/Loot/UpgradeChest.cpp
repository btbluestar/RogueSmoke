// UpgradeChest.cpp

#include "Loot/UpgradeChest.h"
#include "VFX/TelegraphZoneFX.h"
#include "RogueHealthSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	const FLinearColor ChestGold(1.f, 0.78f, 0.12f);
}

AUpgradeChest::AUpgradeChest()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicatingMovement(false); // static; the idle spin is local cosmetic on each machine

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	SetRootComponent(Body);
	Body->SetMobility(EComponentMobility::Movable);
	Body->SetRelativeScale3D(FVector(0.7f, 0.7f, 0.7f));
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision); // walk-through; proximity opens it

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		Body->SetStaticMesh(CubeMesh.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Mat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (Mat.Succeeded())
	{
		Body->SetMaterial(0, Mat.Object);
	}
}

void AUpgradeChest::BeginPlay()
{
	Super::BeginPlay();
	BodyMID = Body->CreateDynamicMaterialInstance(0);
	if (BodyMID != nullptr)
	{
		BodyMID->SetVectorParameterValue(FName(TEXT("Color")), ChestGold);
	}
}

void AUpgradeChest::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!bOpened)
	{
		// Idle spin: loot language without art. Cosmetic, run on every machine.
		AddActorLocalRotation(FRotator(0.f, 60.f * DeltaSeconds, 0.f));

		if (HasAuthority())
		{
			CheckForOpener();
		}
	}
}

void AUpgradeChest::CheckForOpener()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	const float RadiusSq = OpenRadius * OpenRadius;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (Pawn == nullptr || GetPawnHealth(Pawn) <= 0.f)
		{
			continue; // downed players can't loot
		}
		if (FVector::DistSquared(Pawn->GetActorLocation(), GetActorLocation()) <= RadiusSq)
		{
			bOpened = true;
			PlayOpenFX();              // server/host view; clients via OnRep
			OnOpened.Broadcast(this);  // the GameMode pauses + offers the synergy pick
			SetLifeSpan(2.f);          // linger so the replicated open reaches clients, then go
			return;
		}
	}
}

void AUpgradeChest::PlayOpenFX()
{
	if (Body != nullptr)
	{
		Body->SetVisibility(false);
	}
	ATelegraphZoneFX::SpawnLocalBurst(GetWorld(), GetActorLocation(), 200.f, ChestGold);
}

void AUpgradeChest::OnRep_Opened()
{
	if (bOpened)
	{
		PlayOpenFX();
	}
}

float AUpgradeChest::GetPawnHealth(APawn* P) const
{
	UAbilitySystemComponent* ASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(P);
	if (ASC == nullptr)
	{
		return 1.f;
	}
	bool bFound = false;
	const float HealthValue = ASC->GetGameplayAttributeValue(URogueHealthSet::GetHealthAttribute(), bFound);
	return bFound ? HealthValue : 1.f;
}

void AUpgradeChest::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AUpgradeChest, bOpened);
}
