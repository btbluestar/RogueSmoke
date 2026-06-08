// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Stationary pawn that tracks `CurrentTarget` by yaw, then hitscans it with
// UGameplayStatics::ApplyDamage when the aim error is within tolerance and
// the cooldown has elapsed. Body StaticMesh + ArrowComponent muzzle — muzzle
// forward is the fire direction. Everything designer-tunable via UPROPERTYs
// so tests can poke the values via set_properties.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "UeMcpTestTurret.generated.h"

class UStaticMeshComponent;
class UArrowComponent;

UCLASS(DisplayName="UeMcp Test Turret")
class UEMCPRUNTIME_API AUeMcpTestTurret : public APawn
{
    GENERATED_BODY()

public:
    AUeMcpTestTurret();

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="UeMcp|Turret")
    TObjectPtr<UStaticMeshComponent> Body;

    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="UeMcp|Turret")
    TObjectPtr<UArrowComponent> Muzzle;

    /** Max degrees per second the turret rotates toward its target. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Turret")
    float TurnRateDegPerSec;

    /** Max distance (cm) at which the turret will fire. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Turret")
    float Range;

    /** Seconds between shots. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Turret")
    float FireCooldown;

    /** Damage per hitscan hit applied via UGameplayStatics::ApplyDamage. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Turret")
    float HitscanDamage;

    /** The actor the turret should aim at / shoot. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Turret")
    TObjectPtr<AActor> CurrentTarget;

    UFUNCTION(BlueprintCallable, Category="UeMcp|Turret")
    void SetTarget(AActor* NewTarget);

    virtual void Tick(float DeltaSeconds) override;

protected:
    virtual void BeginPlay() override;

private:
    float TimeSinceLastShot = 0.0f;

    void TryFireHitscan();
};
