// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpTestHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

#include "UeMcpDispatcher.h"
#include "UeMcpEditorSubsystem.h" // for LogUeMcpEditor
#include "UeMcpGameThreadExecutor.h"

/**
 * Design note — why `FAutomationTestFramework::Get()` and not
 * `IAutomationControllerManager`.
 *
 * The controller-manager path requires a live session (MessageBus worker,
 * device group, test preset) which the editor does not spin up until the
 * Session Frontend tab is opened. Calling `RequestTests()` cold crashes
 * inside UE at `SharedPointer.h:1112` because an internal session ptr is
 * null. Both ChiR24 and remiphilippe sidestep this by talking to the
 * framework directly — that's the engine singleton every
 * `IMPLEMENT_*_AUTOMATION_TEST` macro registers against, independent of
 * any distributed test session.
 *
 * Trade-off: we lose controller-tree grouping (`Editor`, `Editor.Maps`,
 * etc.) and per-session state. Neither matters for the v0 wedge — our
 * clients filter by substring on the full path, not by tree node. And we
 * gain crash-free cold-boot behavior.
 */

namespace UeMcpTestHandlersPrivate
{
	/** Default cap on tests returned by `tests.list` before truncation. */
	static constexpr int32 DefaultResultLimit = 500;

	/** Default dispatcher timeout for `tests.list`. Framework-direct is sub-ms even for thousands of tests. */
	static constexpr double ListDefaultTimeoutSeconds = 10.0;

	/** Default dispatcher timeout for `tests.refresh`. */
	static constexpr double RefreshDefaultTimeoutSeconds = 15.0;

