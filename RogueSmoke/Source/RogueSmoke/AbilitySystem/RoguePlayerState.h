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

	// --- Per-player run stats (results screen / scoreboard). Replicated so every client can
	// render everyone's column. Server-only accumulation via the Add* helpers: damage credit
	// comes from UHealthComponent::ApplyDamage / UCombatSubsystem::ApplyDamageToPlayer (C++);
	// downs/revives/upgrades from the AngelScript systems that own those moments. ---

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Stats")
	int32 Kills = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Stats")
	float DamageDealt = 0.f;

	/** Incoming (pre-mitigation) damage — armor/shield resolve later in RogueHealthSet. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Stats")
	float DamageTaken = 0.f;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Stats")
	int32 TimesDowned = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Stats")
	int32 Revives = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Stats")
	int32 UpgradesTaken = 0;

	// --- Lobby state (hero select). Replicated so every client's lobby UI shows everyone's
	// pick + ready flag. Index into the shared RogueHeroes roster; -1 = no pick yet. ---

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Lobby")
	int32 SelectedHeroIndex = -1;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Lobby")
	bool bLobbyReady = false;

	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void SetSelectedHeroIndex(int32 Index);

	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void SetLobbyReady(bool bReady);

	UFUNCTION(BlueprintCallable, Category = "Stats")
	void AddKill();

	UFUNCTION(BlueprintCallable, Category = "Stats")
	void AddDamageDealt(float Amount);

	UFUNCTION(BlueprintCallable, Category = "Stats")
	void AddDamageTaken(float Amount);

	UFUNCTION(BlueprintCallable, Category = "Stats")
	void AddDowned();

	UFUNCTION(BlueprintCallable, Category = "Stats")
	void AddRevive();

	UFUNCTION(BlueprintCallable, Category = "Stats")
	void AddUpgradeTaken();

protected:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AbilitySystem", meta = (AllowPrivateAccess = true))
	TObjectPtr<UAngelscriptAbilitySystemComponent> AbilitySystem;
};
