// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Damageable target actor used as the subject of turret/zombie fixture tests.
// Static mesh + collision + UUeMcpHealthComponent. ReceiveAnyDamage routes
// the incoming scalar into the health component; when the component's
// OnHealthZero fires the actor Destroy()s itself.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "UeMcpTestTarget.generated.h"

class UStaticMeshComponent;
class UUeMcpHealthComponent;

UCLASS(DisplayName="UeMcp Test Target")
class UEMCPRUNTIME_API AUeMcpTestTarget : public AActor
{
    GENERATED_BODY()

public:
    AUeMcpTestTarget();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="UeMcp|Target")
    TObjectPtr<UStaticMeshComponent> Mesh;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="UeMcp|Target")
    TObjectPtr<UUeMcpHealthComponent> HealthComponent;

    virtual float TakeDamage(float DamageAmount,
                             struct FDamageEvent const& DamageEvent,
                             class AController* EventInstigator,
                             AActor* DamageCauser) override;

protected:
    virtual void BeginPlay() override;

private:
    UFUNCTION()
    void HandleHealthZero(AActor* KilledOwner);
};
