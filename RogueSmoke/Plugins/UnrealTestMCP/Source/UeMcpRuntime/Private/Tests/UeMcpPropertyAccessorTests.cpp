// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Components/PointLightComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "UeMcpPropertyAccessor.h"
#include "UeMcpPropertyPath.h"
#include "UeMcpPropertyValue.h"

#include "UeMcpReflectionTestObject.h"

// Accessor tests drive against two roots:
//   1. A freshly-minted `UUeMcpReflectionTestObject` in the transient package —
//      covers scalars, structs, enums, containers, CDO, depth, writability.
//   2. An `APointLight` engine actor in a transient world — covers the
//      component-by-name fallback (`LightComponent.Intensity`,
//      `PointLightComponent0.Intensity`, etc.). We don't need full PIE; the
//      accessor is engine-world-agnostic.
//
// These tests do not pump the dispatcher. The accessor is synchronous by
// design; its only threading invariant is `check(IsInGameThread())`, which
// the automation runner satisfies natively.

namespace UeMcpAccessorTestsPrivate
{
	/** Spawn an APointLight in a new transient world for component-fallback tests. */
	static APointLight* SpawnTestPointLight(UWorld*& OutWorld)
	{
		OutWorld = UWorld::CreateWorld(EWorldType::Editor, false);
		if (!OutWorld)
		{
			return nullptr;
		}
		FActorSpawnParameters Params;
		Params.Name = TEXT("UeMcpTestPointLight");
		APointLight* Light = OutWorld->SpawnActor<APointLight>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
		return Light;
	}

	static void DestroyTestWorld(UWorld* World)
	{
		if (World)
		{
			World->DestroyWorld(false);
		}
	}

	static UUeMcpReflectionTestObject* NewTestObject()
	{
		return NewObject<UUeMcpReflectionTestObject>(GetTransientPackage());
	}
}

// --- Scalars ------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorScalarsTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.Scalars",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorScalarsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	TestNotNull(TEXT("object created"), Obj);
	if (!Obj)
	{
		return false;
	}

	Obj->IntField = 42;
	Obj->FloatField = 3.5f;
	Obj->BoolField = true;
	Obj->StringField = TEXT("hello");
	Obj->NameField = FName(TEXT("WorldTag"));

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	TestTrue(TEXT("read int"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("IntField"), V, Err));
	TestEqual(TEXT("int json"), V.Json.IsValid() ? static_cast<int64>(V.Json->AsNumber()) : 0, (int64)42);

	TestTrue(TEXT("read float"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("FloatField"), V, Err));
	TestEqual(TEXT("float json"), V.Json.IsValid() ? V.Json->AsNumber() : 0.0, 3.5);

	TestTrue(TEXT("read bool"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("BoolField"), V, Err));
	TestTrue(TEXT("bool json true"), V.Json.IsValid() && V.Json->AsBool());

	TestTrue(TEXT("read string"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("StringField"), V, Err));
	TestEqual(TEXT("string json"),
		V.Json.IsValid() ? V.Json->AsString() : FString(), FString(TEXT("hello")));

	TestTrue(TEXT("read name"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("NameField"), V, Err));
	TestEqual(TEXT("name json"),
		V.Json.IsValid() ? V.Json->AsString() : FString(), FString(TEXT("WorldTag")));

	// Round-trip each scalar.
	TestTrue(TEXT("write int"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("IntField"), MakeShared<FJsonValueNumber>(7.0), Err));
	TestEqual(TEXT("int after write"), Obj->IntField, 7);

	TestTrue(TEXT("write float"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("FloatField"), MakeShared<FJsonValueNumber>(1.25), Err));
	TestEqual(TEXT("float after write"), Obj->FloatField, 1.25f);

	TestTrue(TEXT("write bool"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("BoolField"), MakeShared<FJsonValueBoolean>(false), Err));
	TestFalse(TEXT("bool after write"), Obj->BoolField);

	TestTrue(TEXT("write string"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("StringField"), MakeShared<FJsonValueString>(TEXT("world")), Err));
	TestEqual(TEXT("string after write"), Obj->StringField, FString(TEXT("world")));

	return true;
}

