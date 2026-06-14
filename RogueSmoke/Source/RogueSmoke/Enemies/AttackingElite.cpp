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
#include "Net/UnrealNetwork.h"
#include "DrawDebugHelpers.h"

namespace
{
	// Engine cylinder is radius 50: scale XY by R/50 for a world radius. Flattened to a thin disc.
	constexpr float RingMeshRadius = 50.f;
	constexpr float RingThickness = 0.02f;
	// Danger-ring radius for single-target (AttackRadius == 0) attacks: "this enemy is striking".
	constexpr float MeleeCueRadius = 140.f;
	constexpr float HitFlashSeconds = 0.09f;
}

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

	// Telegraph ground rings: absolute scale (so per-archetype BodyScale doesn't squash the discs),
	// no collision, hidden until a wind-up starts. Same mesh/material as TelegraphZoneFX.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> RingMat(TEXT("/Game/VFX/M_TelegraphRing.M_TelegraphRing"));
	const auto MakeRing = [&](const TCHAR* Name, float RelZ) -> UStaticMeshComponent*
	{
		UStaticMeshComponent* Ring = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Ring->SetupAttachment(Body);
		Ring->SetUsingAbsoluteScale(true);
		Ring->SetRelativeLocation(FVector(0.f, 0.f, RelZ));
		Ring->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ring->SetCastShadow(false);
		Ring->SetVisibility(false);
		Ring->SetMobility(EComponentMobility::Movable);
		if (CylinderMesh.Succeeded())
		{
			Ring->SetStaticMesh(CylinderMesh.Object);
		}
		if (RingMat.Succeeded())
		{
			Ring->SetMaterial(0, RingMat.Object);
		}
		return Ring;
	};
	TelegraphOutline = MakeRing(TEXT("TelegraphOutline"), 1.f);
	TelegraphFill = MakeRing(TEXT("TelegraphFill"), 2.f);
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

	EngageState = EEngageState::Background;
	TimeInEngageState = 0.f;
	bAttackedThisEngagement = false;

	// Cosmetics back to baseline so a pooled recycle doesn't wake up mid-flash/swell.
	LocalTelegraphElapsed = 0.f;
	FlashUntilSeconds = 0.f;
	if (Body != nullptr)
	{
		Body->SetRelativeScale3D(BodyScale);
	}
	if (BodyMID != nullptr)
	{
		BodyMID->SetVectorParameterValue(FName(TEXT("Color")), BodyColor);
	}
	if (TelegraphOutline != nullptr)
	{
		TelegraphOutline->SetVisibility(false);
	}
	if (TelegraphFill != nullptr)
	{
		TelegraphFill->SetVisibility(false);
	}
}

void AAttackingElite::BeginPlay()
{
	Super::BeginPlay();

	Body->SetRelativeScale3D(BodyScale);

	// Per-archetype tint via a dynamic instance of BasicShapeMaterial (it exposes a "Color" param), so
	// Carapace/Spitter/Bloater/Lunger/boss read differently at a glance even as placeholder cubes.
	// Kept as a member: the cue pass repaints it per-frame (warning pulse / hit flash / baseline).
	BodyMID = Body->CreateDynamicMaterialInstance(0);
	if (BodyMID != nullptr)
	{
		BodyMID->SetVectorParameterValue(FName(TEXT("Color")), BodyColor);
	}

	// Ring tints: shared warning orange outline; the fill MID also drives an opacity ramp.
	if (UMaterialInstanceDynamic* OutlineMID = TelegraphOutline->CreateDynamicMaterialInstance(0))
	{
		OutlineMID->SetVectorParameterValue(FName(TEXT("Color")), FLinearColor(1.f, 0.35f, 0.05f));
		OutlineMID->SetScalarParameterValue(FName(TEXT("Opacity")), 0.18f);
	}
	TelegraphFillMID = TelegraphFill->CreateDynamicMaterialInstance(0);
	if (TelegraphFillMID != nullptr)
	{
		TelegraphFillMID->SetVectorParameterValue(FName(TEXT("Color")), FLinearColor(1.f, 0.08f, 0.03f));
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
	UpdateCombatCosmetics(DeltaSeconds); // every machine — replicated bTelegraphing drives the cues
	if (!HasAuthority() || GetWorld() == nullptr)
	{
		return;
	}

	if (AttackCooldown > 0.f)
	{
		AttackCooldown -= DeltaSeconds;
	}
	TimeInEngageState += DeltaSeconds;

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
			bAttackedThisEngagement = true; // committed this engagement (even a whiff) -> director can release
			AttackCooldown = AttackInterval;
		}
		return; // committed to the wind-up; hold position
	}

	if (IsBeingPulled())
	{
		return; // taunt owns our movement (the synergy SETUP); the C++ base Tick steers us
	}

	// Token-gating: a token-using elite only attacks while Engaged; in Background it holds at the ring.
	const bool bMayAttack = !bUsesAttackToken || EngageState == EEngageState::Engaged;
	if (bMayAttack && IsTargetInAttackRange())
	{
		FaceTarget();
		if (AttackCooldown <= 0.f)
		{
			bTelegraphing = true;
			TelegraphRemaining = TelegraphSeconds;
			OnTelegraphStarted(); // subclass hook: lock targets / ring attack-specific zones
		}
	}
	else
	{
		const float StopRange = (bUsesAttackToken && EngageState == EEngageState::Background)
			? GetRingStandoff() : PreferredRange;
		ApproachTarget(DeltaSeconds, StopRange);
	}
}

void AAttackingElite::OnRep_Telegraphing()
{
	// Fresh wind-up on this client: restart the local progress clock the cosmetics animate from.
	LocalTelegraphElapsed = 0.f;
}

