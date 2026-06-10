// AttackingElite.cpp

#include "Enemies/AttackingElite.h"
#include "Combat/HealthComponent.h"
#include "Combat/CombatSubsystem.h"
#include "RogueHealthSet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"

AAttackingElite::AAttackingElite()
{
	// Self-contained visible body (mirrors AFodderEnemy): a static-mesh cube as the root, so an elite
	// is visible + hittable with no Blueprint. Replaced by a creature skeletal mesh on a BP later.
	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	SetRootComponent(Body);
	Body->SetMobility(EComponentMobility::Movable);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		Body->SetStaticMesh(CubeMesh.Object);
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Mat(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	if (Mat.Succeeded())
	{
		Body->SetMaterial(0, Mat.Object);
	}

	// Query-only collision: block the hitscan Visibility channel (so FireHitscan can kill it), overlap
	// everything else so elites/fodder/players don't shove each other around.
	Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Body->SetCollisionObjectType(ECC_WorldDynamic);
	Body->SetCollisionResponseToAllChannels(ECR_Overlap);
	Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
}

void AAttackingElite::ClearTransientState()
{
	Super::ClearTransientState();
	// Reset the attack loop so a recycled elite doesn't resume a stale telegraph / cooldown.
	Target = nullptr;
	AttackCooldown = 0.f;
	TelegraphRemaining = 0.f;
	bTelegraphing = false;
	bDashing = false;
	DashTimeRemaining = 0.f;
	bDashHitApplied = false;
}

void AAttackingElite::BeginPlay()
{
	Super::BeginPlay();

	Body->SetRelativeScale3D(BodyScale);

	// Per-archetype tint via a dynamic instance of BasicShapeMaterial (it exposes a "Color" param), so
	// Carapace/Spitter/Bloater/Lunger/boss read differently at a glance even as placeholder cubes.
	if (UMaterialInstanceDynamic* DynMat = Body->CreateDynamicMaterialInstance(0))
	{
		DynMat->SetVectorParameterValue(FName(TEXT("Color")), BodyColor);
	}

	if (MaxHealthOverride > 0.f && Health != nullptr)
	{
		Health->MaxHealth = MaxHealthOverride;
		Health->Health = MaxHealthOverride;
	}
}

void AAttackingElite::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds); // base pull steering runs while taunted
	if (bShowDebug)
	{
		DrawEnemyDebug();
	}
	if (!HasAuthority() || GetWorld() == nullptr)
	{
		return;
	}

	if (AttackCooldown > 0.f)
	{
		AttackCooldown -= DeltaSeconds;
	}

	AcquireTarget();

	// A dash in progress owns movement: slide it out (and bite on contact), ignoring approach/telegraph.
	if (bDashing)
	{
		UpdateDash(DeltaSeconds);
		return;
	}

	if (!Target.IsValid())
	{
		return;
	}

	// Resolve an in-flight telegraph first: the attack lands at the end of the wind-up, and whiffs if the
	// target left the attack range during it (so dodging / sliding out of a slam is real counterplay).
	if (bTelegraphing)
	{
		TelegraphRemaining -= DeltaSeconds;
		FaceTarget();
		if (TelegraphRemaining <= 0.f)
		{
			bTelegraphing = false;
			if (IsTargetInAttackRange())
			{
				PerformAttack();
			}
			AttackCooldown = AttackInterval;
		}
		return; // committed to the wind-up; hold position
	}

	if (IsBeingPulled())
	{
		return; // taunt owns our movement (the synergy SETUP); the C++ base Tick steers us
	}

	if (IsTargetInAttackRange())
	{
		FaceTarget();
		if (AttackCooldown <= 0.f)
		{
			bTelegraphing = true;
			TelegraphRemaining = TelegraphSeconds;
		}
	}
	else
	{
		ApproachTarget(DeltaSeconds);
	}
}

void AAttackingElite::StartDash(FVector Direction, float Speed, float Duration)
{
	FVector Dir = Direction;
	Dir.Z = 0.f;
	Dir = Dir.GetSafeNormal();
	if (Dir.IsNearlyZero() || Speed <= 0.f || Duration <= 0.f)
	{
		return;
	}
	DashDir = Dir;
	DashSpeed = Speed;
	DashTimeRemaining = Duration;
	bDashHitApplied = false;
	bDashing = true;
	SetActorRotation(Dir.Rotation());
}

void AAttackingElite::UpdateDash(float DeltaSeconds)
{
	DashTimeRemaining -= DeltaSeconds;
	SetActorLocation(GetActorLocation() + DashDir * DashSpeed * DeltaSeconds, /*bSweep=*/false);

	// Bite once, the first frame the dash brings us into contact with the (still-living) target.
	if (!bDashHitApplied && Target.IsValid())
	{
		if (FVector::Dist(Target->GetActorLocation(), GetActorLocation()) <= DashContactRange)
		{
			if (UCombatSubsystem* Combat = GetWorld() ? GetWorld()->GetSubsystem<UCombatSubsystem>() : nullptr)
			{
				Combat->ApplyDamageToPlayer(Target.Get(), AttackDamage, this);
			}
			bDashHitApplied = true;
		}
	}

	if (DashTimeRemaining <= 0.f)
	{
		bDashing = false;
	}
}

