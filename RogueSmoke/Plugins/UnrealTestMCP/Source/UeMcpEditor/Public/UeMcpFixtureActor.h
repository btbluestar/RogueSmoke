// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Spawnable fixture actor for the live roundtrip harness. Exists so tests
// have a stable, non-engine-version-dependent target for:
//   - `CPF_EditConst` writability rejection (via `FixtureEditConstValue`).
//   - A Blueprint-subclass anchor for the `blueprint.outline` fixture BP
//     (see `/Game/Blueprints/BP_HarnessFixture`, created on first
//     live-verification run — see `docs/tool-testing.md` M5 section).
//
// Keep the field set small and stable. Every field name is load-bearing
// for a specific test assertion; adding/renaming requires a roundtrip
// change.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "UeMcpFixtureActor.generated.h"

UCLASS(Blueprintable, Category = "UeMcp|Fixture", meta = (DisplayName = "UE MCP Fixture Actor"))
class UEMCPEDITOR_API AUeMcpFixtureActor : public AActor
{
	GENERATED_BODY()

public:
	AUeMcpFixtureActor();

	/**
	 * `VisibleAnywhere` is what makes this `CPF_EditConst` on the wire —
	 * our accessor policy refuses writes to `CPF_EditConst` but allows
	 * reads. The `set_property.rejects_edit_const` roundtrip targets
	 * exactly this field.
	 */
	UPROPERTY(VisibleAnywhere, Category = "UeMcp|Fixture")
	float FixtureEditConstValue = 17.0f;

	/**
	 * A plain editable float. Control group — proves writes DO land when
	 * the property isn't `EditConst`.
	 */
	UPROPERTY(EditAnywhere, Category = "UeMcp|Fixture")
	float FixtureEditableValue = 0.0f;
};
