// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpFixtureActor.h"

AUeMcpFixtureActor::AUeMcpFixtureActor()
{
	// No tick; fixture is inert. Minimises overhead when a test spawns
	// and destroys many instances in quick succession.
	PrimaryActorTick.bCanEverTick = false;
}
