// CombatSubsystem.cpp

#include "Combat/CombatSubsystem.h"
#include "Combat/HealthComponent.h"
#include "Enemies/EliteEnemyBase.h"
#include "VFX/TelegraphZoneFX.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "CollisionQueryParams.h"
#include "DrawDebugHelpers.h"
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

	// Iterate a SNAPSHOT: a kill inside ApplyDamage fires OnDeath -> pool Deactivate ->
	// UnregisterElite, which RemoveAll()s from Elites — mutating the live array mid-iteration
	// skipped every neighbor of a kill (half the blast's targets survived).
	const TArray<TWeakObjectPtr<AEliteEnemyBase>> Snapshot = Elites;

	const float RadiusSq = Radius * Radius;
	float TotalDealt = 0.f;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Snapshot)
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

int32 UCombatSubsystem::ApplyDotInSphere(FVector Center, float Radius, ERogueDotType Type,
                                         float DamagePerSecond, float Duration, AActor* DamageInstigator)
{
	if (!IsServer() || Radius <= 0.f || DamagePerSecond <= 0.f || Duration <= 0.f)
	{
		return 0;
	}

	// Snapshot for the same reason ApplyRadialDamage does: registry mutation safety.
	const TArray<TWeakObjectPtr<AEliteEnemyBase>> Snapshot = Elites;
	const float RadiusSq = Radius * Radius;
	int32 Applied = 0;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Snapshot)
	{
		AEliteEnemyBase* Elite = Ptr.Get();
		if (Elite == nullptr || Elite->Health == nullptr || Elite->Health->IsDead())
		{
			continue;   // the bursting corpse itself is dead -> skipped
		}
		if (FVector::DistSquared(Elite->GetActorLocation(), Center) <= RadiusSq)
		{
			Elite->Health->ApplyDot(Type, DamagePerSecond, Duration, DamageInstigator);
			++Applied;
		}
	}
	return Applied;
}

void UCombatSubsystem::GrantShieldToSquad(float Amount)
{
	UWorld* World = GetWorld();
	if (!IsServer() || World == nullptr || Amount <= 0.f)
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (Pawn == nullptr)
		{
			continue;
		}
		UAbilitySystemComponent* ASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(Pawn);
		if (ASC == nullptr)
		{
			continue;
		}

		// Clamp in C++ so an evolution can never overfill past MaxShield, mirroring the
		// transient-GE pattern of ApplyDamageToPlayer (no direct attribute pokes).
		const float Shield = ASC->GetNumericAttribute(URogueHealthSet::GetShieldAttribute());
		const float MaxShield = ASC->GetNumericAttribute(URogueHealthSet::GetMaxShieldAttribute());
		const float Delta = FMath::Min(Amount, FMath::Max(0.f, MaxShield - Shield));
		if (Delta <= 0.f)
		{
			continue;
		}

		UGameplayEffect* ShieldGE = NewObject<UGameplayEffect>(GetTransientPackage());
		ShieldGE->DurationPolicy = EGameplayEffectDurationType::Instant;
		FGameplayModifierInfo Mod;
		Mod.Attribute = URogueHealthSet::GetShieldAttribute();
		Mod.ModifierOp = EGameplayModOp::Additive;
		Mod.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Delta));
		ShieldGE->Modifiers.Add(Mod);
		ASC->ApplyGameplayEffectToSelf(ShieldGE, 1.0f, ASC->MakeEffectContext());
	}
}

void UCombatSubsystem::ShowTelegraphZone(FVector Center, float Radius, float DurationSeconds)
{
	if (!IsServer() || Radius <= 0.f)
	{
		return;
	}
	if (ATelegraphZoneFX* Zone = ATelegraphZoneFX::SpawnZone(GetWorld(), Center, Radius, DurationSeconds))
	{
		// Breadcrumb for headless verification (cosmetics can't be screenshotted under nullrhi).
		UE_LOG(LogTemp, Log, TEXT("[Telegraph] zone r=%.0f dur=%.2fs at %s"), Radius, DurationSeconds, *Center.ToCompactString());
	}
}

FHitscanResult UCombatSubsystem::FireHitscan(FVector Start, FVector End, float Damage, AActor* DamageInstigator)
{
	// Thin wrapper: default params reproduce the historical single-trace behavior exactly.
	FWeaponShotParams Params;
	Params.Damage = Damage;
	return FireWeaponShot(Start, End, Params, DamageInstigator);
}