void AAttackingElite::NotifyHealthVisual(bool bDamaged, bool bDied)
{
	Super::NotifyHealthVisual(bDamaged, bDied); // death burst
	if (bDamaged && GetWorld() != nullptr)
	{
		FlashUntilSeconds = GetWorld()->GetTimeSeconds() + HitFlashSeconds;
	}
}

void AAttackingElite::UpdateCombatCosmetics(float DeltaSeconds)
{
	UWorld* World = GetWorld();
	if (World == nullptr || Body == nullptr)
	{
		return;
	}
	const float Now = World->GetTimeSeconds();

	// Wind-up progress 0->1: the server reads its countdown, clients accumulate from the OnRep.
	float Progress = 0.f;
	if (bTelegraphing && TelegraphSeconds > 0.f)
	{
		if (HasAuthority())
		{
			Progress = FMath::Clamp(1.f - TelegraphRemaining / TelegraphSeconds, 0.f, 1.f);
		}
		else
		{
			LocalTelegraphElapsed += DeltaSeconds;
			Progress = FMath::Clamp(LocalTelegraphElapsed / TelegraphSeconds, 0.f, 1.f);
		}
	}

	// Ground rings: danger footprint + a fill that touches the edge exactly at impact (the dodge
	// timer). Single-target attacks get a small "striking" disc instead of an AoE footprint.
	if (TelegraphOutline != nullptr && TelegraphFill != nullptr)
	{
		TelegraphOutline->SetVisibility(bTelegraphing);
		TelegraphFill->SetVisibility(bTelegraphing);
		if (bTelegraphing)
		{
			const float DangerRadius = AttackRadius > 0.f ? AttackRadius : MeleeCueRadius;
			const float OuterScale = DangerRadius / RingMeshRadius;
			TelegraphOutline->SetWorldScale3D(FVector(OuterScale, OuterScale, RingThickness));
			TelegraphFill->SetWorldScale3D(FVector(OuterScale * Progress, OuterScale * Progress, RingThickness));
			if (TelegraphFillMID != nullptr)
			{
				TelegraphFillMID->SetScalarParameterValue(FName(TEXT("Opacity")), 0.22f + 0.18f * Progress);
			}
		}
	}

	// Body color: hit flash beats the warning pulse beats the archetype baseline.
	if (BodyMID != nullptr)
	{
		FLinearColor BodyTint = BodyColor;
		if (Now < FlashUntilSeconds)
		{
			BodyTint = FLinearColor::White;
		}
		else if (bTelegraphing)
		{
			const float Pulse = 0.5f + 0.5f * FMath::Sin(Now * 14.f);
			BodyTint = FMath::Lerp(BodyColor, FLinearColor(1.f, 0.85f, 0.1f), 0.35f + 0.45f * Pulse);
		}
		BodyMID->SetVectorParameterValue(FName(TEXT("Color")), BodyTint);
	}

	// Swell tell (the Bloater): grow toward TelegraphSwell across the wind-up, snap back after.
	const float Swell = bTelegraphing ? FMath::Lerp(1.f, FMath::Max(TelegraphSwell, 1.f), Progress) : 1.f;
	Body->SetRelativeScale3D(BodyScale * Swell);
}

void AAttackingElite::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AAttackingElite, bTelegraphing);
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

void AAttackingElite::ApproachTarget(float DeltaSeconds, float StopRange)
{
	if (!Target.IsValid())
	{
		return;
	}
	const FVector Mine = GetActorLocation();
	FVector ToTarget = Target->GetActorLocation() - Mine;
	ToTarget.Z = 0.f;
	const float Dist = ToTarget.Size();
	if (Dist <= StopRange)
	{
		return;
	}
	const FVector Dir = ToTarget / Dist;
	SetActorLocation(Mine + Dir * MoveSpeed * DeltaSeconds, /*bSweep=*/false);
	SetActorRotation(Dir.Rotation());
}

void AAttackingElite::SetEngageState(EEngageState NewState)
{
	if (NewState == EngageState)
	{
		return;
	}
	EngageState = NewState;
	TimeInEngageState = 0.f;
	if (NewState == EEngageState::Engaged)
	{
		bAttackedThisEngagement = false;
	}
}

bool AAttackingElite::IsAlive() const
{
	return Health == nullptr || Health->Health > 0.f;
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
	// Telegraph progress 0->1 across the wind-up, so the readout reads as a countdown to the hit.
	const float TeleProgress = (bTelegraphing && TelegraphSeconds > 0.f)
		? FMath::Clamp(1.f - TelegraphRemaining / TelegraphSeconds, 0.f, 1.f) : 0.f;

	// Yellow = telegraphing (incoming hit), green = clustered (taunted), white = idle/normal. The head
	// marker grows as the strike nears, so the wind-up reads as a timer even without VFX.
	const FColor Color = bTelegraphing ? FColor(255, 215, 25) : (IsClustered() ? FColor::Green : FColor::White);
	const float HeadRadius = bTelegraphing ? FMath::Lerp(25.f, 70.f, TeleProgress) : 45.f;
	DrawDebugSphere(GetWorld(), GetActorLocation() + FVector(0.f, 0.f, 160.f), HeadRadius, 10, Color, false, -1.f, 0, 2.f);
	if (bTelegraphing && AttackRadius > 0.f)
	{
		// Danger footprint (outline) + a filling zone that reaches the edge exactly at impact, so you can
		// time the dodge: clear the area before the inner zone touches the outline.
		DrawDebugSphere(GetWorld(), GetActorLocation(), AttackRadius, 16, FColor(255, 100, 25), false, -1.f, 0, 1.5f);
		DrawDebugSphere(GetWorld(), GetActorLocation(), AttackRadius * TeleProgress, 16, FColor::Red, false, -1.f, 0, 2.5f);
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
