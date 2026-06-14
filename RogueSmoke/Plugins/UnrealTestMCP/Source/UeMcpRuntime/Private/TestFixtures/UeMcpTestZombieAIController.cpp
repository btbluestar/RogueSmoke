// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "TestFixtures/UeMcpTestZombieAIController.h"
#include "TestFixtures/UeMcpTestZombie.h"

AUeMcpTestZombieAIController::AUeMcpTestZombieAIController()
{
    bStartAILogicOnPossess = true;
}

void AUeMcpTestZombieAIController::BeginPlay()
{
    Super::BeginPlay();
    TryMoveToZombieTarget();
}

void AUeMcpTestZombieAIController::OnPossess(APawn* InPawn)
{
    Super::OnPossess(InPawn);
    TryMoveToZombieTarget();
}

void AUeMcpTestZombieAIController::TryMoveToZombieTarget()
{
    if (AUeMcpTestZombie* Zombie = Cast<AUeMcpTestZombie>(GetPawn()))
    {
        if (Zombie->TargetActor)
        {
            MoveToActor(Zombie->TargetActor, /*AcceptanceRadius*/ 80.0f,
                        /*bStopOnOverlap*/ true,
                        /*bUsePathfinding*/ true,
                        /*bCanStrafe*/ false);
        }
    }
}
