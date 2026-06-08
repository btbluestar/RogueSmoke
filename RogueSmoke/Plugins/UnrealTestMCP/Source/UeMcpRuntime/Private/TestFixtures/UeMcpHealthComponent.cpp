// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "TestFixtures/UeMcpHealthComponent.h"

UUeMcpHealthComponent::UUeMcpHealthComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    Health = 100.0f;
    MaxHealth = 100.0f;
}

void UUeMcpHealthComponent::TakeDamage(float Damage)
{
    if (bHasFiredZero)
    {
        return;
    }

    Health = FMath::Clamp(Health - Damage, 0.0f, MaxHealth);

    if (Health <= 0.0f && !bHasFiredZero)
    {
        bHasFiredZero = true;
        OnHealthZero.Broadcast(GetOwner());
    }
}