// --- Special struct (FVector) -------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorVectorTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.Vector",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorVectorTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	if (!Obj)
	{
		return false;
	}

	Obj->VectorField = FVector(10.0, 20.0, 30.0);

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	// Whole-vector read yields the ToString form.
	TestTrue(TEXT("read whole vector"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("VectorField"), V, Err));
	const FString AsString = V.Json.IsValid() ? V.Json->AsString() : FString();
	TestTrue(TEXT("string form contains X=10"), AsString.Contains(TEXT("X=10")));

	// Member access — `.X` reaches into the vector's X component.
	TestTrue(TEXT("read vector.X"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("VectorField.X"), V, Err));
	TestEqual(TEXT("vector.X == 10"), V.Json->AsNumber(), 10.0);

	// Round-trip writing the whole vector via its string form.
	TestTrue(TEXT("write whole vector"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("VectorField"),
			MakeShared<FJsonValueString>(TEXT("X=1 Y=2 Z=3")), Err));
	TestEqual(TEXT("vector X after write"), Obj->VectorField.X, 1.0);
	TestEqual(TEXT("vector Y after write"), Obj->VectorField.Y, 2.0);
	TestEqual(TEXT("vector Z after write"), Obj->VectorField.Z, 3.0);

	// Round-trip writing a single member through the dotted path.
	TestTrue(TEXT("write vector.Y"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("VectorField.Y"),
			MakeShared<FJsonValueNumber>(99.0), Err));
	TestEqual(TEXT("vector Y after dotted write"), Obj->VectorField.Y, 99.0);

	return true;
}

// --- Generic struct and depth ------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorStructDepthTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.StructDepth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorStructDepthTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	if (!Obj)
	{
		return false;
	}

	Obj->NestedField.Depth = 5;
	Obj->NestedField.Label = TEXT("nested");

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	// Leaf member access — walks struct normally.
	TestTrue(TEXT("read nested.Depth"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("NestedField.Depth"), V, Err));
	TestEqual(TEXT("depth value"), static_cast<int32>(V.Json->AsNumber()), 5);

	// Depth-0 read truncates immediately.
	FUeMcpReadOptions DepthZero;
	DepthZero.MaxDepth = 0;
	TestTrue(TEXT("read whole nested at depth 0"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("DeepField"), V, Err, DepthZero));
	TestTrue(TEXT("truncated flag set"), V.bTruncated);

	// Generic struct write via JSON object.
	TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetNumberField(TEXT("Depth"), 99);
	Body->SetStringField(TEXT("Label"), TEXT("updated"));
	TestTrue(TEXT("write nested via json object"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("NestedField"),
			MakeShared<FJsonValueObject>(Body), Err));
	TestEqual(TEXT("nested depth updated"), Obj->NestedField.Depth, 99);
	TestEqual(TEXT("nested label updated"),
		Obj->NestedField.Label, FString(TEXT("updated")));

	return true;
}

