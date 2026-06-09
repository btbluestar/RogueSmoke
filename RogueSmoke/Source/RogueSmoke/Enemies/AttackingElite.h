// AttackingElite.h
// C++ base for the attacking elites/specials (Carapace / Spitter / Bloater / Lunger — bio-horde roster).
// Extends AEliteEnemyBase so it inherits replicated health, the Clustered synergy state, pull steering,
// and pool registration. Self-contained like AFodderEnemy: it owns a visible body with Visibility-blocking
// collision, so an elite is visible AND hittable with no Blueprint or assigned mesh.
//
// The shared server loop — acquire the nearest LIVING hero (GAS Health > 0), approach to PreferredRange,
// telegraph, then attack — lives here ("compile the simulation"). The per-archetype attack is a
// BlueprintNativeEvent that AngelScript subclasses override ("script the decision"). All tuning is
// UPROPERTY (designer-iterable). Telegraphs are a hard readability requirement (GDD §10); the wind-up is
// the player's counterplay window.

#pragma once

#include "CoreMinimal.h"
#include "Enemies/EliteEnemyBase.h"
#include "AttackingElite.generated.h"

class UStaticMeshComponent;
class APawn;

UCLASS()
class ROGUESMOKE_API AAttackingElite : public AEliteEnemyBase
{
	GENERATED_BODY()

public:
	AAttackingElite();

	// Tunables are BlueprintReadWrite so AngelScript subclasses can both set them via `default` and read
	// them in overridden attacks (e.g. Lunger/Bloater reading AttackDamage/AttackRadius).

	/** 0 = keep the HealthComponent default (100); else set max+current at BeginPlay (per-archetype HP). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float MaxHealthOverride = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float MoveSpeed = 220.f;

	/** Stop closing once this near the target (melee elites ~150; ranged elites set this large to kite). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float PreferredRange = 160.f;

	/** The attack reaches when the target is within this distance. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float AttackRange = 220.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float AttackDamage = 15.f;

	/** >0 = radial attack (slam/explosion) damaging all players in the radius; 0 = single-target. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float AttackRadius = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float AttackInterval = 2.5f;

	/** Wind-up before the attack lands — the readability tell + the player's dodge window (GDD §10). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	float TelegraphSeconds = 0.7f;

	/** Per-archetype silhouette scale, applied to the body at BeginPlay. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Enemy")
	FVector BodyScale = FVector(1.f, 1.f, 2.f);

	UPROPERTY(EditDefaultsOnly, Category="Enemy")
	bool bShowDebug = true;

	/** The currently targeted player pawn (nearest living hero). Valid only on the server. */
	UFUNCTION(BlueprintPure, Category="Enemy")
	APawn* GetCurrentTarget() const { return Target.Get(); }

	/** True while this elite is winding up an attack (for cues / animation). */
	UFUNCTION(BlueprintPure, Category="Enemy")
	bool IsTelegraphing() const { return bTelegraphing; }

	/**
	 * The attack committed at the end of the telegraph. Override in AngelScript subclasses for ranged /
	 * explosion / charge behaviors. Default: radial slam when AttackRadius > 0, else single-target melee.
	 */
	UFUNCTION(BlueprintNativeEvent, Category="Combat")
	void PerformAttack();
	virtual void PerformAttack_Implementation();

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void ClearTransientState() override;

	/** Cheap visible body + hitscan target (Visibility-blocking), no skeletal mesh / AIController. */
	UPROPERTY(VisibleAnywhere, Category="Enemy")
	UStaticMeshComponent* Body;

	TWeakObjectPtr<APawn> Target;
	float AttackCooldown = 0.f;
	float TelegraphRemaining = 0.f;
	bool bTelegraphing = false;

	void AcquireTarget();
	bool IsTargetInAttackRange() const;
	void ApproachTarget(float DeltaSeconds);
	void FaceTarget();
	/** Current Health of a pawn via its ASC (GAS); returns 1 when unknown (treat as alive). */
	float GetPawnHealth(APawn* P) const;
	void DrawEnemyDebug() const;
};
