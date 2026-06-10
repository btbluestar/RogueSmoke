// HealthComponent.cpp

#include "Combat/HealthComponent.h"
#include "AbilitySystem/RoguePlayerState.h"
#include "GameFramework/Pawn.h"
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

	// Stat credit: every player->enemy damage path (hitscan, radial, future ones) funnels through
	// here, so this is the one place damage-dealt and kills attribute to the instigating player.
	// Pool-safe: IsDead() early-outs above and ResetHealth() restores on recycle, so a given death
	// credits exactly one kill.
	if (Applied > 0.f && DamageInstigator != nullptr)
	{
		if (const APawn* InstigatorPawn = Cast<APawn>(DamageInstigator))
		{
			if (ARoguePlayerState* PS = InstigatorPawn->GetPlayerState<ARoguePlayerState>())
			{
				PS->AddDamageDealt(Applied);
				if (IsDead())
				{
					PS->AddKill();
				}
			}
		}
	}

	if (IsDead())
	{
		OnDeath.Broadcast(GetOwner());
	}

	return Applied;
}

void UHealthComponent::ResetHealth()
{
	Health = MaxHealth;
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