// --- Array / Map ---------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorContainersTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.Containers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorContainersTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	if (!Obj)
	{
		return false;
	}

	Obj->IntArrayField = { 10, 20, 30 };
	Obj->StringIntMap.Add(TEXT("alpha"), 1);
	Obj->StringIntMap.Add(TEXT("beta"), 2);
	Obj->NameStringMap.Add(FName(TEXT("first")), TEXT("one"));

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	// Array indexing.
	TestTrue(TEXT("read IntArrayField[1]"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("IntArrayField[1]"), V, Err));
	TestEqual(TEXT("IntArrayField[1] == 20"),
		V.Json->AsNumber(), 20.0);

	// Array out-of-bounds surfaces IndexOOB with actual_size.
	TestFalse(TEXT("OOB fails"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("IntArrayField[99]"), V, Err));
	TestEqual(TEXT("code IndexOOB"),
		static_cast<int32>(Err.Code),
		static_cast<int32>(EUeMcpAccessorError::IndexOOB));

	// Array write: per-element.
	TestTrue(TEXT("write IntArrayField[0]"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("IntArrayField[0]"),
			MakeShared<FJsonValueNumber>(999.0), Err));
	TestEqual(TEXT("element 0 updated"), Obj->IntArrayField[0], 999);

	// Array write: whole-array replacement.
	TArray<TSharedPtr<FJsonValue>> NewArr;
	NewArr.Add(MakeShared<FJsonValueNumber>(1.0));
	NewArr.Add(MakeShared<FJsonValueNumber>(2.0));
	TestTrue(TEXT("write whole IntArrayField"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("IntArrayField"),
			MakeShared<FJsonValueArray>(NewArr), Err));
	TestEqual(TEXT("array shrunk"), Obj->IntArrayField.Num(), 2);
	TestEqual(TEXT("new element 0"), Obj->IntArrayField[0], 1);

	// Map read via quoted-key path.
	TestTrue(TEXT("read StringIntMap[\"alpha\"]"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("StringIntMap[\"alpha\"]"), V, Err));
	TestEqual(TEXT("alpha == 1"), static_cast<int32>(V.Json->AsNumber()), 1);

	// Missing map key.
	TestFalse(TEXT("missing key fails"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("StringIntMap[\"missing\"]"), V, Err));
	TestEqual(TEXT("code KeyNotFound"),
		static_cast<int32>(Err.Code),
		static_cast<int32>(EUeMcpAccessorError::KeyNotFound));

	// Map write: per-key.
	TestTrue(TEXT("write StringIntMap[\"alpha\"]"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("StringIntMap[\"alpha\"]"),
			MakeShared<FJsonValueNumber>(42.0), Err));
	TestEqual(TEXT("alpha updated"), *Obj->StringIntMap.Find(TEXT("alpha")), 42);

	// FName-keyed map.
	TestTrue(TEXT("read NameStringMap[\"first\"]"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("NameStringMap[\"first\"]"), V, Err));
	TestEqual(TEXT("first == 'one'"),
		V.Json->AsString(), FString(TEXT("one")));

	return true;
}

// --- Enum ----------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorEnumTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.Enum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorEnumTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	if (!Obj)
	{
		return false;
	}

	Obj->MovementState = EUeMcpReflectionTestMovementState::Walking;

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	// Read — primary form is the display name.
	TestTrue(TEXT("read enum"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("MovementState"), V, Err));
	TestEqual(TEXT("enum value == 'Walking'"),
		V.Json.IsValid() ? V.Json->AsString() : FString(),
		FString(TEXT("Walking")));
	TestEqual(TEXT("enum value type"),
		static_cast<int32>(V.Type),
		static_cast<int32>(EUeMcpValueType::Enum));
	TestEqual(TEXT("enum numeric set"), V.EnumNumeric,
		static_cast<int64>(EUeMcpReflectionTestMovementState::Walking));

	// Write via string display name.
	TestTrue(TEXT("write enum by name"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("MovementState"),
			MakeShared<FJsonValueString>(TEXT("Flying")), Err));
	TestEqual(TEXT("enum after string write"),
		static_cast<uint8>(Obj->MovementState),
		static_cast<uint8>(EUeMcpReflectionTestMovementState::Flying));

	// Write via numeric.
	TestTrue(TEXT("write enum by number"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("MovementState"),
			MakeShared<FJsonValueNumber>(static_cast<double>(
				EUeMcpReflectionTestMovementState::Running)), Err));
	TestEqual(TEXT("enum after numeric write"),
		static_cast<uint8>(Obj->MovementState),
		static_cast<uint8>(EUeMcpReflectionTestMovementState::Running));

	return true;
}

