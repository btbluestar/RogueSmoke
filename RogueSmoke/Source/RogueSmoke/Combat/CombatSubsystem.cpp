// CombatSubsystem.cpp

#include "Combat/CombatSubsystem.h"
#include "Combat/HealthComponent.h"
#include "Enemies/EliteEnemyBase.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "CollisionQueryParams.h"

bool UCombatSubsystem::IsServer() const
{
	const UWorld* World = GetWorld();
	return World != nullptr && World->GetNetMode() != NM_Client;
}

void UCombatSubsystem::RegisterElite(AEliteEnemyBase* Elite)
{
	if (Elite != nullptr)
	{
		Elites.AddUnique(Elite);
	}
}

void UCombatSubsystem::UnregisterElite(AEliteEnemyBase* Elite)
{
	Elites.RemoveAll([Elite](const TWeakObjectPtr<AEliteEnemyBase>& Ptr)
	{
		return !Ptr.IsValid() || Ptr.Get() == Elite;
	});
}

int32 UCombatSubsystem::CountEnemiesInSphere(FVector Center, float Radius) const
{
	const float RadiusSq = Radius * Radius;
	int32 Count = 0;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Elites)
	{
		if (const AEliteEnemyBase* Elite = Ptr.Get())
		{
			if (FVector::DistSquared(Elite->GetActorLocation(), Center) <= RadiusSq)
			{
				++Count;
			}
		}
	}
	return Count;
}

int32 UCombatSubsystem::GetEliteCount() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Elites)
	{
		// Only true elites/bosses gate "arena cleared" — fodder shares this registry (so AoE/pull
		// hit it) but must not keep extraction from opening. CountEnemiesInSphere still counts all.
		if (const AEliteEnemyBase* Enemy = Ptr.Get())
		{
			if (Enemy->CountsAsObjectiveTarget())
			{
				++Count;
			}
		}
	}
	return Count;
}

void UCombatSubsystem::PullEnemiesToward(FVector Center, float Radius, float Strength, float Duration)
{
	if (!IsServer())
	{
		return;
	}

	const float RadiusSq = Radius * Radius;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Elites)
	{
		if (AEliteEnemyBase* Elite = Ptr.Get())
		{
			if (FVector::DistSquared(Elite->GetActorLocation(), Center) <= RadiusSq)
			{
				Elite->ApplyPull(Center, Strength, Duration);
			}
		}
	}
}

void UCombatSubsystem::MarkClustered(FVector Center, float Radius, float Duration)
{
	if (!IsServer())
	{
		return;
	}

	const float RadiusSq = Radius * Radius;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Elites)
	{
		if (AEliteEnemyBase* Elite = Ptr.Get())
		{
			if (FVector::DistSquared(Elite->GetActorLocation(), Center) <= RadiusSq)
			{
				Elite->MarkClustered(Duration);
			}
		}
	}
}

float UCombatSubsystem::ApplyRadialDamage(FVector Center, float Radius, float BaseDamage,
                                          float ClusterBonusMultiplier, AActor* DamageInstigator)
{
	if (!IsServer())
	{
		return 0.f;
	}

	const float RadiusSq = Radius * Radius;
	float TotalDealt = 0.f;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Elites)
	{
		AEliteEnemyBase* Elite = Ptr.Get();
		if (Elite == nullptr || Elite->Health == nullptr)
		{
			continue;
		}

		if (FVector::DistSquared(Elite->GetActorLocation(), Center) <= RadiusSq)
		{
			const float Damage = Elite->IsClustered() ? BaseDamage * ClusterBonusMultiplier : BaseDamage;
			TotalDealt += Elite->Health->ApplyDamage(Damage, DamageInstigator);
		}
	}
	return TotalDealt;
}

FHitscanResult UCombatSubsystem::FireHitscan(FVector Start, FVector End, float Damage, AActor* DamageInstigator)
{
	FHitscanResult Result;
	Result.ImpactPoint = End; // default: nothing hit -> tracer reaches the trace end

	UWorld* World = GetWorld();
	if (World == nullptr || !IsServer())
	{
		return Result;
	}

	FCollisionQueryParams Params(FName(TEXT("FireHitscan")), /*bTraceComplex=*/false, DamageInstigator);
	Params.AddIgnoredActor(DamageInstigator);

	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
	{
		Result.bBlockingHit = true;
		Result.ImpactPoint = Hit.ImpactPoint;
		Result.ImpactNormal = Hit.ImpactNormal;
		Result.HitActor = Hit.GetActor();

		// Actor-elite backend (D-0003). The Mass-agent raycast resolves here later, behind the same call.
		if (AEliteEnemyBase* Elite = Cast<AEliteEnemyBase>(Hit.GetActor()))
		{
			if (Elite->Health != nullptr)
			{
				Result.DamageDealt = Elite->Health->ApplyDamage(Damage, DamageInstigator);
				Result.bHitEnemy = true;
			}
		}
	}

	return Result;
}
