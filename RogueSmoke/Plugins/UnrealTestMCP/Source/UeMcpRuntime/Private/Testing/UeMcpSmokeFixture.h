// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Minimal AUeMcpTestFixture subclass used to live-verify the base class
// from a running editor. Private header — the class is only reached via
// reflection (UObject registry), nothing outside this module needs the
// declaration.

#pragma once

#include "CoreMinimal.h"

#include "Testing/UeMcpTestFixture.h"

#include "UeMcpSmokeFixture.generated.h"

UCLASS()
class AUeMcpSmokeFixture : public AUeMcpTestFixture
{
	GENERATED_BODY()

public:
	virtual void StartTest() override;
};
