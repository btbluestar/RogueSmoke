// SpawnDirector.cpp

#include "Spawning/SpawnDirector.h"
#include "Combat/HealthComponent.h"
#include "Enemies/EliteEnemyBase.h"
#include "Enemies/FodderEnemy.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

bool USpawnDirector::IsServer() const
{
	const UWorld* World = GetWorld();
	return World != nullptr && World->GetNetMode() != NM_Client;
}

AEliteEnemyBase* USpawnDirector::CreateNewElite(UClass* EliteClass)
{
	UWorld* World = GetWorld();
	if (World == nullptr || EliteClass == nullptr)
	{
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AEliteEnemyBase* Elite = World->SpawnActor<AEliteEnemyBase>(EliteClass, FVector::ZeroVector, FRotator::ZeroRotator, Params);
	if (Elite != nullptr && Elite->Health != nullptr)
	{
		// Bind once; the binding persists across recycle so reused actors report death too.
		Elite->Health->OnDeath.AddDynamic(this, &USpawnDirector::HandleEliteDeath);
	}
	return Elite;
}

AEliteEnemyBase* USpawnDirector::AcquireFromPool(UClass* EliteClass)
{
	if (TArray<TWeakObjectPtr<AEliteEnemyBase>>* Free = FreePool.Find(EliteClass))
	{
		while (Free->Num() > 0)
		{
			TWeakObjectPtr<AEliteEnemyBase> Ptr = Free->Pop(EAllowShrinking::No);
			if (Ptr.IsValid())
			{
				return Ptr.Get();
			}
		}
	}
	return CreateNewElite(EliteClass);
}

AEliteEnemyBase* USpawnDirector::SpawnElite(TSubclassOf<AEliteEnemyBase> EliteClass, FVector Location, FRotator Rotation)
{
	if (!IsServer() || EliteClass == nullptr)
	{
		return nullptr;
	}

	AEliteEnemyBase* Elite = AcquireFromPool(EliteClass);
	if (Elite != nullptr)
	{
		Elite->Activate(Location, Rotation);
	}
	return Elite;
}

void USpawnDirector::SpawnEliteWave(TSubclassOf<AEliteEnemyBase> EliteClass, FVector Center, float Radius, int32 Count)
{
	if (!IsServer() || EliteClass == nullptr || Count <= 0)
	{
		return;
	}

	// Deterministic ring placement — no unseeded random (CODING_STANDARDS §5). When the
	// seeded RNG stream exists (RunManager), jitter can be layered on through it.
	for (int32 i = 0; i < Count; ++i)
	{
		const float Angle = (2.f * PI * i) / Count;
		const FVector Offset(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.f);
		SpawnElite(EliteClass, Center + Offset, FRotator::ZeroRotator);
	}
}

void USpawnDirector::PrewarmElites(TSubclassOf<AEliteEnemyBase> EliteClass, int32 Count)
{
	if (!IsServer() || EliteClass == nullptr || Count <= 0)
	{
		return;
	}

	UClass* Key = EliteClass.Get();
	TArray<TWeakObjectPtr<AEliteEnemyBase>>& Free = FreePool.FindOrAdd(Key);
	for (int32 i = 0; i < Count; ++i)
	{
		if (AEliteEnemyBase* Elite = CreateNewElite(Key))
		{
			Elite->Deactivate();
			Free.Add(Elite);
		}
	}
}

void USpawnDirector::HandleEliteDeath(AActor* DeadActor)
{
	AEliteEnemyBase* Elite = Cast<AEliteEnemyBase>(DeadActor);
	if (Elite == nullptr)
	{
		return;
	}

	// Kill event BEFORE the recycle, while the corpse's class/location are still meaningful
	// (the upgrade loop reads XPValue and spawns the mini-boss chest where the boss fell).
	OnEnemyKilled.Broadcast(Elite);

	Elite->Deactivate();
	FreePool.FindOrAdd(Elite->GetClass()).Add(Elite);
}

void USpawnDirector::SpawnFodderWave(FVector Center, float Radius, int32 Count)
{
	// INTERIM cheap-Actor fodder (D-0003 fast path; Mass replaces this behind the same signature).
	// AFodderEnemy IS-A AEliteEnemyBase, so it reuses SpawnElite's pool/Activate/recycle path and
	// registers with the seam — taunt/barrage hit it for free; only GetEliteCount excludes it.
	if (!IsServer() || Count <= 0)
	{
		return;
	}

	// Deterministic ring placement (CODING_STANDARDS §5; seeded jitter via RunManager later).
	for (int32 i = 0; i < Count; ++i)
	{
		const float Angle = (2.f * PI * i) / Count;
		const FVector Offset(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.f);
		SpawnElite(AFodderEnemy::StaticClass(), Center + Offset, FRotator::ZeroRotator);
	}
}
