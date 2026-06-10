// HealthComponent.h
// Replicated health for elite Actors (D-0002 hot path / D-0003 elites-are-Actors).
// Authoritative: only the server may apply damage; Health replicates to clients.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HealthComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeath, AActor*, DeadActor);

/** Damage-over-time flavors weapon upgrades can proc on enemies (WEAPON_UPGRADES_PLAN.md). */
UENUM(BlueprintType)
enum class ERogueDotType : uint8
{
	Burn,    // fast: big DPS, short duration
	Poison   // slow: lower DPS, long duration, bigger total
};

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

	/** Restore to full. Used when recycling a pooled actor (SpawnDirector). Also clears active DoTs. */
	UFUNCTION(BlueprintCallable, Category="Health")
	void ResetHealth();

	UFUNCTION(BlueprintPure, Category="Health")
	bool IsDead() const { return Health <= 0.f; }

	/**
	 * Server-authoritative damage-over-time (weapon upgrade procs: burn/poison). One slot per type:
	 * re-applying refreshes the duration and keeps the strongest DPS (no unbounded stacking). Damage
	 * ticks through ApplyDamage, so stat credit / kill credit / pool safety are inherited. The
	 * component only ticks while a dot is active.
	 */
	UFUNCTION(BlueprintCallable, Category="Health")
	void ApplyDot(ERogueDotType Type, float DamagePerSecond, float Duration, AActor* DamageInstigator);

	UFUNCTION(BlueprintPure, Category="Health")
	bool HasActiveDot(ERogueDotType Type) const;

	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_Health();

private:
	struct FActiveDot
	{
		float Dps = 0.f;
		float Remaining = 0.f;
		TWeakObjectPtr<AActor> Instigator;
	};

	// Indexed by ERogueDotType. Server-only state (the resulting Health change replicates).
	FActiveDot Dots[2];

	void ClearDots();
};
