// FodderEnemy.h
// Interim cheap-Actor SWARM unit (D-0003 fodder, fast path to prove combat feel BEFORE Mass).
// Subclasses AEliteEnemyBase so it reuses the proven seam plumbing for free: it registers in
// the same CombatSubsystem registry, so pull / MarkClustered / ApplyRadialDamage / FireHitscan
// all affect it with ZERO seam changes, and the SpawnDirector pool spawns it unchanged.
//
// The ONE difference from an elite: fodder does NOT count toward the raid objective's
// "arena cleared" gate (bCountsAsObjectiveTarget = false), and it steers toward the nearest
// player when it isn't being pulled. Self-contained (owns a cheap sphere body), so it needs no
// Blueprint to be visible. When the Mass backend lands it replaces this behind SpawnFodderWave().

#pragma once

#include "CoreMinimal.h"
#include "Enemies/EliteEnemyBase.h"
#include "FodderEnemy.generated.h"

class UStaticMeshComponent;

UCLASS()
class ROGUESMOKE_API AFodderEnemy : public AEliteEnemyBase
{
	GENERATED_BODY()

public:
	AFodderEnemy();

	/** Ground move speed (uu/s) when steering toward the nearest player. */
	UPROPERTY(EditDefaultsOnly, Category="Fodder")
	float MoveSpeed = 320.f;

	/** Stop closing once this near a player (melee range; prevents jitter on top of the hero). */
	UPROPERTY(EditDefaultsOnly, Category="Fodder")
	float StopDistance = 120.f;

	/** Contact melee: damage dealt to a player within AttackRange, at most once per AttackInterval. */
	UPROPERTY(EditDefaultsOnly, Category="Fodder")
	float MeleeDamage = 8.f;

	UPROPERTY(EditDefaultsOnly, Category="Fodder")
	float AttackInterval = 1.0f;

	UPROPERTY(EditDefaultsOnly, Category="Fodder")
	float AttackRange = 170.f;

protected:
	virtual void Tick(float DeltaSeconds) override;

	/** Cheap visible body + hitscan target. No skeletal mesh, no AIController, no movement comp. */
	UPROPERTY(VisibleAnywhere, Category="Fodder")
	UStaticMeshComponent* Body;

	/** Server-only: move toward the nearest player pawn. Skipped while a pull is steering us. */
	void SteerTowardNearestPlayer(float DeltaSeconds);
	AActor* FindNearestPlayerPawn() const;

	/** Server-only: if a player is within AttackRange and the cooldown is up, deal contact melee. */
	void TryMeleeNearestPlayer();

	float AttackCooldown = 0.f;
};
