// RogueHeroBase.cpp
#include "RogueHeroBase.h"

#include "RoguePlayerState.h"
#include "AngelscriptAbilitySystemComponent.h"

ARogueHeroBase::ARogueHeroBase()
{
}

UAbilitySystemComponent* ARogueHeroBase::GetAbilitySystemComponent() const
{
	return AbilitySystem;
}

void ARogueHeroBase::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	// Server: PlayerState is assigned by now.
	InitAbilitySystem();
}

void ARogueHeroBase::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	// Client: the PlayerState (and thus its ASC) has just replicated in.
	InitAbilitySystem();
}

void ARogueHeroBase::InitAbilitySystem()
{
	ARoguePlayerState* RoguePS = GetPlayerState<ARoguePlayerState>();
	if (RoguePS == nullptr)
	{
		return;
	}

	UAngelscriptAbilitySystemComponent* ASC = RoguePS->GetRogueAbilitySystemComponent();
	if (ASC == nullptr || AbilitySystem == ASC)
	{
		return; // not ready, or already initialized for this avatar
	}

	AbilitySystem = ASC;

	// Owner = PlayerState (persistent), Avatar = this pawn (the body abilities act through).
	ASC->InitAbilityActorInfo(RoguePS, this);

	OnAbilitySystemReady();
}
