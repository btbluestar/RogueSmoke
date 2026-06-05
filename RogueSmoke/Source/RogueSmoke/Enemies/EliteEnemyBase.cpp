// EliteEnemyBase.cpp

#include "Enemies/EliteEnemyBase.h"
#include "Combat/HealthComponent.h"
#include "Combat/CombatSubsystem.h"
#include "Engine/World.h"

AEliteEnemyBase::AEliteEnemyBase()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;

	Health = CreateDefaultSubobject<UHealthComponent>(TEXT("Health"));
}

void AEliteEnemyBase::BeginPlay()
{
	Super::BeginPlay();

	// Register with the seam so AoE queries/abilities can find us (server owns the registry).
	if (HasAuthority())
	{
		if (UCombatSubsystem* Combat = UCombatSubsystem::Get(this))
		{
			Combat->RegisterElite(this);
		}
	}
}

void AEliteEnemyBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UCombatSubsystem* Combat = UCombatSubsystem::Get(this))
	{
		Combat->UnregisterElite(this);
	}

	Super::EndPlay(EndPlayReason);
}

void AEliteEnemyBase::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (!HasAuthority() || GetWorld() == nullptr)
	{
		return;
	}

	// Stub pull steering: lerp toward the taunt point while the pull is active.
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now < PullExpiresAtSeconds)
	{
		const FVector Dir = (PullTarget - GetActorLocation()).GetSafeNormal();
		SetActorLocation(GetActorLocation() + Dir * PullStrength * DeltaSeconds, /*bSweep=*/true);
	}
}

void AEliteEnemyBase::MarkClustered(float Duration)
{
	if (GetWorld())
	{
		ClusteredUntilSeconds = GetWorld()->GetTimeSeconds() + Duration;
	}
}

bool AEliteEnemyBase::IsClustered() const
{
	return GetWorld() != nullptr && GetWorld()->GetTimeSeconds() < ClusteredUntilSeconds;
}

void AEliteEnemyBase::ApplyPull(const FVector& Target, float Strength, float Duration)
{
	if (GetWorld())
	{
		PullTarget = Target;
		PullStrength = Strength;
		PullExpiresAtSeconds = GetWorld()->GetTimeSeconds() + Duration;
	}
}
