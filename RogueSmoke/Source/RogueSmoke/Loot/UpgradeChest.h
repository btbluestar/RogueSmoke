// UpgradeChest.h
// The mini-boss reward chest (UpgradeLoop concept, 2026-06-11): spawned by the GameMode where
// the Brood-mother fell. Any LIVING player standing within OpenRadius opens it (stand-near, like
// the extraction pad — no new input binding), which fires OnOpened once; the GameMode then pauses
// the raid and presents the synergy-upgrade pick to everyone.
//
// Self-contained visuals (engine cube, gold tint, idle spin) so it needs no Blueprint; the open
// pops a gold TelegraphZoneFX burst on every machine via the replicated bOpened flag.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UpgradeChest.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class AUpgradeChest;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnChestOpenedSig, AUpgradeChest*, Chest);

UCLASS()
class ROGUESMOKE_API AUpgradeChest : public AActor
{
	GENERATED_BODY()

public:
	AUpgradeChest();

	/** Any living player inside this radius opens the chest. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Chest")
	float OpenRadius = 220.f;

	/** Server: fired exactly once when a player opens the chest. */
	UPROPERTY(BlueprintAssignable, Category="Chest")
	FOnChestOpenedSig OnOpened;

	UFUNCTION(BlueprintPure, Category="Chest")
	bool IsOpened() const { return bOpened; }

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(ReplicatedUsing=OnRep_Opened)
	bool bOpened = false;

	UFUNCTION()
	void OnRep_Opened();

	UPROPERTY(VisibleAnywhere, Category="Chest")
	UStaticMeshComponent* Body;

	UPROPERTY()
	UMaterialInstanceDynamic* BodyMID = nullptr;

	/** Server: detect a living hero in range and open. */
	void CheckForOpener();

	/** Cosmetic open (every machine): hide the body + pop a gold burst. */
	void PlayOpenFX();

	/** Pawn Health via its ASC (GAS); 1 when unknown (treat as alive). Mirrors AAttackingElite. */
	float GetPawnHealth(APawn* P) const;
};
