// HealthComponent.h
// Replicated health for elite Actors (D-0002 hot path / D-0003 elites-are-Actors).
// Authoritative: only the server may apply damage; Health replicates to clients.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HealthComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeath, AActor*, DeadActor);

UCLASS(ClassGroup=(Combat), meta=(BlueprintSpawnableComponent))
class ROGUESMOKE_API UHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UHealthComponent();

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Health")
	float MaxHealth = 100.f;

	UPROPERTY(ReplicatedUsing=OnRep_Health, BlueprintReadOnly, Category="Health")
	float Health = 100.f;

	/** Broadcast on the server when Health first reaches 0. */
	UPROPERTY(BlueprintAssignable, Category="Health")
	FOnDeath OnDeath;

	/** Server-authoritative. Returns the damage actually applied (0 on a client or if dead). */
	UFUNCTION(BlueprintCallable, Category="Health")
	float ApplyDamage(float Amount, AActor* DamageInstigator);

	/** Restore to full. Used when recycling a pooled actor (SpawnDirector). */
	UFUNCTION(BlueprintCallable, Category="Health")
	void ResetHealth();

	UFUNCTION(BlueprintPure, Category="Health")
	bool IsDead() const { return Health <= 0.f; }

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_Health();
};
