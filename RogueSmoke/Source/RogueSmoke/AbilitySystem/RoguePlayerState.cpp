// RoguePlayerState.cpp
#include "RoguePlayerState.h"

#include "AngelscriptAbilitySystemComponent.h"
#include "AbilitySystemComponent.h"
#include "Net/UnrealNetwork.h"

ARoguePlayerState::ARoguePlayerState()
{
	AbilitySystem = CreateDefaultSubobject<UAngelscriptAbilitySystemComponent>(TEXT("AbilitySystem"));
	AbilitySystem->SetIsReplicated(true);

	// Mixed: replicate GEs to the owning client (full prediction surface) + minimal to others.
	// Standard for player-controlled ASCs (Lyra). Attribute values still replicate to teammates.
	AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

	// PlayerState defaults to a low update frequency; GAS wants timely ability/attribute updates.
	SetNetUpdateFrequency(100.0f);
}

UAbilitySystemComponent* ARoguePlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystem;
}

void ARoguePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ARoguePlayerState, Kills);
	DOREPLIFETIME(ARoguePlayerState, DamageDealt);
	DOREPLIFETIME(ARoguePlayerState, DamageTaken);
	DOREPLIFETIME(ARoguePlayerState, TimesDowned);
	DOREPLIFETIME(ARoguePlayerState, Revives);
	DOREPLIFETIME(ARoguePlayerState, UpgradesTaken);
}

// Authority discipline (CODING_STANDARDS §4.4): stats only mutate on the server; clients read
// the replicated values.

void ARoguePlayerState::AddKill()
{
	if (HasAuthority())
	{
		++Kills;
	}
}

void ARoguePlayerState::AddDamageDealt(float Amount)
{
	if (HasAuthority() && Amount > 0.f)
	{
		DamageDealt += Amount;
	}
}

void ARoguePlayerState::AddDamageTaken(float Amount)
{
	if (HasAuthority() && Amount > 0.f)
	{
		DamageTaken += Amount;
	}
}

void ARoguePlayerState::AddDowned()
{
	if (HasAuthority())
	{
		++TimesDowned;
	}
}

void ARoguePlayerState::AddRevive()
{
	if (HasAuthority())
	{
		++Revives;
	}
}

void ARoguePlayerState::AddUpgradeTaken()
{
	if (HasAuthority())
	{
		++UpgradesTaken;
	}
}
