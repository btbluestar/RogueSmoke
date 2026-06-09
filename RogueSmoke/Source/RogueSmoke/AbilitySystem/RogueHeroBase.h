// RogueHeroBase.h
// C++ base for all player heroes. Bridges the pawn to the PlayerState-owned ASC: implements
// IAbilitySystemInterface (which AngelScript cannot) and initializes the ability actor info at
// the right replication moments. The AngelScript AHeroCharacter subclasses this and does all the
// actual gameplay (granting its AbilitySet, binding input, abilities) via OnAbilitySystemReady.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "RogueHeroBase.generated.h"

class UAngelscriptAbilitySystemComponent;

UCLASS(abstract, meta = (ChildCanTick))
class ROGUESMOKE_API ARogueHeroBase : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	ARogueHeroBase();

	//~IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~End IAbilitySystemInterface

	UFUNCTION(BlueprintPure, Category = "AbilitySystem")
	UAngelscriptAbilitySystemComponent* GetRogueAbilitySystem() const { return AbilitySystem; }

	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;

protected:
	// Fired (server + clients) once the ASC actor info is initialized for this pawn. AngelScript
	// overrides this to grant abilities (server, gated on authority) and bind input (local client).
	UFUNCTION(BlueprintImplementableEvent, Category = "AbilitySystem")
	void OnAbilitySystemReady();

	// Resolve the PlayerState ASC, point it at this pawn as avatar, and notify script. Idempotent.
	void InitAbilitySystem();

	UPROPERTY(Transient)
	TObjectPtr<UAngelscriptAbilitySystemComponent> AbilitySystem;
};
