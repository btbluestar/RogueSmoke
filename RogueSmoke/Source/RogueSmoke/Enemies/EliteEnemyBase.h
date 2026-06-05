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

	/** Begin steering toward Target at Strength (uu/s) for Duration seconds. Server-only stub. */
	UFUNCTION(BlueprintCallable, Category="Combat")
	void ApplyPull(const FVector& Target, float Strength, float Duration);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** World time (seconds) before which this enemy counts as Clustered. */
	float ClusteredUntilSeconds = 0.f;

	FVector PullTarget = FVector::ZeroVector;
	float PullStrength = 0.f;
	float PullExpiresAtSeconds = 0.f;
};
