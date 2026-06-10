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
class AActor;
class APawn;

/** Result of a single hitscan through the seam. Returned by value for clean AngelScript interop. */
USTRUCT(BlueprintType)
struct FHitscanResult
{
	GENERATED_BODY()

	/** True if the trace hit a registered enemy and applied damage. */
	UPROPERTY(BlueprintReadOnly, Category="Combat")
	bool bHitEnemy = false;

	/** True if the trace hit anything (enemy or world geometry). */
	UPROPERTY(BlueprintReadOnly, Category="Combat")
	bool bBlockingHit = false;

	/** Where the trace stopped (impact point, or the trace end if nothing was hit). Use for tracer end + impact FX. */
	UPROPERTY(BlueprintReadOnly, Category="Combat")
	FVector ImpactPoint = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category="Combat")
	FVector ImpactNormal = FVector::ZeroVector;

	/** Damage actually applied to the enemy (0 if a wall/nothing was hit, or on a client). */
	UPROPERTY(BlueprintReadOnly, Category="Combat")
	float DamageDealt = 0.f;

	/** The actor hit, if any. */
	UPROPERTY(BlueprintReadOnly, Category="Combat")
	TObjectPtr<AActor> HitActor = nullptr;
};

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

	// --- Single-target hitscan (server-authoritative; the shooting path) ---
	/**
	 * Trace Start->End and damage the first registered enemy hit through its HealthComponent.
	 * Returns the impact (for tracer/impact FX) and damage dealt. The Mass-fodder raycast plugs in
	 * here later (D-0003); abilities call this instead of tracing/iterating enemies themselves.
	 */
	UFUNCTION(BlueprintCallable, Category="Combat")
	FHitscanResult FireHitscan(FVector Start, FVector End, float Damage, AActor* DamageInstigator);

	/**
	 * Camera-origin visibility trace returning the world point under the crosshair — the convergence
	 * target for third-person muzzle fire (D-0014). Returns the impact point, or CamStart + CamDir*MaxDist
	 * if nothing is hit. Read-only query (no damage), so abilities don't world-trace themselves: the
	 * shooting ability resolves this, then fires the damage trace from the muzzle toward the result.
	 */
	UFUNCTION(BlueprintCallable, Category="Combat")
	FVector ResolveAimPoint(FVector CamStart, FVector CamDir, float MaxDist, AActor* IgnoreActor) const;

	/**
	 * True if nothing blocks the Visibility channel between From and Target (the target's own body is
	 * ignored, so only world geometry / a third actor count as cover). Ranged enemies gate their shot on
	 * this so they can't snipe through walls — breaking line of sight is the counterplay. Read-only.
	 */
	UFUNCTION(BlueprintCallable, Category="Combat")
	bool HasLineOfSightToActor(FVector From, AActor* Target, AActor* IgnoreActor) const;

	// --- Enemy -> player damage (server-authoritative). The outbound analog of FireHitscan: enemies
	// damage the players' GAS health through here instead of touching the ASC directly. Applies a
	// transient instant Damage GE, so RogueHealthSet resolves armor/shield/health uniformly. ---
	UFUNCTION(BlueprintCallable, Category="Combat")
	void ApplyDamageToPlayer(APawn* Target, float Damage, AActor* DamageInstigator);

	/** Apply Damage to every player pawn within Radius of Center (explosions, slams). Server-only. */
	UFUNCTION(BlueprintCallable, Category="Combat")
	void ApplyRadialDamageToPlayers(FVector Center, float Radius, float Damage, AActor* DamageInstigator);

private:
	bool IsServer() const;

	// Stub backend. Weak ptrs so destroyed elites drop out without dangling.
	TArray<TWeakObjectPtr<AEliteEnemyBase>> Elites;
};
