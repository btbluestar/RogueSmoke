// CombatSubsystem.h
// THE SEAM (D-0002, MVP arch §3). The only combat API AngelScript needs.
// Hides whether a target is an Actor elite or (later) a Mass agent, and is
// authoritative: mutating calls only do work on the server.
//
// MVP STATUS: Actor-only stub backend (a registry of AEliteEnemyBase). The Mass
// entity-grid path (D-0003) plugs into the same four methods in SETUP §5.5.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "CombatSubsystem.generated.h"

class AEliteEnemyBase;

UCLASS()
class ROGUESMOKE_API UCombatSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// NOTE: no hand-written Get() — the AngelScript fork auto-generates a static
	// UCombatSubsystem::Get() for UWorldSubsystem types. In C++ use GetWorld()->GetSubsystem<>().

	// --- Registration (elite Actors call these on the server) ---
	void RegisterElite(AEliteEnemyBase* Elite);
	void UnregisterElite(AEliteEnemyBase* Elite);

	// --- Queries (safe on any machine) ---
	UFUNCTION(BlueprintCallable, Category="Combat")
	int32 CountEnemiesInSphere(FVector Center, float Radius) const;

	/** Total live elites registered (any location). Used by the raid objective to detect "arena cleared". */
	UFUNCTION(BlueprintCallable, Category="Combat")
	int32 GetEliteCount() const;

	// --- Setup operations (server-authoritative) ---
	UFUNCTION(BlueprintCallable, Category="Combat")
	void PullEnemiesToward(FVector Center, float Radius, float Strength, float Duration);

	UFUNCTION(BlueprintCallable, Category="Combat")
	void MarkClustered(FVector Center, float Radius, float Duration);

	// --- Payoff operation (server-authoritative) ---
	/** Radial damage to every enemy in range; Clustered enemies take BaseDamage * ClusterBonusMultiplier. Returns total dealt. */
	UFUNCTION(BlueprintCallable, Category="Combat")
	float ApplyRadialDamage(FVector Center, float Radius, float BaseDamage,
	                        float ClusterBonusMultiplier, AActor* DamageInstigator);

private:
	bool IsServer() const;

	// Stub backend. Weak ptrs so destroyed elites drop out without dangling.
	TArray<TWeakObjectPtr<AEliteEnemyBase>> Elites;
};