	/**
	 * Build an inline error payload: `{error, message}` at the root of the
	 * handler's returned JSON. The dispatcher hoists those into the
	 * top-level response's `ok: false, error, message` shape.
	 */
	static TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}

	/** Descriptor table for the flag bits we expose on the wire. */
	struct FFlagMaskDescriptor
	{
		const TCHAR* Name;
		EAutomationTestFlags Mask;
	};

	static const TArray<FFlagMaskDescriptor>& GetFlagDescriptors()
	{
		static const TArray<FFlagMaskDescriptor> Descriptors = {
			{ TEXT("editor"),         EAutomationTestFlags::EditorContext },
			{ TEXT("client"),         EAutomationTestFlags::ClientContext },
			{ TEXT("server"),         EAutomationTestFlags::ServerContext },
			{ TEXT("commandlet"),     EAutomationTestFlags::CommandletContext },
			{ TEXT("smoke"),          EAutomationTestFlags::SmokeFilter },
			{ TEXT("engine"),         EAutomationTestFlags::EngineFilter },
			{ TEXT("product"),        EAutomationTestFlags::ProductFilter },
			{ TEXT("perf"),           EAutomationTestFlags::PerfFilter },
			{ TEXT("stress"),         EAutomationTestFlags::StressFilter },
			{ TEXT("negative"),       EAutomationTestFlags::NegativeFilter },
			{ TEXT("requires_user"),  EAutomationTestFlags::RequiresUser },
		};
		return Descriptors;
	}

	/** Build the per-test flag bool object. One bit per known descriptor. */
	static TSharedRef<FJsonObject> BuildFlagsObject(EAutomationTestFlags Flags)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const FFlagMaskDescriptor& Desc : GetFlagDescriptors())
		{
			const bool bSet = EnumHasAnyFlags(Flags, Desc.Mask);
			Out->SetBoolField(Desc.Name, bSet);
		}
		return Out;
	}

	/**
	 * Parse the optional `flags` args object into an OR-mask. Returns `None`
	 * when no known keys are set — callers interpret `None` as "no filter".
	 */
	static EAutomationTestFlags ParseFlagFilter(const TSharedPtr<FJsonObject>& FlagsObj)
	{
		EAutomationTestFlags Combined = EAutomationTestFlags::None;
		if (!FlagsObj.IsValid())
		{
			return Combined;
		}
		for (const FFlagMaskDescriptor& Desc : GetFlagDescriptors())
		{
			bool bValue = false;
			if (FlagsObj->TryGetBoolField(Desc.Name, bValue) && bValue)
			{
				Combined |= Desc.Mask;
			}
		}
		return Combined;
	}

	/** Build the per-test JSON object. */
	static TSharedRef<FJsonObject> BuildTestJson(const FAutomationTestInfo& Info)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();

		const FString& DisplayName = Info.GetDisplayName();
		const FString& FullPath = Info.GetFullTestPath();
		const EAutomationTestFlags Flags =
			static_cast<EAutomationTestFlags>(Info.GetTestFlags());

		Out->SetStringField(TEXT("name"), DisplayName);
		Out->SetStringField(TEXT("full_path"), FullPath);
		Out->SetObjectField(TEXT("flags"), BuildFlagsObject(Flags));

		const FString& SourceFile = Info.GetSourceFile();
		if (!SourceFile.IsEmpty())
		{
			Out->SetStringField(TEXT("source_file"), SourceFile);
			const int32 Line = Info.GetSourceFileLine();
			if (Line > 0)
			{
				Out->SetNumberField(TEXT("source_line"), Line);
			}
		}

		Out->SetBoolField(TEXT("requires_user"),
			EnumHasAnyFlags(Flags, EAutomationTestFlags::RequiresUser));
		Out->SetNumberField(TEXT("num_participants"),
			Info.GetNumParticipantsRequired());

		return Out;
	}

	/**
	 * Enumerate every registered test by asking the framework with a
	 * maximally-permissive filter set. UE 5.7 expresses the filter as
	 * `EAutomationTestFlags` (a flag enum) rather than the legacy
	 * `uint32`. We don't try to save/restore the prior filter — there's
	 * no public `GetRequestedTestFilter` on 5.7, and our handlers run
	 * serially under the dispatcher's game-thread queue, so concurrent
	 * overlap isn't a concern.
	 */
	static void EnumerateAllTests(
		TArray<FAutomationTestInfo>& OutInfos)
	{
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

		// All contexts + all filter categories. Every registered test must
		// match one of these; the OR of the full set is equivalent to
		// "return everything."
		const EAutomationTestFlags AllContextsAndFilters =
			EAutomationTestFlags_ApplicationContextMask
			| EAutomationTestFlags::SmokeFilter
			| EAutomationTestFlags::EngineFilter
			| EAutomationTestFlags::ProductFilter
			| EAutomationTestFlags::PerfFilter
			| EAutomationTestFlags::StressFilter
			| EAutomationTestFlags::NegativeFilter;

		Framework.SetRequestedTestFilter(AllContextsAndFilters);
		Framework.GetValidTestNames(OutInfos);
	}

	/** `tests.list` handler body. */
	static TSharedRef<FJsonObject> HandleTestsList(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		UE_LOG(LogUeMcpEditor, Verbose, TEXT("tests.list dispatch"));

		FString Filter;
		Args->TryGetStringField(TEXT("filter"), Filter);

		int32 Limit = DefaultResultLimit;
		{
			int32 LimitRaw = 0;
			if (Args->TryGetNumberField(TEXT("limit"), LimitRaw) && LimitRaw > 0)
			{
				Limit = LimitRaw;
			}
		}

		const TSharedPtr<FJsonObject>* FlagsObjPtr = nullptr;
		Args->TryGetObjectField(TEXT("flags"), FlagsObjPtr);
		const EAutomationTestFlags FlagFilter =
			ParseFlagFilter(FlagsObjPtr != nullptr ? *FlagsObjPtr : TSharedPtr<FJsonObject>());

		TArray<FAutomationTestInfo> AllInfos;
		EnumerateAllTests(AllInfos);

		const FString LowerFilter = Filter.ToLower();

		int32 TotalMatched = 0;
		TArray<TSharedPtr<FJsonValue>> Emitted;
		Emitted.Reserve(FMath::Min(Limit, AllInfos.Num()));

		for (const FAutomationTestInfo& Info : AllInfos)
		{
			if (!LowerFilter.IsEmpty())
			{
				if (!Info.GetFullTestPath().ToLower().Contains(LowerFilter))
				{
					continue;
				}
			}

			if (FlagFilter != EAutomationTestFlags::None)
			{
				const EAutomationTestFlags TestFlags =
					static_cast<EAutomationTestFlags>(Info.GetTestFlags());
				if (!EnumHasAnyFlags(TestFlags, FlagFilter))
				{
					continue;
				}
			}

			TotalMatched++;
			if (Emitted.Num() < Limit)
			{
				Emitted.Add(MakeShared<FJsonValueObject>(BuildTestJson(Info)));
			}
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("tests"), Emitted);
		Data->SetNumberField(TEXT("total_matched"), TotalMatched);
		Data->SetBoolField(TEXT("truncated"), TotalMatched > Emitted.Num());
		// The framework-direct path is synchronously authoritative — if the
		// call returned, we are ready. `Loading` would only ever appear on
		// the (removed) controller path; preserved in the schema vocabulary
		// for backward compatibility with clients that pattern-match on it.
		Data->SetStringField(TEXT("controller_state"), TEXT("Ready"));

		return Data;
	}

	/** `tests.refresh` handler body. */
	static TSharedRef<FJsonObject> HandleTestsRefresh(
		const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		UE_LOG(LogUeMcpEditor, Verbose, TEXT("tests.refresh dispatch"));

		const double StartSeconds = FPlatformTime::Seconds();

		// The framework auto-enumerates as modules load; there's no explicit
		// rescan API. Callers that expect `tests.refresh` to reflect a
		// newly-compiled test module should trigger that compile first (via
		// Hot Reload / Live Coding / UBT) — once the module is in the
		// process, the framework sees it immediately and `tests.list` will
		// include it without us doing anything.
		//
		// We still count tests so the caller gets a useful signal that the
		// refresh was observed.
		TArray<FAutomationTestInfo> Infos;
		EnumerateAllTests(Infos);

		const double EndSeconds = FPlatformTime::Seconds();

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("controller_state"), TEXT("Ready"));
		Data->SetNumberField(TEXT("elapsed_ms"),
			FMath::RoundToDouble((EndSeconds - StartSeconds) * 1000.0));
		Data->SetNumberField(TEXT("num_tests_discovered"), Infos.Num());
		return Data;
	}
}

void UeMcp::RegisterTestHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpTestHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.list"));
		Reg.DefaultTimeoutSeconds = ListDefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsList);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.refresh"));
		Reg.DefaultTimeoutSeconds = RefreshDefaultTimeoutSeconds;
		// Marked mutating so the dispatcher's timeout-policy layer treats
		// it as a write; the refresh itself is idempotent. See handler-
		// conventions doc §4.
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsRefresh);
		Dispatcher.RegisterTool(Reg);
	}
}
