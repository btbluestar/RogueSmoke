// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Shared health state for the fixture content pack. Intentionally small:
// a float `Health` + `MaxHealth`, a `TakeDamage` helper that clamps to zero,
// and a multicast `OnHealthZero` delegate that fires once when the clamp hits.
//
// Kept in the Runtime module so tests can read/mutate via actor.properties
// or set_component_property without any editor-only dependency.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UeMcpHealthComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FUeMcpOnHealthZero, AActor*, KilledOwner);

UCLASS(ClassGroup=(UeMcpTest), meta=(BlueprintSpawnableComponent),
       DisplayName="UeMcp Health Component")
class UEMCPRUNTIME_API UUeMcpHealthComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UUeMcpHealthComponent();

    /** Current health. Reaches zero -> fires OnHealthZero once. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Health")
    float Health;

    /** Upper bound used by TakeDamage to clamp negative-damage healing. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Health")
    float MaxHealth;

    /** Fires once when Health transitions to <= 0. */
    UPROPERTY(BlueprintAssignable, Category="UeMcp|Health")
    FUeMcpOnHealthZero OnHealthZero;

    /** Subtracts `Damage` from Health, clamps to [0, MaxHealth], fires delegate at zero. */
    UFUNCTION(BlueprintCallable, Category="UeMcp|Health")
    void TakeDamage(float Damage);

private:
    bool bHasFiredZero = false;
};
