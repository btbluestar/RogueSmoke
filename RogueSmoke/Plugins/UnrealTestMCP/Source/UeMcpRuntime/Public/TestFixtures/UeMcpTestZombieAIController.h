// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Minimal AI controller used by the fixture zombie. On BeginPlay it issues a
// MoveToActor against the controlled pawn's TargetActor (if the pawn is a
// AUeMcpTestZombie and the target is set). Relies on a built navmesh in the
// arena — real QA tests supply one; battle tests that don't will simply see
// the zombie stand still, which is the honest failure mode.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "UeMcpTestZombieAIController.generated.h"

UCLASS(DisplayName="UeMcp Test Zombie AI Controller")
class UEMCPRUNTIME_API AUeMcpTestZombieAIController : public AAIController
{
    GENERATED_BODY()

public:
    AUeMcpTestZombieAIController();

protected:
    virtual void BeginPlay() override;
    virtual void OnPossess(APawn* InPawn) override;

private:
    void TryMoveToZombieTarget();
};
