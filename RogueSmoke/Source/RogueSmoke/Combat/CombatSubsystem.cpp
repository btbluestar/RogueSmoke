// CombatSubsystem.cpp

#include "Combat/CombatSubsystem.h"
#include "Combat/HealthComponent.h"
#include "Enemies/EliteEnemyBase.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "CollisionQueryParams.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayEffect.h"
#include "RogueHealthSet.h"
#include "AbilitySystem/RoguePlayerState.h"

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

FVector UCombatSubsystem::ResolveAimPoint(FVector CamStart, FVector CamDir, float MaxDist, AActor* IgnoreActor) const
{
	const FVector End = CamStart + CamDir * MaxDist;

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return End;
	}

	// Visibility trace from the camera; the first thing under the crosshair (enemy or world geo) is the
	// convergence point. Ignore the shooter so a pitched-down camera can't converge on the player's own
	// body. No damage here — FireHitscan applies damage on the muzzle->aim trace.
	FCollisionQueryParams Params(FName(TEXT("ResolveAimPoint")), /*bTraceComplex=*/false, IgnoreActor);
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, CamStart, End, ECC_Visibility, Params))
	{
		return Hit.ImpactPoint;
	}
	return End;
}

bool UCombatSubsystem::HasLineOfSightToActor(FVector From, AActor* Target, AActor* IgnoreActor) const
{
	UWorld* World = GetWorld();
	if (World == nullptr || Target == nullptr)
	{
		return false;
	}

	const FVector To = Target->GetActorLocation();
	// Ignore the shooter and the target's own collision, so any *blocking* hit is geometry (or a third
	// actor) standing between them — i.e. the shot is obstructed. No damage here; callers gate on the bool.
	FCollisionQueryParams Params(FName(TEXT("EnemyLineOfSight")), /*bTraceComplex=*/false, IgnoreActor);
	Params.AddIgnoredActor(Target);

	FHitResult Hit;
	const bool bBlocked = World->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Params);
	return !bBlocked;
}

void UCombatSubsystem::ApplyDamageToPlayer(APawn* Target, float Damage, AActor* DamageInstigator)
{
	if (!IsServer() || Target == nullptr || Damage <= 0.f)
	{
		return;
	}

	UAbilitySystemComponent* ASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Target);
	if (ASC == nullptr)
	{
		return;
	}

	// Transient instant GE that adds to the Damage meta attribute; RogueHealthSet::PostGameplayEffectExecute
	// resolves Damage -> armor mitigation -> shield absorption -> Health. Mirrors how upgrades/abilities
	// apply effects, so player damage stays uniform and server-authoritative (no direct attribute pokes).
	// NAME_None -> auto-unique name; a fixed name would collide across repeated per-hit allocations.
	UGameplayEffect* DamageGE = NewObject<UGameplayEffect>(GetTransientPackage());
	DamageGE->DurationPolicy = EGameplayEffectDurationType::Instant;

	FGameplayModifierInfo Mod;
	Mod.Attribute = URogueHealthSet::GetDamageAttribute();
	Mod.ModifierOp = EGameplayModOp::Additive;
	Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Damage));
	DamageGE->Modifiers.Add(Mod);

	FGameplayEffectContextHandle Ctx = ASC->MakeEffectContext();
	Ctx.AddInstigator(DamageInstigator, DamageInstigator);
	ASC->ApplyGameplayEffectToSelf(DamageGE, 1.0f, Ctx);

	// Stat credit: incoming (pre-mitigation) damage on the victim's results-screen column.
	if (ARoguePlayerState* TargetPS = Target->GetPlayerState<ARoguePlayerState>())
	{
		TargetPS->AddDamageTaken(Damage);
	}
}

void UCombatSubsystem::ApplyRadialDamageToPlayers(FVector Center, float Radius, float Damage, AActor* DamageInstigator)
{
	UWorld* World = GetWorld();
	if (!IsServer() || World == nullptr || Damage <= 0.f)
	{
		return;
	}

	const float RadiusSq = Radius * Radius;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (Pawn == nullptr)
		{
			continue;
		}
		if (FVector::DistSquared(Pawn->GetActorLocation(), Center) <= RadiusSq)
		{
			ApplyDamageToPlayer(Pawn, Damage, DamageInstigator);
		}
	}
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
