// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Fixture zombie character. Skeletal mesh + anim blueprint are intentionally
// left unset — a real project supplies them via a derived BP or editor
// assignment; the C++ code only cares about the damage-on-contact loop and
// the AI navigation hookup (see AUeMcpTestZombieAIController).
//
// Uses a timer to apply `ContactDamage` at 1Hz while overlapping the
// `TargetActor`. Timer beats a per-tick check because damage should tick at
// a game-thread-independent rate regardless of framerate.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "UeMcpTestZombie.generated.h"

UCLASS(DisplayName="UeMcp Test Zombie")
class UEMCPRUNTIME_API AUeMcpTestZombie : public ACharacter
{
    GENERATED_BODY()

public:
    AUeMcpTestZombie();

    /** Actor the zombie tries to move to + melee. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Zombie")
    TObjectPtr<AActor> TargetActor;

    /** Damage applied to TargetActor once per second while overlapping. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|Zombie")
    float ContactDamage;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    UFUNCTION()
    void OnBodyBeginOverlap(UPrimitiveComponent* OverlappedComponent,
                            AActor* OtherActor,
                            UPrimitiveComponent* OtherComp,
                            int32 OtherBodyIndex,
                            bool bFromSweep,
                            const FHitResult& SweepResult);

    UFUNCTION()
    void OnBodyEndOverlap(UPrimitiveComponent* OverlappedComponent,
                          AActor* OtherActor,
                          UPrimitiveComponent* OtherComp,
                          int32 OtherBodyIndex);

    void ApplyContactDamageTick();

    FTimerHandle ContactDamageTimer;
    bool bIsOverlappingTarget = false;
};