// --- Writability policy --------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorWritabilityTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.Writability",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorWritabilityTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	if (!Obj)
	{
		return false;
	}

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	// BlueprintReadOnly alone is writable by our policy (we're editor code).
	TestTrue(TEXT("write BlueprintReadOnly"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("BlueprintReadOnlyFloat"),
			MakeShared<FJsonValueNumber>(5.0), Err));
	TestEqual(TEXT("BlueprintReadOnly actually updated"),
		Obj->BlueprintReadOnlyFloat, 5.0f);

	// EditConst (via VisibleAnywhere) is NOT writable.
	TestFalse(TEXT("write EditConst fails"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("EditConstFloat"),
			MakeShared<FJsonValueNumber>(1.0), Err));
	TestEqual(TEXT("code NotWritable"),
		static_cast<int32>(Err.Code),
		static_cast<int32>(EUeMcpAccessorError::NotWritable));

	// Reading EditConst still works.
	TestTrue(TEXT("read EditConst works"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("EditConstFloat"), V, Err));

	// Synthesized segments are read-only.
	TestFalse(TEXT("write through @Class fails"),
		FUeMcpPropertyAccessor::SetValue(Obj, TEXT("@Class"),
			MakeShared<FJsonValueString>(TEXT("x")), Err));
	TestEqual(TEXT("code ReadOnlySegment"),
		static_cast<int32>(Err.Code),
		static_cast<int32>(EUeMcpAccessorError::ReadOnlySegment));

	return true;
}

// --- Synthesized + CDO ---------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorSynthesizedTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.Synthesized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorSynthesizedTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	if (!Obj)
	{
		return false;
	}

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	// @Class — returns the class path of the object.
	TestTrue(TEXT("read @Class"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("@Class"), V, Err));
	TestTrue(TEXT("@Class returns a string"),
		V.Json.IsValid() && V.Json->Type == EJson::String);
	TestTrue(TEXT("@Class contains UeMcpReflectionTestObject"),
		V.Json->AsString().Contains(TEXT("UeMcpReflectionTestObject")));

	// @CDO.DefaultFloat — reads from the class default object, which we
	// never mutated, so it should be the header-initialized 42.0f.
	TestTrue(TEXT("read @CDO.DefaultFloat"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("@CDO.DefaultFloat"), V, Err));
	TestEqual(TEXT("CDO DefaultFloat == 42.0"),
		V.Json->AsNumber(), 42.0);

	// Prove the instance's own `DefaultFloat` is independent: mutate the
	// instance, check CDO is unchanged.
	Obj->DefaultFloat = 7.0f;
	TestTrue(TEXT("read live DefaultFloat"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("DefaultFloat"), V, Err));
	TestEqual(TEXT("instance DefaultFloat == 7.0"),
		V.Json->AsNumber(), 7.0);
	TestTrue(TEXT("read @CDO.DefaultFloat again"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("@CDO.DefaultFloat"), V, Err));
	TestEqual(TEXT("CDO DefaultFloat still 42.0"),
		V.Json->AsNumber(), 42.0);

	return true;
}

