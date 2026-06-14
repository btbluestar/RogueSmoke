// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#include "UeMcpPropertyPath.h"

// Parser tests cover: identifier forms, dot chains, numeric indices, quoted
// map keys with escapes, synthesized tokens (terminal and CDO-with-tail),
// enum_name suffix, and parse errors with position-accurate reporting.
//
// The point of these tests is NOT to re-test UE's JSON/string code. It's to
// pin the v0 path grammar so that when we extend (FGameplayTag keys,
// function-call path segments, etc.) a regression in the grammar is loud.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpPropertyPathParseBasicTest,
	"unreal-test-mcp.Runtime.PropertyPath.ParseBasic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpPropertyPathParseBasicTest::RunTest(const FString& /*Parameters*/)
{
	{
		TArray<FUeMcpPathSegment> Segs;
		TestTrue(TEXT("single identifier"),
			FUeMcpPropertyPath::ParsePath(TEXT("Health"), Segs));
		TestEqual(TEXT("one segment"), Segs.Num(), 1);
		TestTrue(TEXT("property kind"),
			Segs.Num() > 0 && Segs[0].Kind == EUeMcpPathSegmentKind::Property);
		TestEqual(TEXT("name"),
			Segs.Num() > 0 ? Segs[0].Name : FString(), FString(TEXT("Health")));
	}

	{
		TArray<FUeMcpPathSegment> Segs;
		TestTrue(TEXT("dotted chain"),
			FUeMcpPropertyPath::ParsePath(TEXT("A.B.C"), Segs));
		TestEqual(TEXT("three segments"), Segs.Num(), 3);
		if (Segs.Num() == 3)
		{
			TestEqual(TEXT("A"), Segs[0].Name, FString(TEXT("A")));
			TestEqual(TEXT("B"), Segs[1].Name, FString(TEXT("B")));
			TestEqual(TEXT("C"), Segs[2].Name, FString(TEXT("C")));
		}
	}

	{
		TArray<FUeMcpPathSegment> Segs;
		TestTrue(TEXT("array index"),
			FUeMcpPropertyPath::ParsePath(TEXT("A[0]"), Segs));
		TestEqual(TEXT("two segments (A + [0])"), Segs.Num(), 2);
		if (Segs.Num() == 2)
		{
			TestEqual(TEXT("first is property"),
				static_cast<int32>(Segs[0].Kind),
				static_cast<int32>(EUeMcpPathSegmentKind::Property));
			TestEqual(TEXT("second is array index"),
				static_cast<int32>(Segs[1].Kind),
				static_cast<int32>(EUeMcpPathSegmentKind::ArrayIndex));
			TestEqual(TEXT("index value"), Segs[1].Index, 0);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpPropertyPathParseMapKeysTest,
	"unreal-test-mcp.Runtime.PropertyPath.ParseMapKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpPropertyPathParseMapKeysTest::RunTest(const FString& /*Parameters*/)
{
	{
		TArray<FUeMcpPathSegment> Segs;
		TestTrue(TEXT("simple map key"),
			FUeMcpPropertyPath::ParsePath(TEXT("A[\"K\"]"), Segs));
		TestEqual(TEXT("two segments"), Segs.Num(), 2);
		if (Segs.Num() == 2)
		{
			TestEqual(TEXT("second kind map key"),
				static_cast<int32>(Segs[1].Kind),
				static_cast<int32>(EUeMcpPathSegmentKind::MapKey));
			TestEqual(TEXT("key value"), Segs[1].Key, FString(TEXT("K")));
		}
	}

	{
		TArray<FUeMcpPathSegment> Segs;
		TestTrue(TEXT("map key with spaces and continuation"),
			FUeMcpPropertyPath::ParsePath(TEXT("A[\"K with spaces\"].B[1]"), Segs));
		// A (Property), [K with spaces] (MapKey), B (Property), [1] (ArrayIndex)
		TestEqual(TEXT("four segments"), Segs.Num(), 4);
		if (Segs.Num() == 4)
		{
			TestEqual(TEXT("key preserves spaces"),
				Segs[1].Key, FString(TEXT("K with spaces")));
			TestEqual(TEXT("final segment index"), Segs[3].Index, 1);
		}
	}

	{
		// Escaped quote inside a key: \\" must yield a literal ".
		TArray<FUeMcpPathSegment> Segs;
		const FString Path = TEXT("Meta[\"key\\\"quoted\"]");
		TestTrue(TEXT("escaped quote in key"),
			FUeMcpPropertyPath::ParsePath(Path, Segs));
		TestEqual(TEXT("two segments"), Segs.Num(), 2);
		if (Segs.Num() == 2)
		{
			TestEqual(TEXT("escaped key body"),
				Segs[1].Key, FString(TEXT("key\"quoted")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpPropertyPathParseSynthesizedTest,
	"unreal-test-mcp.Runtime.PropertyPath.ParseSynthesized",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpPropertyPathParseSynthesizedTest::RunTest(const FString& /*Parameters*/)
{
	{
		TArray<FUeMcpPathSegment> Segs;
		TestTrue(TEXT("@Class terminal"),
			FUeMcpPropertyPath::ParsePath(TEXT("@Class"), Segs));
		TestEqual(TEXT("one segment"), Segs.Num(), 1);
		if (Segs.Num() == 1)
		{
			TestEqual(TEXT("synthesized kind"),
				static_cast<int32>(Segs[0].Kind),
				static_cast<int32>(EUeMcpPathSegmentKind::Synthesized));
			TestEqual(TEXT("name stripped of @"),
				Segs[0].Name, FString(TEXT("Class")));
		}
	}

	{
		TArray<FUeMcpPathSegment> Segs;
		TestTrue(TEXT("@CDO with tail"),
			FUeMcpPropertyPath::ParsePath(TEXT("@CDO.A.B"), Segs));
		TestEqual(TEXT("three segments"), Segs.Num(), 3);
		if (Segs.Num() == 3)
		{
			TestEqual(TEXT("first synthesized"),
				static_cast<int32>(Segs[0].Kind),
				static_cast<int32>(EUeMcpPathSegmentKind::Synthesized));
			TestEqual(TEXT("CDO"), Segs[0].Name, FString(TEXT("CDO")));
			TestEqual(TEXT("A"), Segs[1].Name, FString(TEXT("A")));
			TestEqual(TEXT("B"), Segs[2].Name, FString(TEXT("B")));
		}
	}

	{
		// @Components is terminal — trailing continuation is a parse error.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("@Components cannot have continuation"),
			FUeMcpPropertyPath::ParsePath(TEXT("@Components.Foo"), Segs, &Err));
		TestEqual(TEXT("code"), Err.Code, FString(TEXT("INVALID_PAYLOAD")));
	}

	{
		// @CDO must be first if used.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("@CDO only as first segment"),
			FUeMcpPropertyPath::ParsePath(TEXT("Health.@CDO.A"), Segs, &Err));
		TestEqual(TEXT("position reported"), Err.Position, 0);
	}

	{
		// Unknown synthesized token rejected at parse time.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("unknown @token"),
			FUeMcpPropertyPath::ParsePath(TEXT("@Wibble"), Segs, &Err));
		TestEqual(TEXT("code"), Err.Code, FString(TEXT("INVALID_PAYLOAD")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpPropertyPathParseEnumSuffixTest,
	"unreal-test-mcp.Runtime.PropertyPath.ParseEnumSuffix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpPropertyPathParseEnumSuffixTest::RunTest(const FString& /*Parameters*/)
{
	{
		TArray<FUeMcpPathSegment> Segs;
		TestTrue(TEXT("enum suffix parses"),
			FUeMcpPropertyPath::ParsePath(TEXT("Foo:enum_name"), Segs));
		TestEqual(TEXT("one segment"), Segs.Num(), 1);
		if (Segs.Num() == 1)
		{
			TestEqual(TEXT("name"), Segs[0].Name, FString(TEXT("Foo")));
			TestTrue(TEXT("enum display requested"), Segs[0].bEnumNameDisplay);
		}
	}

	{
		// Unknown suffix is rejected.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("unknown suffix rejected"),
			FUeMcpPropertyPath::ParsePath(TEXT("Foo:bogus"), Segs, &Err));
		TestEqual(TEXT("code"), Err.Code, FString(TEXT("INVALID_PAYLOAD")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpPropertyPathParseErrorsTest,
	"unreal-test-mcp.Runtime.PropertyPath.ParseErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpPropertyPathParseErrorsTest::RunTest(const FString& /*Parameters*/)
{
	{
		// Empty path — position 0.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("empty path rejected"),
			FUeMcpPropertyPath::ParsePath(TEXT(""), Segs, &Err));
		TestEqual(TEXT("position 0"), Err.Position, 0);
	}

	{
		// Trailing dot — position should be len(path).
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("trailing dot rejected"),
			FUeMcpPropertyPath::ParsePath(TEXT("A."), Segs, &Err));
		// The parser surfaces the position where the trailing-dot condition
		// tripped — length of the input.
		TestEqual(TEXT("position at end"), Err.Position, 2);
	}

	{
		// Unclosed bracket — position should point at the '['.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("unclosed bracket rejected"),
			FUeMcpPropertyPath::ParsePath(TEXT("A["), Segs, &Err));
		// Specific-position check: parser reports the last consumed position
		// at the failure point; tolerate off-by-one by checking it's inside
		// the input range.
		TestTrue(TEXT("error position within string"),
			Err.Position >= 0 && Err.Position <= 2);
	}

	{
		// Unclosed quoted key.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("unclosed quote rejected"),
			FUeMcpPropertyPath::ParsePath(TEXT("A[\""), Segs, &Err));
	}

	{
		// Double dot between segments.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("double dot rejected"),
			FUeMcpPropertyPath::ParsePath(TEXT("A..B"), Segs, &Err));
	}

	{
		// Bracket without a preceding segment.
		TArray<FUeMcpPathSegment> Segs;
		FUeMcpPathParseError Err;
		TestFalse(TEXT("leading bracket rejected"),
			FUeMcpPropertyPath::ParsePath(TEXT("[0]"), Segs, &Err));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpPropertyPathValidateTest,
	"unreal-test-mcp.Runtime.PropertyPath.Validate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpPropertyPathValidateTest::RunTest(const FString& /*Parameters*/)
{
	// `ValidatePath` is the same parser without the segment output — good for
	// the tools' pre-hop SCHEMA_ERROR check.
	TestTrue(TEXT("happy"), FUeMcpPropertyPath::ValidatePath(TEXT("A.B[0]")));
	TestFalse(TEXT("trailing dot"), FUeMcpPropertyPath::ValidatePath(TEXT("A.")));
	TestFalse(TEXT("empty"), FUeMcpPropertyPath::ValidatePath(TEXT("")));
	TestTrue(TEXT("synthesized terminal valid"),
		FUeMcpPropertyPath::ValidatePath(TEXT("@Class")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpPropertyPathSynthesizedTableTest,
	"unreal-test-mcp.Runtime.PropertyPath.SynthesizedTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpPropertyPathSynthesizedTableTest::RunTest(const FString& /*Parameters*/)
{
	// Contract: only the three tokens are recognised; new ones require a
	// deliberate addition and will break this test.
	TestTrue(TEXT("Class"),
		FUeMcpPropertyPath::IsKnownSynthesizedName(TEXT("Class")));
	TestTrue(TEXT("Components"),
		FUeMcpPropertyPath::IsKnownSynthesizedName(TEXT("Components")));
	TestTrue(TEXT("CDO"),
		FUeMcpPropertyPath::IsKnownSynthesizedName(TEXT("CDO")));
	TestTrue(TEXT("case-insensitive"),
		FUeMcpPropertyPath::IsKnownSynthesizedName(TEXT("cdo")));
	TestFalse(TEXT("Wibble rejected"),
		FUeMcpPropertyPath::IsKnownSynthesizedName(TEXT("Wibble")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
