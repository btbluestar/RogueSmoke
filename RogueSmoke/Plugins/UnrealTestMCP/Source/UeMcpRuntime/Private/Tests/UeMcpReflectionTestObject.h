// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Test-only UObject exercising the property shapes the reflection accessor
// needs to round-trip: scalars, string-ish, struct (special + generic),
// containers (TArray, TMap with string keys), enum, and the two writability
// policy cases (BlueprintReadOnly writable, EditConst blocked).
//
// Not wrapped in a preprocessor gate: UHT refuses `UCLASS`/`UPROPERTY` under
// any `#if` other than `WITH_EDITORONLY_DATA`. The types compile into every
// configuration but are only *referenced* from test .cpp files gated on
// `WITH_DEV_AUTOMATION_TESTS`, so they add ~zero cost outside tests.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UeMcpReflectionTestObject.generated.h"

UENUM()
enum class EUeMcpReflectionTestMovementState : uint8
{
	Idle,
	Walking,
	Running,
	Flying,
};

USTRUCT()
struct FUeMcpReflectionTestNested
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Depth = 0;

	UPROPERTY()
	FString Label;

	// One level of self-reference via pointer would be ideal for depth
	// testing, but TObjectPtr<FUeMcpReflectionTestNested*> isn't supported
	// on USTRUCTs — we cover depth by nesting struct-in-struct in the owner.
};

USTRUCT()
struct FUeMcpReflectionTestDeep
{
	GENERATED_BODY()

	UPROPERTY()
	FUeMcpReflectionTestNested A;

	UPROPERTY()
	FUeMcpReflectionTestNested B;

	UPROPERTY()
	int32 Marker = 0;
};

UCLASS()
class UUeMcpReflectionTestObject : public UObject
{
	GENERATED_BODY()

public:
	// Scalars.
	UPROPERTY()
	int32 IntField = 0;

	UPROPERTY()
	float FloatField = 0.0f;

	UPROPERTY()
	bool BoolField = false;

	UPROPERTY()
	FString StringField;

	UPROPERTY()
	FName NameField;

	// Struct (special — FVector has the ToString/InitFromString fast path).
	UPROPERTY()
	FVector VectorField = FVector::ZeroVector;

	// Struct (generic — no fast path, forces member-by-member walk).
	UPROPERTY()
	FUeMcpReflectionTestNested NestedField;

	// Depth-truncation canary: a struct-of-structs five levels deep.
	UPROPERTY()
	FUeMcpReflectionTestDeep DeepField;

	// Containers.
	UPROPERTY()
	TArray<int32> IntArrayField;

	UPROPERTY()
	TArray<FString> StringArrayField;

	UPROPERTY()
	TMap<FString, int32> StringIntMap;

	UPROPERTY()
	TMap<FName, FString> NameStringMap;

	// Enum.
	UPROPERTY()
	EUeMcpReflectionTestMovementState MovementState = EUeMcpReflectionTestMovementState::Idle;

	// Writability policy cases.
	//
	// BlueprintReadOnly on its own MUST still be writable by the accessor —
	// we're editor code, not Blueprint script. Matches Incurian §2.4.
	UPROPERTY(BlueprintReadOnly, Category = "UeMcp|Test")
	float BlueprintReadOnlyFloat = 0.0f;

	// EditConst is the real write barrier. Also marked VisibleAnywhere so the
	// accessor's property-walk can reach it for a read but reject the write.
	UPROPERTY(VisibleAnywhere, Category = "UeMcp|Test")
	float EditConstFloat = 0.0f;

	// A value on the CDO the CDO-path test can compare against.
	UPROPERTY()
	float DefaultFloat = 42.0f;
};
