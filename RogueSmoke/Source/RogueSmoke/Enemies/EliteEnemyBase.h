// EliteEnemyBase.h
// AActor base for elites/bosses (D-0003). Extended in AngelScript (ClusterableElite.as).
// Holds replicated health + the "Clustered" synergy state + a stub pull-steering impl.
// NOTE: pull steering here is a placeholder so the Actor-only combo (SETUP §5.4) is
// playable. Real movement/steering arrives with the Mass spike (SETUP §5.5).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "EliteEnemyBase.generated.h"

class UHealthComponent;

UCLASS(abstract)
class ROGUESMOKE_API AEliteEnemyBase : public AActor
{
	GENERATED_BODY()

public:
	AEliteEnemyBase();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Combat")
	UHealthComponent* Health;

	/** Flag this enemy as Clustered for Duration seconds (the synergy condition). Server-only. */
	UFUNCTION(BlueprintCallable, Category="Combat")
	void MarkClustered(float Duration);

	UFUNCTION(BlueprintPure, Category="Combat")
	bool IsClustered() const;

	/** True for elites/bosses (gate the "arena cleared" objective); false for fodder. See GetEliteCount. */
	bool CountsAsObjectiveTarget() const { return bCountsAsObjectiveTarget; }

	/** Begin steering toward Target at Strength (uu/s) for Duration seconds. Server-only stub. */
	UFUNCTION(BlueprintCallable, Category="Combat")
	void ApplyPull(const FVector& Target, float Strength, float Duration);

	/** True while a taunt/pull is steering us. AS attacking-elite subclasses skip their own movement
	 *  while this is set so they don't fight the base pull steering (the synergy SETUP). */
	UFUNCTION(BlueprintPure, Category="Combat")
	bool IsBeingPulled() const;

	// --- Object pooling (driven by USpawnDirector) ---
	/** Re-activate from the pool: place, reset health + transient state, register, show. */
	void Activate(const FVector& Location, const FRotator& Rotation);
	/** Return to the pool: unregister, clear transient state, hide + disable. */
	void Deactivate();
	bool IsActive() const { return bActive; }

protected:
	/** Wipe per-life transient state (cluster mark, pull) so a recycled actor starts clean. Virtual so
	 *  attacking-elite subclasses also clear their attack/telegraph state on pool reuse. */
	virtual void ClearTransientState();

	bool bActive = true;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Counts toward the raid "clear the arena" objective. Fodder sets this false. */
	bool bCountsAsObjectiveTarget = true;

	/** World time (seconds) before which this enemy counts as Clustered. */
	float ClusteredUntilSeconds = 0.f;

	FVector PullTarget = FVector::ZeroVector;
	float PullStrength = 0.f;
	float PullExpiresAtSeconds = 0.f;
};
