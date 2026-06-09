// RoguePlayerState.cpp
#include "RoguePlayerState.h"

#include "AngelscriptAbilitySystemComponent.h"
#include "AbilitySystemComponent.h"

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
