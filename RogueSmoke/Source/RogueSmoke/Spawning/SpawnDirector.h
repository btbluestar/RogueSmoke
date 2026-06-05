// SpawnDirector.h
// THE SPAWN SEAM. Mirrors UCombatSubsystem: just as combat hides whether a target is an
// Actor or a Mass agent, the SpawnDirector hides HOW enemies are created. Script asks for
// enemies; the director owns pooling (elites) and — later — the Mass fodder backend.
//
// MVP STATUS: pooled-Actor backend for elites is implemented (cheap reuse instead of
// SpawnActor/Destroy churn). Mass fodder (D-0003, SETUP §5.5) is a logged placeholder that
// plugs into SpawnFodderWave() without changing any caller.
//
// Authority: spawning is server-only (D-0004). Mutating calls no-op on clients.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "SpawnDirector.generated.h"

class AEliteEnemyBase;

UCLASS()
class ROGUESMOKE_API USpawnDirector : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// NOTE: no hand-written Get() — the AngelScript fork auto-generates a static
	// USpawnDirector::Get() for UWorldSubsystem types. In C++ use GetWorld()->GetSubsystem<>().

	/** Spawn or recycle one elite at a transform. Server-only. Null off-server / on failure. */
	UFUNCTION(BlueprintCallable, Category="Spawning")
	AEliteEnemyBase* SpawnElite(TSubclassOf<AEliteEnemyBase> EliteClass, FVector Location, FRotator Rotation);

	/** Spawn Count elites evenly around Center on a ring of Radius (deterministic). Server-only. */
	UFUNCTION(BlueprintCallable, Category="Spawning")
	void SpawnEliteWave(TSubclassOf<AEliteEnemyBase> EliteClass, FVector Center, float Radius, int32 Count);

	/** Pre-create Count pooled elites up front to avoid first-spawn hitches. Server-only. */
	UFUNCTION(BlueprintCallable, Category="Spawning")
	void PrewarmElites(TSubclassOf<AEliteEnemyBase> EliteClass, int32 Count);

	/** PLACEHOLDER for the Mass fodder backend (D-0003, SETUP §5.5). Logs until Mass lands. */
	UFUNCTION(BlueprintCallable, Category="Spawning")
	void SpawnFodderWave(FVector Center, float Radius, int32 Count);

private:
	bool IsServer() const;

	AEliteEnemyBase* AcquireFromPool(UClass* EliteClass);
	AEliteEnemyBase* CreateNewElite(UClass* EliteClass);

	/** Bound to each pooled elite's OnDeath — returns the actor to the free list. */
	UFUNCTION()
	void HandleEliteDeath(AActor* DeadActor);

	// Inactive elites available for reuse, keyed by leaf class.
	TMap<UClass*, TArray<TWeakObjectPtr<AEliteEnemyBase>>> FreePool;
};