// --- Component-by-name fallback against an engine actor -----------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorComponentFallbackTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.ComponentFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorComponentFallbackTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;

	UWorld* World = nullptr;
	APointLight* Light = SpawnTestPointLight(World);
	TestNotNull(TEXT("light spawned"), Light);
	if (!Light)
	{
		DestroyTestWorld(World);
		return false;
	}

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	// Component-by-name: APointLight's light component has a class-default
	// name of `LightComponent0` (APointLight inherits from ALight which
	// constructs a `PointLightComponent0` subobject named `LightComponent0`).
	// Either `LightComponent0.AttenuationRadius` OR the prefix-match
	// `LightComponent.AttenuationRadius` should find it.
	TestTrue(TEXT("read LightComponent0.AttenuationRadius"),
		FUeMcpPropertyAccessor::GetValue(
			Light, TEXT("LightComponent0.AttenuationRadius"), V, Err));
	TestTrue(TEXT("attenuation radius > 0"),
		V.Json.IsValid() && V.Json->AsNumber() > 0.0);

	// Prefix-plus-trailing-digits: `LightComponent` should resolve
	// `LightComponent0`.
	TestTrue(TEXT("prefix match LightComponent.AttenuationRadius"),
		FUeMcpPropertyAccessor::GetValue(
			Light, TEXT("LightComponent.AttenuationRadius"), V, Err));

	// Case-insensitive root property match.
	TestTrue(TEXT("case-insensitive root property"),
		FUeMcpPropertyAccessor::GetValue(
			Light, TEXT("lightcomponent0.attenuationradius"), V, Err));

	// @Components on an actor returns a non-empty array.
	TestTrue(TEXT("read @Components"),
		FUeMcpPropertyAccessor::GetValue(Light, TEXT("@Components"), V, Err));
	TestTrue(TEXT("@Components is an array"),
		V.Json.IsValid() && V.Json->Type == EJson::Array);
	TestTrue(TEXT("@Components has entries"),
		V.Json->AsArray().Num() > 0);

	DestroyTestWorld(World);
	return true;
}

// --- Error taxonomy ------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorErrorsTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.Errors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorErrorsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	if (!Obj)
	{
		return false;
	}

	FUeMcpPropertyValue V;
	FUeMcpAccessorErrorInfo Err;

	// Property not found.
	TestFalse(TEXT("missing property fails"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("Nonsense"), V, Err));
	TestEqual(TEXT("code PropNotFound"),
		static_cast<int32>(Err.Code),
		static_cast<int32>(EUeMcpAccessorError::PropNotFound));

	// Null root.
	TestFalse(TEXT("null root fails"),
		FUeMcpPropertyAccessor::GetValue(nullptr, TEXT("Whatever"), V, Err));
	TestEqual(TEXT("code NullObject"),
		static_cast<int32>(Err.Code),
		static_cast<int32>(EUeMcpAccessorError::NullObject));

	// Bad path.
	TestFalse(TEXT("trailing dot fails"),
		FUeMcpPropertyAccessor::GetValue(Obj, TEXT("A."), V, Err));
	TestEqual(TEXT("code InvalidPath"),
		static_cast<int32>(Err.Code),
		static_cast<int32>(EUeMcpAccessorError::InvalidPath));

	return true;
}

// --- ListPropertyPaths ---------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpAccessorListPathsTest,
	"unreal-test-mcp.Runtime.PropertyAccessor.ListPaths",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpAccessorListPathsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpAccessorTestsPrivate;
	UUeMcpReflectionTestObject* Obj = NewTestObject();
	if (!Obj)
	{
		return false;
	}

	TArray<FString> Paths;
	FUeMcpAccessorErrorInfo Err;
	TestTrue(TEXT("list paths"),
		FUeMcpPropertyAccessor::ListPropertyPaths(Obj, Paths, Err));
	TestTrue(TEXT("paths non-empty"), Paths.Num() > 0);

	// Spot-check the expected paths.
	auto Contains = [&Paths](const TCHAR* Needle)
	{
		for (const FString& P : Paths)
		{
			if (P == Needle)
			{
				return true;
			}
		}
		return false;
	};
	TestTrue(TEXT("has IntField"), Contains(TEXT("IntField")));
	TestTrue(TEXT("has VectorField (special struct, atomic)"),
		Contains(TEXT("VectorField")));
	TestTrue(TEXT("has NestedField.Depth (generic struct, descended)"),
		Contains(TEXT("NestedField.Depth")));
	TestTrue(TEXT("has @Class"), Contains(TEXT("@Class")));
	TestTrue(TEXT("has @CDO"), Contains(TEXT("@CDO")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