FHitscanResult UCombatSubsystem::FireWeaponShot(FVector Start, FVector End, const FWeaponShotParams& Params,
                                                AActor* DamageInstigator)
{
	FHitscanResult Result;
	Result.ImpactPoint = End; // default: nothing hit -> tracer reaches the trace end

	UWorld* World = GetWorld();
	if (World == nullptr || !IsServer())
	{
		return Result;
	}

	FCollisionQueryParams QueryParams(FName(TEXT("FireWeaponShot")), /*bTraceComplex=*/false, DamageInstigator);
	QueryParams.AddIgnoredActor(DamageInstigator);

	// Pierce loop: re-trace past each enemy hit (the victim joins the ignore list, so no epsilon
	// nudging). World geometry — or any non-enemy actor — always stops the bullet.
	FVector TraceStart = Start;
	int32 RemainingPierces = FMath::Max(0, Params.PierceCount);

	for (;;)
	{
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, TraceStart, End, ECC_Visibility, QueryParams))
		{
			// Flew to max range (possibly after piercing bodies): the tracer ends at the trace end.
			Result.ImpactPoint = End;
			break;
		}

		Result.bBlockingHit = true;
		Result.ImpactPoint = Hit.ImpactPoint;
		Result.ImpactNormal = Hit.ImpactNormal;
		if (Result.HitActor == nullptr)
		{
			Result.HitActor = Hit.GetActor(); // first thing struck, matching FireHitscan semantics
		}

		// Actor-elite backend (D-0003). The Mass-agent raycast resolves here later, behind the same call.
		AEliteEnemyBase* Elite = Cast<AEliteEnemyBase>(Hit.GetActor());
		if (Elite == nullptr || Elite->Health == nullptr)
		{
			break;
		}

		if (Result.bHitEnemy)
		{
			Result.ExtraEnemiesHit++; // a pierced body beyond the first victim
		}
		Result.bHitEnemy = true;
		Result.DamageDealt += Elite->Health->ApplyDamage(Params.Damage, DamageInstigator);

		ProcOnHitEffects(Elite, Params, DamageInstigator, Result);

		if (RemainingPierces <= 0)
		{
			break;
		}
		RemainingPierces--;
		QueryParams.AddIgnoredActor(Elite);
		TraceStart = Hit.ImpactPoint;
	}

	return Result;
}

void UCombatSubsystem::ProcOnHitEffects(AEliteEnemyBase* Victim, const FWeaponShotParams& Params,
                                        AActor* DamageInstigator, FHitscanResult& Result)
{
	// Status procs: every directly-hit enemy rolls independently (per pellet, per pierced body).
	// Unseeded FRand is fine here — combat resolves server-side only, like the spread VRandCone;
	// the procgen determinism rule covers world generation, not authoritative combat rolls.
	if (Params.BurnChance > 0.f && Params.BurnDuration > 0.f && FMath::FRand() < Params.BurnChance)
	{
		Victim->Health->ApplyDot(ERogueDotType::Burn,
		                         Params.Damage * Params.BurnDamageFraction / Params.BurnDuration,
		                         Params.BurnDuration, DamageInstigator);
	}
	if (Params.PoisonChance > 0.f && Params.PoisonDuration > 0.f && FMath::FRand() < Params.PoisonChance)
	{
		Victim->Health->ApplyDot(ERogueDotType::Poison,
		                         Params.Damage * Params.PoisonDamageFraction / Params.PoisonDuration,
		                         Params.PoisonDuration, DamageInstigator);
	}

	// Chain arcs (RoR2-ukulele shape): nearest arcs within ChainRadius of the victim take a
	// damage fraction. The Overwhelm evolution adds arcs when the victim is Clustered — the
	// taunt->gun loop. Arcs never re-chain or re-pierce; Searing Arcs may add a Burn DoT to
	// arc targets (still no recursive procs), so the cascade stays bounded.
	const int32 EffectiveChainCount = Params.ChainCount +
		(Victim->IsClustered() ? FMath::Max(0, Params.ClusterChainBonusArcs) : 0);
	if (EffectiveChainCount <= 0 || Params.ChainDamageFraction <= 0.f)
	{
		return;
	}

	const FVector Origin = Victim->GetActorLocation();
	const float RadiusSq = Params.ChainRadius * Params.ChainRadius;

	TArray<AEliteEnemyBase*> Candidates;
	for (const TWeakObjectPtr<AEliteEnemyBase>& Ptr : Elites)
	{
		AEliteEnemyBase* Other = Ptr.Get();
		if (Other == nullptr || Other == Victim || Other->Health == nullptr || Other->Health->IsDead())
		{
			continue;
		}
		if (FVector::DistSquared(Other->GetActorLocation(), Origin) <= RadiusSq)
		{
			Candidates.Add(Other);
		}
	}

	Candidates.Sort([&Origin](const AEliteEnemyBase& A, const AEliteEnemyBase& B)
	{
		return FVector::DistSquared(A.GetActorLocation(), Origin) <
		       FVector::DistSquared(B.GetActorLocation(), Origin);
	});

	const int32 Arcs = FMath::Min(EffectiveChainCount, Candidates.Num());
	const float ChainDamage = Params.Damage * Params.ChainDamageFraction;
	for (int32 i = 0; i < Arcs; ++i)
	{
		Result.DamageDealt += Candidates[i]->Health->ApplyDamage(ChainDamage, DamageInstigator);
		Result.ExtraEnemiesHit++;

		// Searing Arcs: the arc ignites its target (magnitude derives from the arc's damage,
		// same Gunfire-Reborn model as the on-hit procs above).
		if (Params.ChainIgniteFraction > 0.f && Params.BurnDuration > 0.f && !Candidates[i]->Health->IsDead())
		{
			Candidates[i]->Health->ApplyDot(ERogueDotType::Burn,
			                                ChainDamage * Params.ChainIgniteFraction / Params.BurnDuration,
			                                Params.BurnDuration, DamageInstigator);
		}

		// MVP readability: debug arc on the host. The Niagara beam lands with the cue pass (#39).
		DrawDebugLine(GetWorld(), Origin + FVector(0, 0, 50.f),
		              Candidates[i]->GetActorLocation() + FVector(0, 0, 50.f),
		              FColor::Cyan, false, 0.2f, 0, 2.f);
	}
}
