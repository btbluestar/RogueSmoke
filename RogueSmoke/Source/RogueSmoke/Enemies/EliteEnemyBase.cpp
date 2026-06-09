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
		if (UCombatSubsystem* Combat = (GetWorld() ? GetWorld()->GetSubsystem<UCombatSubsystem>() : nullptr))
		{
			Combat->RegisterElite(this);
		}
	}
}

void AEliteEnemyBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UCombatSubsystem* Combat = (GetWorld() ? GetWorld()->GetSubsystem<UCombatSubsystem>() : nullptr))
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
		// Steer horizontally toward the taunt point. bSweep=false on purpose: a swept move from
		// an actor placed at/under the floor (Z=0, half-buried) reports an immediate blocking hit
		// and freezes — which made this stub a visible no-op. The pull only needs to read, so slide.
		FVector Dir = PullTarget - GetActorLocation();
		Dir.Z = 0.f;
		Dir = Dir.GetSafeNormal();
		SetActorLocation(GetActorLocation() + Dir * PullStrength * DeltaSeconds, /*bSweep=*/false);
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

void AEliteEnemyBase::ClearTransientState()
{
	ClusteredUntilSeconds = 0.f;
	PullTarget = FVector::ZeroVector;
	PullStrength = 0.f;
	PullExpiresAtSeconds = 0.f;
}

void AEliteEnemyBase::Activate(const FVector& Location, const FRotator& Rotation)
{
	SetActorLocationAndRotation(Location, Rotation);
	ClearTransientState();

	if (Health != nullptr)
	{
		Health->ResetHealth();
	}

	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	SetActorTickEnabled(true);

	// Re-join the combat registry (AddUnique guards against double-register).
	if (HasAuthority())
	{
		if (UCombatSubsystem* Combat = (GetWorld() ? GetWorld()->GetSubsystem<UCombatSubsystem>() : nullptr))
		{
			Combat->RegisterElite(this);
		}
	}

	bActive = true;
}

void AEliteEnemyBase::Deactivate()
{
	if (UCombatSubsystem* Combat = (GetWorld() ? GetWorld()->GetSubsystem<UCombatSubsystem>() : nullptr))
	{
		Combat->UnregisterElite(this);
	}

	ClearTransientState();
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);

	bActive = false;
}
