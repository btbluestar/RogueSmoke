// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "TestFixtures/UeMcpTestZombie.h"
#include "TestFixtures/UeMcpTestZombieAIController.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "TimerManager.h"

AUeMcpTestZombie::AUeMcpTestZombie()
{
    PrimaryActorTick.bCanEverTick = false;

    ContactDamage = 5.0f;
    TargetActor = nullptr;

    AIControllerClass = AUeMcpTestZombieAIController::StaticClass();
    AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;

    if (UCapsuleComponent* Capsule = GetCapsuleComponent())
    {
        Capsule->SetGenerateOverlapEvents(true);
    }
}

void AUeMcpTestZombie::BeginPlay()
{
    Super::BeginPlay();

    if (UCapsuleComponent* Capsule = GetCapsuleComponent())
    {
        Capsule->OnComponentBeginOverlap.AddDynamic(this, &AUeMcpTestZombie::OnBodyBeginOverlap);
        Capsule->OnComponentEndOverlap.AddDynamic(this, &AUeMcpTestZombie::OnBodyEndOverlap);
    }
}

void AUeMcpTestZombie::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(ContactDamageTimer);
    }
    Super::EndPlay(EndPlayReason);
}

void AUeMcpTestZombie::OnBodyBeginOverlap(UPrimitiveComponent* /*OverlappedComponent*/,
                                          AActor* OtherActor,
                                          UPrimitiveComponent* /*OtherComp*/,
                                          int32 /*OtherBodyIndex*/,
                                          bool /*bFromSweep*/,
                                          const FHitResult& /*SweepResult*/)
{
    if (!OtherActor || OtherActor != TargetActor)
    {
        return;
    }
    if (bIsOverlappingTarget)
    {
        return;
    }
    bIsOverlappingTarget = true;

    if (UWorld* World = GetWorld())
    {
        // First tick lands on the next 1s boundary; no immediate hit so a
        // brief brush-past doesn't deal damage.
        World->GetTimerManager().SetTimer(
            ContactDamageTimer, this,
            &AUeMcpTestZombie::ApplyContactDamageTick,
            1.0f, /*bLoop*/ true, /*FirstDelay*/ 1.0f);
    }
}

void AUeMcpTestZombie::OnBodyEndOverlap(UPrimitiveComponent* /*OverlappedComponent*/,
                                        AActor* OtherActor,
                                        UPrimitiveComponent* /*OtherComp*/,
                                        int32 /*OtherBodyIndex*/)
{
    if (!OtherActor || OtherActor != TargetActor)
    {
        return;
    }
    bIsOverlappingTarget = false;
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(ContactDamageTimer);
    }
}

void AUeMcpTestZombie::ApplyContactDamageTick()
{
    if (!bIsOverlappingTarget || !TargetActor || !IsValid(TargetActor))
    {
        return;
    }
    UGameplayStatics::ApplyDamage(TargetActor, ContactDamage, GetController(), this, nullptr);
}
