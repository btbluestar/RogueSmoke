// SpitterProjectile.h
// The Spitter's acid glob: a short-lived arcing projectile lobbed after the telegraph instead of an
// instant hit, so the shot is readable (you see it coming — GDD §10) and dodgeable by repositioning.
// Server-authoritative: the server flies it along a straight path with a cosmetic sine Z-arc toward the
// captured impact point, then applies a small radial splash to players via the combat seam and destroys.
// Movement replicates so clients see the glob. MVP: direct spawn (pool later, CODING_STANDARDS §8).

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SpitterProjectile.generated.h"

class UStaticMeshComponent;

UCLASS()
class ROGUESMOKE_API ASpitterProjectile : public AActor
{
	GENERATED_BODY()

public:
	ASpitterProjectile();

	/**
	 * Fire toward ImpactPoint (captured at launch — the target's position then, so moving dodges it).
	 * Server-only effect; movement replicates for clients. Damage is a radial splash on landing.
	 */
	UFUNCTION(BlueprintCallable, Category="Combat")
	void Launch(FVector ImpactPoint, float Speed, float Damage, float SplashRadius, AActor* Shooter);

	/** Cosmetic peak height of the lob arc. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category="Projectile")
	float ArcHeight = 180.f;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY(VisibleAnywhere, Category="Projectile")
	UStaticMeshComponent* Body;

private:
	FVector StartLoc = FVector::ZeroVector;
	FVector TargetLoc = FVector::ZeroVector;
	float Speed = 1400.f;
	float HitDamage = 12.f;
	float Splash = 180.f;
	float Elapsed = 0.f;
	float TravelTime = 0.5f;
	bool bLaunched = false;
	bool bResolved = false;
	TWeakObjectPtr<AActor> Shooter;

	void Resolve();
};
