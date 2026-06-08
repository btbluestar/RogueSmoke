// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "Testing/UeMcpSmokeFixture.h"

#include "GameFramework/Actor.h"
#include "Math/Transform.h"

void AUeMcpSmokeFixture::StartTest()
{
	Super::StartTest();

	AActor* const Spawned = SpawnFixtureActor(AActor::StaticClass(), FTransform::Identity);
	const bool bOK = AssertTrue(Spawned != nullptr, TEXT("SpawnFixtureActor returned a non-null actor"));

	// AssertTrue already logged AddInfo/AddError. Finishing with Succeeded
	// on pass / Failed on fail keeps the automation-report status clear
	// independent of the internal pass/fail flag AFunctionalTest tracks.
	FinishTest(
		bOK ? EFunctionalTestResult::Succeeded : EFunctionalTestResult::Failed,
		bOK ? TEXT("AUeMcpSmokeFixture: base-class smoke succeeded.")
			: TEXT("AUeMcpSmokeFixture: SpawnFixtureActor failed."));
}
