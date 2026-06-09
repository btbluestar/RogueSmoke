// RoguePlayerState.h
// Owns the player AbilitySystemComponent (Lyra-style: ASC on PlayerState so it survives pawn
// death/respawn). Exists in C++ because IAbilitySystemInterface cannot be implemented in
// AngelScript (plugin limitation / house rule: no UInterfaces in AS).
//
// The ASC itself is the stock UAngelscriptAbilitySystemComponent, so AngelScript drives all
// ability granting / activation against it with no further C++.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "RoguePlayerState.generated.h"

class UAngelscriptAbilitySystemComponent;

UCLASS()
class ROGUESMOKE_API ARoguePlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	ARoguePlayerState();

	UFUNCTION(BlueprintPure, Category = "AbilitySystem")
	UAngelscriptAbilitySystemComponent* GetRogueAbilitySystemComponent() const { return AbilitySystem; }

	//~IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	//~End IAbilitySystemInterface

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AbilitySystem", meta = (AllowPrivateAccess = true))
	TObjectPtr<UAngelscriptAbilitySystemComponent> AbilitySystem;
};
