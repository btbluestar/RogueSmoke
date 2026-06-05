// HealthComponent.cpp

#include "Combat/HealthComponent.h"
#include "Net/UnrealNetwork.h"

UHealthComponent::UHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UHealthComponent::BeginPlay()
{
	Super::BeginPlay();
	Health = MaxHealth;
}

float UHealthComponent::ApplyDamage(float Amount, AActor* DamageInstigator)
{
	// Authority discipline (CODING_STANDARDS §4.4): damage is server-only.
	if (GetOwner() == nullptr || !GetOwner()->HasAuthority() || IsDead() || Amount <= 0.f)
	{
		return 0.f;
	}

	const float Before = Health;
	Health = FMath::Max(0.f, Health - Amount);
	const float Applied = Before - Health;

	if (IsDead())
	{
		OnDeath.Broadcast(GetOwner());
	}

	return Applied;
}

void UHealthComponent::OnRep_Health()
{
	// Hook for clients to update health bars / cosmetics. No gameplay logic here.
}

void UHealthComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UHealthComponent, Health);
}