void AAttackingElite::AcquireTarget()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		Target = nullptr;
		return;
	}

	APawn* Best = nullptr;
	float BestDistSq = TNumericLimits<float>::Max();
	const FVector Mine = GetActorLocation();
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (Pawn == nullptr)
		{
			continue;
		}
		if (GetPawnHealth(Pawn) <= 0.f)
		{
			continue; // skip downed/dead heroes (0 HP) — focus the living
		}
		const float DistSq = FVector::DistSquared(Pawn->GetActorLocation(), Mine);
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Pawn;
		}
	}
	Target = Best;
}

float AAttackingElite::GetPawnHealth(APawn* P) const
{
	UAbilitySystemComponent* ASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(P);
	if (ASC == nullptr)
	{
		return 1.f; // unknown -> treat as alive
	}
	bool bFound = false;
	const float HealthValue = ASC->GetGameplayAttributeValue(URogueHealthSet::GetHealthAttribute(), bFound);
	return bFound ? HealthValue : 1.f;
}

bool AAttackingElite::IsTargetInAttackRange() const
{
	if (!Target.IsValid())
	{
		return false;
	}
	return FVector::Dist(Target->GetActorLocation(), GetActorLocation()) <= AttackRange;
}

void AAttackingElite::ApproachTarget(float DeltaSeconds)
{
	if (!Target.IsValid())
	{
		return;
	}
	const FVector Mine = GetActorLocation();
	FVector ToTarget = Target->GetActorLocation() - Mine;
	ToTarget.Z = 0.f;
	const float Dist = ToTarget.Size();
	if (Dist <= PreferredRange)
	{
		return;
	}
	const FVector Dir = ToTarget / Dist;
	SetActorLocation(Mine + Dir * MoveSpeed * DeltaSeconds, /*bSweep=*/false);
	SetActorRotation(Dir.Rotation());
}

void AAttackingElite::FaceTarget()
{
	if (!Target.IsValid())
	{
		return;
	}
	FVector ToTarget = Target->GetActorLocation() - GetActorLocation();
	ToTarget.Z = 0.f;
	if (!ToTarget.IsNearlyZero())
	{
		SetActorRotation(ToTarget.GetSafeNormal().Rotation());
	}
}

void AAttackingElite::PerformAttack_Implementation()
{
	UCombatSubsystem* Combat = GetWorld() ? GetWorld()->GetSubsystem<UCombatSubsystem>() : nullptr;
	if (Combat == nullptr)
	{
		return;
	}

	if (AttackRadius > 0.f)
	{
		Combat->ApplyRadialDamageToPlayers(GetActorLocation(), AttackRadius, AttackDamage, this);
	}
	else if (Target.IsValid())
	{
		// Ranged single-target (e.g. the Spitter): only land the hit if the shot has a clear line. Trace
		// from the elite's upper body — not its feet, which sit in the floor — so cover actually blocks it.
		const FVector From = GetActorLocation() + FVector(0.f, 0.f, 60.f);
		if (Combat->HasLineOfSightToActor(From, Target.Get(), this))
		{
			Combat->ApplyDamageToPlayer(Target.Get(), AttackDamage, this);
		}
		// else: blocked by cover -> the shot whiffs (intended counterplay: break line of sight)
	}
}

void AAttackingElite::DrawEnemyDebug() const
{
#if ENABLE_DRAW_DEBUG
	if (GetWorld() == nullptr)
	{
		return;
	}
	// Yellow = telegraphing (incoming hit), green = clustered (taunted), white = idle/normal.
	const FColor Color = bTelegraphing ? FColor(255, 215, 25) : (IsClustered() ? FColor::Green : FColor::White);
	DrawDebugSphere(GetWorld(), GetActorLocation() + FVector(0.f, 0.f, 160.f), 45.f, 10, Color, false, -1.f, 0, 2.f);
	if (bTelegraphing && AttackRadius > 0.f)
	{
		// Show the slam/explosion footprint during the wind-up so players can clear it.
		DrawDebugSphere(GetWorld(), GetActorLocation(), AttackRadius, 16, FColor(255, 100, 25), false, -1.f, 0, 1.5f);
	}

	// Live HP% above the head (green/yellow/red) — see damage land while debugging; doubles as the
	// Brood-mother's boss health readout. Billboards to the camera; refreshed each frame.
	if (Health != nullptr && Health->MaxHealth > 0.f)
	{
		const float Frac = FMath::Clamp(Health->Health / Health->MaxHealth, 0.f, 1.f);
		const FColor HpColor = Frac > 0.5f ? FColor::Green : (Frac > 0.25f ? FColor(255, 215, 25) : FColor::Red);
		const FString HpText = FString::Printf(TEXT("HP %d%%"), FMath::RoundToInt(Frac * 100.f));
		DrawDebugString(GetWorld(), GetActorLocation() + FVector(0.f, 0.f, 210.f), HpText, nullptr, HpColor, 0.f, true);
	}
#endif
}
