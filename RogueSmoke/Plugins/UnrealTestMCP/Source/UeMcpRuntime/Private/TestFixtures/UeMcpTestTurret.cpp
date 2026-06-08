// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "TestFixtures/UeMcpTestTurret.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ArrowComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/DamageEvents.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeMcpTurret, Log, All);

AUeMcpTestTurret::AUeMcpTestTurret()
{
    PrimaryActorTick.bCanEverTick = true;

    Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
    SetRootComponent(Body);
    Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Body->SetCollisionResponseToAllChannels(ECR_Block);

    Muzzle = CreateDefaultSubobject<UArrowComponent>(TEXT("Muzzle"));
    Muzzle->SetupAttachment(Body);
    Muzzle->SetRelativeLocation(FVector(50.0f, 0.0f, 0.0f));

    TurnRateDegPerSec = 90.0f;
    Range = 1000.0f;
    FireCooldown = 0.5f;
    HitscanDamage = 25.0f;
    CurrentTarget = nullptr;
}

void AUeMcpTestTurret::BeginPlay()
{
    Super::BeginPlay();
    // Ensure cooldown is considered elapsed when play begins so first shot
    // lands ~immediately after acquiring a target.
    TimeSinceLastShot = FireCooldown;
}

void AUeMcpTestTurret::SetTarget(AActor* NewTarget)
{
    CurrentTarget = NewTarget;
}

void AUeMcpTestTurret::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    TimeSinceLastShot += DeltaSeconds;

    if (!CurrentTarget || !IsValid(CurrentTarget))
    {
        return;
    }

    const FVector MyLoc = GetActorLocation();
    const FVector TgtLoc = CurrentTarget->GetActorLocation();
    FVector ToTarget = TgtLoc - MyLoc;
    const float DistSq = ToTarget.SizeSquared();
    if (DistSq < KINDA_SMALL_NUMBER)
    {
        return;
    }

    // Yaw-only turning. Use desired yaw and slerp at TurnRateDegPerSec.
    const FRotator CurRot = GetActorRotation();
    const FRotator DesiredRot = ToTarget.Rotation();
    const float YawDelta = FMath::FindDeltaAngleDegrees(CurRot.Yaw, DesiredRot.Yaw);
    const float MaxStep = TurnRateDegPerSec * DeltaSeconds;
    const float Step = FMath::Clamp(YawDelta, -MaxStep, MaxStep);
    FRotator NewRot = CurRot;
    NewRot.Yaw += Step;
    SetActorRotation(NewRot);

    // Aim error = angle between muzzle forward and vector-to-target (on yaw plane).
    const FVector MuzzleFwd = Muzzle ? Muzzle->GetForwardVector() : GetActorForwardVector();
    const FVector ToTargetNorm = ToTarget.GetSafeNormal();
    const float CosErr = FVector::DotProduct(MuzzleFwd, ToTargetNorm);
    const float AimErrorDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(CosErr, -1.0f, 1.0f)));

    const bool bInRange = DistSq <= (Range * Range);
    const bool bAimed = AimErrorDeg <= 5.0f;
    const bool bCooledDown = TimeSinceLastShot >= FireCooldown;

    UE_LOG(LogUeMcpTurret, VeryVerbose,
        TEXT("Tick: dist=%.1f aimErr=%.2f cool=%.2f inRange=%d aimed=%d cd=%d"),
        FMath::Sqrt(DistSq), AimErrorDeg, TimeSinceLastShot,
        bInRange ? 1 : 0, bAimed ? 1 : 0, bCooledDown ? 1 : 0);

    if (bInRange && bAimed && bCooledDown)
    {
        UE_LOG(LogUeMcpTurret, Verbose, TEXT("Firing at %s"),
               CurrentTarget ? *CurrentTarget->GetName() : TEXT("<null>"));
        TryFireHitscan();
        TimeSinceLastShot = 0.0f;
    }
}

void AUeMcpTestTurret::TryFireHitscan()
{
    UWorld* World = GetWorld();
    if (!World || !Muzzle)
    {
        return;
    }

    const FVector Start = Muzzle->GetComponentLocation();
    const FVector Dir = Muzzle->GetForwardVector();
    const FVector End = Start + Dir * Range;

    FHitResult Hit;
    FCollisionQueryParams Params(SCENE_QUERY_STAT(UeMcpTurretHitscan), /*bTraceComplex*/ false, this);
    Params.AddIgnoredActor(this);

    const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params);
    if (bHit && Hit.GetActor())
    {
        FDamageEvent DmgEvent;
        UGameplayStatics::ApplyDamage(Hit.GetActor(), HitscanDamage, GetInstigatorController(), this, nullptr);
    }
    else if (CurrentTarget)
    {
        // Fallback: target may not be trace-visible (no mesh yet) but the
        // fixture still wants the damage pipeline exercised in tests.
        UGameplayStatics::ApplyDamage(CurrentTarget, HitscanDamage, GetInstigatorController(), this, nullptr);
    }
}
