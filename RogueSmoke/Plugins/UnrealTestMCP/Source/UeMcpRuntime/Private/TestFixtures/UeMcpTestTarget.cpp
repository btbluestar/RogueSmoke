// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "TestFixtures/UeMcpTestTarget.h"
#include "TestFixtures/UeMcpHealthComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DamageEvents.h"

AUeMcpTestTarget::AUeMcpTestTarget()
{
    PrimaryActorTick.bCanEverTick = false;

    Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
    SetRootComponent(Mesh);
    // Block all by default so hitscan lines register hits.
    Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    Mesh->SetCollisionResponseToAllChannels(ECR_Block);
    Mesh->SetGenerateOverlapEvents(true);

    HealthComponent = CreateDefaultSubobject<UUeMcpHealthComponent>(TEXT("HealthComponent"));
}

void AUeMcpTestTarget::BeginPlay()
{
    Super::BeginPlay();
    if (HealthComponent)
    {
        HealthComponent->OnHealthZero.AddDynamic(this, &AUeMcpTestTarget::HandleHealthZero);
    }
}

float AUeMcpTestTarget::TakeDamage(float DamageAmount,
                                   FDamageEvent const& DamageEvent,
                                   AController* EventInstigator,
                                   AActor* DamageCauser)
{
    const float Actual = Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
    if (HealthComponent)
    {
        HealthComponent->TakeDamage(DamageAmount);
    }
    return Actual;
}

void AUeMcpTestTarget::HandleHealthZero(AActor* /*KilledOwner*/)
{
    Destroy();
}
