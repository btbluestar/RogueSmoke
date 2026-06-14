// TelegraphZoneFX.h
// The one ground-disc cosmetic actor behind the cue pass (GDD §10 readability):
//  - ZONE mode (replicated, server-spawned via UCombatSubsystem::ShowTelegraphZone): a danger
//    footprint outline at Radius plus a fill disc that reaches the edge exactly when Duration
//    elapses — the player's dodge timer (e.g. the Brood-mother's artillery).
//  - BURST mode (local, each machine spawns its own): a short expanding/fading disc used as the
//    enemy death cue (AEliteEnemyBase::NotifyHealthVisual).
// No art dependencies: engine cylinder mesh + /Game/VFX/M_TelegraphRing (unlit translucent,
// Color/Opacity params — authored by Tools/py_make_telegraph_material.py).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "TelegraphZoneFX.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;

UCLASS()
class ROGUESMOKE_API ATelegraphZoneFX : public AActor
{
	GENERATED_BODY()

public:
	ATelegraphZoneFX();

	/** Danger radius in uu (the outline; the fill grows to it). Replicated at spawn. */
	UPROPERTY(Replicated)
	float Radius = 300.f;

	/** Seconds until impact — the fill reaches the outline exactly then. Replicated at spawn. */
	UPROPERTY(Replicated)
	float Duration = 1.f;

	/** Server-only: spawn the replicated danger-zone ring. Returns null on clients/failure. */
	static ATelegraphZoneFX* SpawnZone(UWorld* World, const FVector& Center, float InRadius, float InDuration);

	/** Any machine: spawn a local (non-replicated) death-burst disc in Color. Fire-and-forget. */
	static void SpawnLocalBurst(UWorld* World, const FVector& Center, float InRadius, const FLinearColor& Color);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, Category="FX")
	UStaticMeshComponent* Outline;

	UPROPERTY(VisibleAnywhere, Category="FX")
	UStaticMeshComponent* Fill;

	UPROPERTY()
	UMaterialInstanceDynamic* OutlineMID = nullptr;

	UPROPERTY()
	UMaterialInstanceDynamic* FillMID = nullptr;

	/** Local death-burst mode (set only on locally spawned instances; never replicated). */
	bool bBurst = false;
	FLinearColor BurstColor = FLinearColor::White;

	float Elapsed = 0.f;

	void SetupDisc(UStaticMeshComponent* Disc);
};
