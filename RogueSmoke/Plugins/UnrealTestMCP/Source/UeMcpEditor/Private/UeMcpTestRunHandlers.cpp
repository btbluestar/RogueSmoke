// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpTestRunHandlers.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/AutomationEvent.h"
#include "Misc/Guid.h"

#include "UeMcpDispatcher.h"
#include "UeMcpEditorSubsystem.h" // LogUeMcpEditor
#include "UeMcpGameThreadExecutor.h"

/**
 * Live-editor test-run dispatcher (M4).
 *
 * DESIGN SHAPE
 *
 * - The handlers are fire-and-forget. `tests.run` hands a `{handle}` back
 *   immediately; the actual run proceeds on a ticker-driven state machine
 *   while the game thread is free to service ticks, PIE, asset loads.
 *
 * - The runner is a module-scoped singleton keyed by `live-<32-hex>`
 *   handle. A second `tests.run` while a run is active returns
 *   `EDITOR_BUSY` with the active handle in `detail`. Concurrency is v1;
 *   the automation framework is itself single-active-test, so layering
 *   concurrent public runs would require a queue we intentionally don't
 *   ship yet.
 *
 * - The ticker drives one test at a time: `StartTestByName` →
 *   poll `ExecuteLatentCommands()` each tick → when the queue empties
 *   and the framework's `CurrentTest` clears, call `StopTest()` to
 *   harvest the execution info, then move to the next requested test.
 *   This mirrors `FAutomationWorkerModule::Tick` at
 *   `Runtime/AutomationWorker/Private/AutomationWorkerModule.cpp:65-100`.
 *
 * - Retry-on-flake is a SECOND full pass over only the first-run
 *   failures. Tests that pass the second time are reported as
 *   `status="flaky"`; tests that fail twice stay `failed`. The merge
 *   rule matches `server/src/unreal_test_mcp/test_runner.py::
 *   _merge_retry_parse` so the Python layer normalises the two flows
 *   through the same `TestResult` shape.
 *
 * - Cancellation semantics: `tests.run_cancel` sets a flag observed
 *   AT SAFE POINTS — between tests, and between the primary and retry
 *   passes. The engine's automation framework does not expose a
 *   "kill a running test now" API; forcing one would corrupt editor
 *   state (latent commands holding world-level resources). So we let
 *   the in-flight test complete naturally, then stop.
 *
 * - Error taxonomy: `EDITOR_NOT_READY`, `EDITOR_BUSY`, `NOT_FOUND`,
 *   `SCHEMA_ERROR`, `TEST_NOT_FOUND`, `TEST_FILTER_MATCHED_ZERO`,
 *   `TEST_TIMEOUT`, `AUTOMATION_CONTROLLER_UNAVAILABLE`. Codes defined
 *   in `docs/handler-conventions.md §2`.
 */
namespace UeMcpTestRunHandlersPrivate
{
	// ---- Dispatcher timeouts -------------------------------------------- //
	// These are the caller-observed timeouts for the quick JSON state-change
	// each handler performs — not the test-run budget (which is separately
	// `timeout_seconds` inside the run state). Keep them short.
	static constexpr double RunDispatcherTimeoutSeconds       = 5.0;
	static constexpr double RunStatusDispatcherTimeoutSeconds = 2.0;
	static constexpr double RunReportDispatcherTimeoutSeconds = 2.0;
	static constexpr double RunCancelDispatcherTimeoutSeconds = 2.0;

	// `tests.await_completion` parks via PendingHandler until the active run
	// reaches a terminal state or the caller's `timeout_seconds` elapses.
	// Dispatcher ceiling is set above the per-call max so the in-handler
	// timeout always wins the race.
	static constexpr double AwaitDispatcherTimeoutSeconds = 605.0;
	static constexpr double AwaitDefaultTimeoutSeconds    = 60.0;
	static constexpr double AwaitMaxTimeoutSeconds        = 600.0;
	static constexpr int32  AwaitDefaultPollMs            = 200;
	static constexpr int32  AwaitMinPollMs                = 50;
	static constexpr int32  AwaitMaxPollMs                = 5000;

	// Poll frequency of our state-machine ticker. The ticker runs every
	// frame; 0.0 seconds is fine because our Tick body is cheap and only
	// advances state between ExecuteLatentCommands() calls the framework
	// is already doing.
	static constexpr float TickerIntervalSeconds = 0.0f;

	/** Default test-run wall-clock budget (seconds). */
	static constexpr double DefaultTimeoutSeconds = 300.0;

	// ---- Wire-vocabulary strings ---------------------------------------- //
	static const FString StateRunning    = TEXT("running");
	static const FString StateFinished   = TEXT("finished");
	static const FString StateCancelled  = TEXT("cancelled");
	static const FString StateTimedOut   = TEXT("timed_out");

	static const FString StatusPassed   = TEXT("passed");
	static const FString StatusFailed   = TEXT("failed");
	static const FString StatusSkipped  = TEXT("skipped");
	static const FString StatusFlaky    = TEXT("flaky");

	/**
	 * Internal per-run state. Owned by `GActiveRun` when non-null. All
	 * mutation happens on the game thread (from handler bodies or the
	 * ticker), so no lock is needed.
	 */
	struct FTestEventRecord
	{
		FString Type;    // "info" | "warning" | "error"
		FString Message;
		FString File;
		int32 Line = 0;  // 0 = no line info on the wire
	};

	struct FTestResultRecord
	{
		FString Name;
		FString FullPath;
		FString Status;       // StatusPassed / Failed / Skipped / Flaky
		int32 DurationMs = 0;
		TArray<FTestEventRecord> Events;
		TArray<FString> Artifacts;
	};

	/** Pass bookkeeping — a run has one primary pass and optionally one retry pass. */
	struct FPassState
	{
		/** Dotted full paths to run this pass. */
		TArray<FString> RequestedTests;

		/** Per-test results keyed by full path, populated as each test finishes. */
		TMap<FString, FTestResultRecord> Results;

		/**
		 * Index into `RequestedTests` for the currently-executing test.
		 * `Idle` means "no test currently driving the framework" — either
		 * we haven't started the first test yet, or we just finished one
		 * and will start the next on the next tick.
		 */
		static constexpr int32 Idle = INDEX_NONE;
		int32 CurrentIndex = Idle;
	};

	/**
	 * Overall run state. Progresses through:
	 *   Running (primary pass)
	 *     → optionally RunningRetry (retry pass of failed tests)
	 *     → Finished / Cancelled / TimedOut.
	 */
	struct FLiveRunState
	{
		FString Handle;                // "live-<32-hex>"
		double StartedAtSeconds = 0.0;
		double StartedAtUnixMs  = 0.0;

		double TimeoutSeconds = DefaultTimeoutSeconds;
		bool   bRetryOnceOnFlake = true;

		FString State = StateRunning;
		bool    bCancelRequested = false;

		/** The current pass. Swapped from primary to retry if retry fires. */
		FPassState CurrentPass;

		/**
		 * Primary-pass snapshot retained for the merge when retry kicks in.
		 * Empty while we're still in the primary pass.
		 */
		TMap<FString, FTestResultRecord> FirstPassResults;

		/** 1 = primary, 2 = after retry started. */
		int32 Attempts = 1;

		/** Ordered full paths for the run — stable index order for progress. */
		TArray<FString> OrderedFullPaths;

		/**
		 * Beautified-path → FAutomationTestInfo::GetTestName() mapping.
		 *
		 * The first arg to FAutomationTestFramework::StartTestByName is
		 * split on the first space (AutomationTest.cpp:631) to extract the
		 * registered test name before calling ContainsTest. Any beautified
		 * path containing spaces (all AFunctionalTest-derived tests live
		 * under "Project.Functional Tests") hit ContainsTest with a
		 * truncated prefix and silently log "Test X does not exist and
		 * could not be run" — the status stays empty / skipped.
		 *
		 * We resolve each path to its registered TestName at run-queue
		 * time (where KnownByPath is still in scope) and look it up in
		 * TryStartNextTest to pass as arg 1, keeping the beautified path
		 * as arg 3 (InFullTestPath) for display.
		 */
		TMap<FString, FString> FullPathToTestName;

		/** Finalised per-test results in `OrderedFullPaths` order. */
		TArray<FTestResultRecord> FinalResults;

		/** Valid once the run has finished (any terminal state). */
		bool bFinal = false;
	};

	/**
	 * Module-scoped runner. Only one active run at a time; stale handles
	 * for a previously-finished run are kept here so `tests.run_report`
	 * can be called after completion. A NEW `tests.run` replaces the slot
	 * (subject to the busy guard).
	 */
	static TUniquePtr<FLiveRunState> GActiveRun;

	/** Ticker that drives the active run's state machine. */
	static FTSTicker::FDelegateHandle GTickerHandle;

	// -------------------------------------------------------------------- //
	// Helpers
	// -------------------------------------------------------------------- //

	/** Build an inline error payload: `{error, message, detail?}` at the root. */
	static TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}

	/** Same as above with a detail object. */
	static TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message,
		const TSharedRef<FJsonObject>& Detail)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		Out->SetObjectField(TEXT("detail"), Detail);
		return Out;
	}

	/** Mint a new "live-<32-hex>" handle. */
	static FString MintHandle()
	{
		const FGuid Guid = FGuid::NewGuid();
		return FString::Printf(TEXT("live-%s"),
			*Guid.ToString(EGuidFormats::Digits).ToLower());
	}

	/** Map UE's event severity to our wire type. */
	static FString EventTypeString(EAutomationEventType Type)
	{
		switch (Type)
		{
			case EAutomationEventType::Error:   return TEXT("error");
			case EAutomationEventType::Warning: return TEXT("warning");
			case EAutomationEventType::Info:
			default:                            return TEXT("info");
		}
	}

	/** Convert one FAutomationExecutionEntry into our FTestEventRecord. */
	static FTestEventRecord TranslateEntry(const FAutomationExecutionEntry& Entry)
	{
		FTestEventRecord Rec;
		Rec.Type    = EventTypeString(Entry.Event.Type);
		Rec.Message = Entry.Event.Message;
		Rec.File    = Entry.Filename;
		// Engine sentinel -1 means "no line info"; map to 0 for "omit".
		Rec.Line    = (Entry.LineNumber > 0) ? Entry.LineNumber : 0;
		return Rec;
	}

	/** Serialise a FTestEventRecord to JSON. */
	static TSharedRef<FJsonObject> BuildEventJson(const FTestEventRecord& Rec)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("type"), Rec.Type);
		Out->SetStringField(TEXT("message"), Rec.Message);
		if (!Rec.File.IsEmpty())
		{
			Out->SetStringField(TEXT("file"), Rec.File);
		}
		if (Rec.Line > 0)
		{
			Out->SetNumberField(TEXT("line"), Rec.Line);
		}
		return Out;
	}

	/** Serialise a FTestResultRecord to JSON. */
	static TSharedRef<FJsonObject> BuildTestResultJson(
		const FTestResultRecord& Rec, bool bIncludeEvents)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("name"), Rec.Name);
		Out->SetStringField(TEXT("full_path"), Rec.FullPath);
		Out->SetStringField(TEXT("status"), Rec.Status);
		Out->SetNumberField(TEXT("duration_ms"), Rec.DurationMs);

		if (bIncludeEvents)
		{
			TArray<TSharedPtr<FJsonValue>> EventsJson;
			EventsJson.Reserve(Rec.Events.Num());
			for (const FTestEventRecord& E : Rec.Events)
			{
				EventsJson.Add(MakeShared<FJsonValueObject>(BuildEventJson(E)));
			}
			Out->SetArrayField(TEXT("events"), EventsJson);

			if (Rec.Artifacts.Num() > 0)
			{
				TArray<TSharedPtr<FJsonValue>> ArtJson;
				ArtJson.Reserve(Rec.Artifacts.Num());
				for (const FString& A : Rec.Artifacts)
				{
					ArtJson.Add(MakeShared<FJsonValueString>(A));
				}
				Out->SetArrayField(TEXT("artifacts"), ArtJson);
			}
		}
		return Out;
	}

	/**
	 * Build a test-info lookup (full-path → info) from the framework's
	 * registered test set. Used to validate requested names and to
	 * resolve display-name + flags metadata we stamp on results.
	 */
	static void BuildKnownTestIndex(TMap<FString, FAutomationTestInfo>& OutByPath)
	{
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

		// Maximally permissive filter — see UeMcpTestHandlers.cpp for why.
		const EAutomationTestFlags AllFlags =
			EAutomationTestFlags_ApplicationContextMask
			| EAutomationTestFlags::SmokeFilter
			| EAutomationTestFlags::EngineFilter
			| EAutomationTestFlags::ProductFilter
			| EAutomationTestFlags::PerfFilter
			| EAutomationTestFlags::StressFilter
			| EAutomationTestFlags::NegativeFilter;
		Framework.SetRequestedTestFilter(AllFlags);

		TArray<FAutomationTestInfo> All;
		Framework.GetValidTestNames(All);

		OutByPath.Reserve(All.Num());
		for (const FAutomationTestInfo& Info : All)
		{
			OutByPath.Add(Info.GetFullTestPath(), Info);
		}
	}

	/** Count terminal per-test statuses. */
	struct FSummaryCounts
	{
		int32 Total = 0;
		int32 Passed = 0;
		int32 Failed = 0;
		int32 Skipped = 0;
		int32 DurationMs = 0;
	};

	static FSummaryCounts ComputeSummary(const TArray<FTestResultRecord>& Results)
	{
		FSummaryCounts S;
		S.Total = Results.Num();
		for (const FTestResultRecord& R : Results)
		{
			// Flaky counts as passed for the headline — matches the subprocess
			// runner and the merge rule in report_parser.py.
			if (R.Status == StatusPassed || R.Status == StatusFlaky)
			{
				++S.Passed;
			}
			else if (R.Status == StatusFailed)
			{
				++S.Failed;
			}
			else
			{
				++S.Skipped;
			}
			S.DurationMs += R.DurationMs;
		}
		return S;
	}

	/** Merge primary-pass results with a retry pass. Same rule as test_runner.py. */
	static TArray<FTestResultRecord> MergeRetry(
		const TMap<FString, FTestResultRecord>& FirstPass,
		const TMap<FString, FTestResultRecord>& SecondPass,
		const TArray<FString>& OrderedFullPaths)
	{
		TArray<FTestResultRecord> Merged;
		Merged.Reserve(OrderedFullPaths.Num());
		for (const FString& Path : OrderedFullPaths)
		{
			const FTestResultRecord* First = FirstPass.Find(Path);
			if (!First)
			{
				// Shouldn't happen — primary pass ran every test. Defensive.
				continue;
			}
			const FTestResultRecord* Second = SecondPass.Find(Path);
			if (!Second)
			{
				// Not re-run (only failures are retried) — keep the primary
				// result verbatim.
				Merged.Add(*First);
				continue;
			}

			if (Second->Status == StatusPassed)
			{
				// Flake — promote to flaky. Take the second pass's
				// duration / events as the authoritative ones, append the
				// primary-pass events after so callers can still see the
				// first failure's error messages.
				FTestResultRecord Flaky = *Second;
				Flaky.Status = StatusFlaky;
				Flaky.Events.Append(First->Events);
				Flaky.Artifacts.Append(First->Artifacts);
				Merged.Add(Flaky);
			}
			else
			{
				// Failed twice — the retry is the definitive result.
				Merged.Add(*Second);
			}
		}
		return Merged;
	}

	// -------------------------------------------------------------------- //
	// Ticker-driven state machine
	// -------------------------------------------------------------------- //

	/** Collect results from the framework after StopTest() for the last-run test. */
	static void HarvestExecutionInfo(
		const FAutomationTestExecutionInfo& Info,
		bool bSuccess,
		FTestResultRecord& OutRec)
	{
		// UE's Duration is in seconds.
		OutRec.DurationMs = static_cast<int32>(FMath::RoundToDouble(Info.Duration * 1000.0));

		bool bAnyErrorEvent = false;
		for (const FAutomationExecutionEntry& E : Info.GetEntries())
		{
			OutRec.Events.Add(TranslateEntry(E));
			if (E.Event.Type == EAutomationEventType::Error)
			{
				bAnyErrorEvent = true;
			}
		}

		// UE can report bSuccess=false without an explicit error entry (e.g.
		// early-exit via `AddError` pre-entry or a skip path). An error event
		// is the stronger signal — if present, the test is failed regardless.
		if (!bSuccess || bAnyErrorEvent)
		{
			OutRec.Status = StatusFailed;
		}
		else
		{
			OutRec.Status = StatusPassed;
		}
	}

	/**
	 * If the timeout budget has elapsed, mark the run timed_out and
	 * finalise. Returns true if the ticker should stop.
	 */
	static bool CheckTimeoutAndMaybeFinalise(FLiveRunState& Run)
	{
		const double Elapsed = FPlatformTime::Seconds() - Run.StartedAtSeconds;
		if (Elapsed < Run.TimeoutSeconds)
		{
			return false;
		}

		UE_LOG(LogUeMcpEditor, Warning,
			TEXT("tests.run[%s]: timeout after %.1fs; stopping."),
			*Run.Handle, Elapsed);

		// If a test is currently executing we cannot safely abort it mid-
		// flight (see file-header cancellation semantics). We force-stop
		// the framework via StopTest so downstream state is consistent,
		// then mark the run timed_out. Remaining tests are recorded as
		// skipped so the summary reflects reality.
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
		if (Framework.GetCurrentTest() != nullptr)
		{
			FAutomationTestExecutionInfo Info;
			const bool bSuccess = Framework.StopTest(Info);

			const int32 Idx = Run.CurrentPass.CurrentIndex;
			if (Run.CurrentPass.RequestedTests.IsValidIndex(Idx))
			{
				const FString& Path = Run.CurrentPass.RequestedTests[Idx];
				FTestResultRecord& Rec = Run.CurrentPass.Results.FindOrAdd(Path);
				Rec.FullPath = Path;
				if (Rec.Name.IsEmpty())
				{
					Rec.Name = Path;
				}
				HarvestExecutionInfo(Info, bSuccess, Rec);
			}
		}

		// Anything not yet run -> skipped.
		for (const FString& Path : Run.OrderedFullPaths)
		{
			if (!Run.CurrentPass.Results.Contains(Path))
			{
				FTestResultRecord Rec;
				Rec.FullPath = Path;
				Rec.Name = Path;
				Rec.Status = StatusSkipped;
				Run.CurrentPass.Results.Add(Path, Rec);
			}
		}

		// Finalise straight from the current pass (no retry after timeout).
		Run.FinalResults.Reserve(Run.OrderedFullPaths.Num());
		for (const FString& Path : Run.OrderedFullPaths)
		{
			if (const FTestResultRecord* R = Run.CurrentPass.Results.Find(Path))
			{
				Run.FinalResults.Add(*R);
			}
		}

		Run.State = StateTimedOut;
		Run.bFinal = true;
		return true;
	}

	/** Record a skip for a still-executing test on cancellation and move on. */
	static void ForceSkipRemainingOnCancel(FLiveRunState& Run)
	{
		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();
		if (Framework.GetCurrentTest() != nullptr)
		{
			// Same rationale as timeout path — we let StopTest collect what
			// it has so framework state is clean, even though cancellation
			// is the user-requested outcome.
			FAutomationTestExecutionInfo Info;
			const bool bSuccess = Framework.StopTest(Info);

			const int32 Idx = Run.CurrentPass.CurrentIndex;
			if (Run.CurrentPass.RequestedTests.IsValidIndex(Idx))
			{
				const FString& Path = Run.CurrentPass.RequestedTests[Idx];
				FTestResultRecord& Rec = Run.CurrentPass.Results.FindOrAdd(Path);
				Rec.FullPath = Path;
				if (Rec.Name.IsEmpty())
				{
					Rec.Name = Path;
				}
				HarvestExecutionInfo(Info, bSuccess, Rec);
			}
		}

		for (const FString& Path : Run.OrderedFullPaths)
		{
			if (!Run.CurrentPass.Results.Contains(Path))
			{
				FTestResultRecord Rec;
				Rec.FullPath = Path;
				Rec.Name = Path;
				Rec.Status = StatusSkipped;
				Run.CurrentPass.Results.Add(Path, Rec);
			}
		}

		Run.FinalResults.Reserve(Run.OrderedFullPaths.Num());
		for (const FString& Path : Run.OrderedFullPaths)
		{
			if (const FTestResultRecord* R = Run.CurrentPass.Results.Find(Path))
			{
				Run.FinalResults.Add(*R);
			}
		}

		Run.State = StateCancelled;
		Run.bFinal = true;
	}

	/**
	 * Start the next test in the current pass. Returns true if a test
	 * was started (ticker continues), false if the pass is done.
	 */
	static bool TryStartNextTest(FLiveRunState& Run)
	{
		FPassState& Pass = Run.CurrentPass;
		const int32 NextIdx = (Pass.CurrentIndex == FPassState::Idle) ? 0 : Pass.CurrentIndex + 1;
		if (!Pass.RequestedTests.IsValidIndex(NextIdx))
		{
			Pass.CurrentIndex = FPassState::Idle;
			return false;
		}

		Pass.CurrentIndex = NextIdx;
		const FString& Path = Pass.RequestedTests[NextIdx];

		// Seed a result record with name/full_path so the status endpoint
		// can report it as in-progress even before StopTest harvests.
		FTestResultRecord& Rec = Pass.Results.FindOrAdd(Path);
		Rec.FullPath = Path;
		if (Rec.Name.IsEmpty())
		{
			Rec.Name = Path;
		}

		// The 0 role index is the convention for single-machine local runs —
		// AutomationWorkerModule.cpp:699 does the same.
		//
		// StartTestByName splits arg-1 on the FIRST space to extract the
		// registered test name. Passing the beautified `Path` directly
		// (which contains spaces for AFunctionalTest hierarchies like
		// "Project.Functional Tests.FTEST_X") silently skips the run.
		// We look up the non-space-containing TestName cached at queue
		// time, falling back to Path only for tests that somehow slipped
		// through the index (shouldn't happen after TEST_NOT_FOUND guard,
		// but we keep the fallback for cancel/retry edge cases).
		const FString* CachedTestName = Run.FullPathToTestName.Find(Path);
		const FString& TestCommand = CachedTestName ? *CachedTestName : Path;

		FAutomationTestFramework::Get().StartTestByName(
			TestCommand, /*RoleIndex=*/ 0, Path);

		UE_LOG(LogUeMcpEditor, Verbose,
			TEXT("tests.run[%s]: StartTestByName(command=%s, full_path=%s)"),
			*Run.Handle, *TestCommand, *Path);
		return true;
	}

	/**
	 * Begin the retry pass over all tests that failed in the primary pass.
	 * Returns true if a retry pass was started; false if there's nothing
	 * to retry (run can finalise).
	 */
	static bool TryBeginRetryPass(FLiveRunState& Run)
	{
		if (!Run.bRetryOnceOnFlake || Run.Attempts >= 2)
		{
			return false;
		}

		TArray<FString> Failed;
		for (const FString& Path : Run.OrderedFullPaths)
		{
			if (const FTestResultRecord* R = Run.CurrentPass.Results.Find(Path))
			{
				if (R->Status == StatusFailed)
				{
					Failed.Add(Path);
				}
			}
		}
		if (Failed.Num() == 0)
		{
			return false;
		}

		UE_LOG(LogUeMcpEditor, Log,
			TEXT("tests.run[%s]: %d failing test(s); retrying once."),
			*Run.Handle, Failed.Num());

		Run.FirstPassResults = Run.CurrentPass.Results;
		Run.Attempts = 2;
		Run.CurrentPass = FPassState();
		Run.CurrentPass.RequestedTests = MoveTemp(Failed);
		return true;
	}

	/** Finalise the run — merge retry (if any) and lock in FinalResults. */
	static void Finalise(FLiveRunState& Run)
	{
		if (Run.FirstPassResults.Num() > 0)
		{
			Run.FinalResults = MergeRetry(
				Run.FirstPassResults,
				Run.CurrentPass.Results,
				Run.OrderedFullPaths);
		}
		else
		{
			Run.FinalResults.Reserve(Run.OrderedFullPaths.Num());
			for (const FString& Path : Run.OrderedFullPaths)
			{
				if (const FTestResultRecord* R = Run.CurrentPass.Results.Find(Path))
				{
					Run.FinalResults.Add(*R);
				}
			}
		}
		Run.State = StateFinished;
		Run.bFinal = true;
	}

	/**
	 * Ticker body. One call per engine tick. Returns true so the ticker
	 * stays registered for the plugin's lifetime — we don't pay to
	 * unregister/reregister per run.
	 */
	static bool TickRunner(float /*DeltaTime*/)
	{
		if (!GActiveRun.IsValid())
		{
			return true;
		}

		FLiveRunState& Run = *GActiveRun;

		if (Run.bFinal)
		{
			// Finished — nothing to do this tick. The record stays resident
			// so `tests.run_report` can be queried after completion.
			return true;
		}

		// Cooperative cancellation — safe points only, per file-header note.
		if (Run.bCancelRequested)
		{
			// If a test is still mid-execution, `HarvestExecutionInfo` inside
			// ForceSkipRemainingOnCancel will stop it before we finalise.
			ForceSkipRemainingOnCancel(Run);
			return true;
		}

		if (CheckTimeoutAndMaybeFinalise(Run))
		{
			return true;
		}

		FAutomationTestFramework& Framework = FAutomationTestFramework::Get();

		// Is there a test running right now?
		if (Framework.GetCurrentTest() != nullptr)
		{
			// Give the framework a tick to drive its latent commands. The
			// call is the same one AutomationWorkerModule uses every frame
			// — if it returns true the test's latent queue is empty.
			const bool bAllLatentDone = Framework.ExecuteLatentCommands();
			if (!bAllLatentDone)
			{
				return true; // still waiting for latent commands
			}

			// Harvest — StopTest moves the framework out of GIsAutomationTesting.
			FAutomationTestExecutionInfo Info;
			const bool bSuccess = Framework.StopTest(Info);

			const int32 Idx = Run.CurrentPass.CurrentIndex;
			if (Run.CurrentPass.RequestedTests.IsValidIndex(Idx))
			{
				const FString& Path = Run.CurrentPass.RequestedTests[Idx];
				FTestResultRecord& Rec = Run.CurrentPass.Results.FindOrAdd(Path);
				Rec.FullPath = Path;
				if (Rec.Name.IsEmpty())
				{
					Rec.Name = Path;
				}
				HarvestExecutionInfo(Info, bSuccess, Rec);
			}

			// Don't immediately start the next test this same tick — give
			// the editor a breath to let the previous test's teardown
			// finalise. The next tick will call TryStartNextTest.
			return true;
		}

		// No test running — start the next one, or transition phases.
		if (TryStartNextTest(Run))
		{
			return true;
		}

		// Primary or retry pass complete.
		if (Run.Attempts == 1 && TryBeginRetryPass(Run))
		{
			return true;
		}

		Finalise(Run);
		return true;
	}

	// -------------------------------------------------------------------- //
	// Handlers
	// -------------------------------------------------------------------- //

	/** `tests.run` body. */
	static TSharedRef<FJsonObject> HandleTestsRun(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		UE_LOG(LogUeMcpEditor, Verbose, TEXT("tests.run dispatch"));

		if (GEditor == nullptr)
		{
			return MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; tests.run cannot run"));
		}

		// --- Busy guard: only one active run at a time. ---
		// We also refuse if the framework is currently executing ANY test
		// (e.g. one fired from the Session Frontend manually) — the
		// framework is single-active, and stomping on it would corrupt
		// both the external run and ours.
		if (GActiveRun.IsValid() && !GActiveRun->bFinal)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("active_handle"), GActiveRun->Handle);
			Detail->SetStringField(TEXT("state"), GActiveRun->State);
			return MakeInlineError(
				TEXT("EDITOR_BUSY"),
				TEXT("A test run is already in flight; call tests.run_cancel or wait for it to finish."),
				Detail);
		}
		if (FAutomationTestFramework::Get().GetCurrentTest() != nullptr)
		{
			return MakeInlineError(
				TEXT("AUTOMATION_CONTROLLER_UNAVAILABLE"),
				TEXT("Another test is currently executing outside the MCP runner; wait for it to finish."));
		}

		// --- Parse args ---
		const TArray<TSharedPtr<FJsonValue>>* TestsArrayPtr = nullptr;
		if (!Args->TryGetArrayField(TEXT("tests"), TestsArrayPtr) || TestsArrayPtr == nullptr)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("field"), TEXT("tests"));
			return MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("Missing required field `tests` (array of strings)."),
				Detail);
		}

		TArray<FString> RequestedTests;
		RequestedTests.Reserve(TestsArrayPtr->Num());
		for (const TSharedPtr<FJsonValue>& V : *TestsArrayPtr)
		{
			if (!V.IsValid() || V->Type != EJson::String)
			{
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("field"), TEXT("tests"));
				return MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					TEXT("`tests` must be an array of non-empty strings."),
					Detail);
			}
			const FString S = V->AsString().TrimStartAndEnd();
			if (!S.IsEmpty())
			{
				RequestedTests.Add(S);
			}
		}
		if (RequestedTests.Num() == 0)
		{
			return MakeInlineError(
				TEXT("TEST_FILTER_MATCHED_ZERO"),
				TEXT("`tests` array is empty after trimming; nothing to run."));
		}

		double TimeoutSeconds = DefaultTimeoutSeconds;
		{
			double Parsed = 0.0;
			if (Args->TryGetNumberField(TEXT("timeout_seconds"), Parsed) && Parsed > 0.0)
			{
				TimeoutSeconds = Parsed;
			}
		}

		bool bRetryOnceOnFlake = true;
		Args->TryGetBoolField(TEXT("retry_once_on_flake"), bRetryOnceOnFlake);

		// --- Validate requested tests against the framework's known set. ---
		TMap<FString, FAutomationTestInfo> KnownByPath;
		BuildKnownTestIndex(KnownByPath);

		TArray<FString> Unknown;
		for (const FString& T : RequestedTests)
		{
			if (!KnownByPath.Contains(T))
			{
				Unknown.Add(T);
			}
		}
		if (Unknown.Num() > 0)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> UnknownJson;
			UnknownJson.Reserve(Unknown.Num());
			for (const FString& U : Unknown)
			{
				UnknownJson.Add(MakeShared<FJsonValueString>(U));
			}
			Detail->SetArrayField(TEXT("unknown"), UnknownJson);
			Detail->SetNumberField(TEXT("num_known"), KnownByPath.Num());
			return MakeInlineError(
				TEXT("TEST_NOT_FOUND"),
				FString::Printf(TEXT("%d requested test name(s) are not registered."), Unknown.Num()),
				Detail);
		}

		// --- Construct run state ---
		TUniquePtr<FLiveRunState> Run = MakeUnique<FLiveRunState>();
		Run->Handle = MintHandle();
		Run->StartedAtSeconds = FPlatformTime::Seconds();
		Run->StartedAtUnixMs  = static_cast<double>(FDateTime::UtcNow().ToUnixTimestamp()) * 1000.0;
		Run->TimeoutSeconds   = TimeoutSeconds;
		Run->bRetryOnceOnFlake = bRetryOnceOnFlake;
		Run->OrderedFullPaths  = RequestedTests;
		Run->CurrentPass.RequestedTests = RequestedTests;

		// Pre-populate name metadata from the info table so the status
		// endpoint can return nice names before any test finishes. Also
		// cache the FullPath -> TestName mapping so TryStartNextTest can
		// pass the non-space-containing registered name as arg 1 to
		// StartTestByName (see FullPathToTestName comment above).
		for (const FString& Path : RequestedTests)
		{
			if (const FAutomationTestInfo* Info = KnownByPath.Find(Path))
			{
				FTestResultRecord& Rec = Run->CurrentPass.Results.FindOrAdd(Path);
				Rec.FullPath = Path;
				Rec.Name = Info->GetDisplayName();
				Run->FullPathToTestName.Add(Path, Info->GetTestName());
			}
		}

		GActiveRun = MoveTemp(Run);

		UE_LOG(LogUeMcpEditor, Log,
			TEXT("tests.run[%s]: queued %d test(s), timeout=%.1fs, retry=%s"),
			*GActiveRun->Handle, RequestedTests.Num(), TimeoutSeconds,
			bRetryOnceOnFlake ? TEXT("true") : TEXT("false"));

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("handle"), GActiveRun->Handle);
		Data->SetStringField(TEXT("state"), StateRunning);
		Data->SetNumberField(TEXT("total"), RequestedTests.Num());
		Data->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
		Data->SetBoolField(TEXT("retry_once_on_flake"), bRetryOnceOnFlake);
		Data->SetNumberField(TEXT("started_at_unix_ms"), GActiveRun->StartedAtUnixMs);
		return Data;
	}

	/** `tests.run_status` body. */
	static TSharedRef<FJsonObject> HandleTestsRunStatus(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Handle;
		if (!Args->TryGetStringField(TEXT("handle"), Handle) || Handle.IsEmpty())
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("field"), TEXT("handle"));
			return MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("Missing required field `handle`."),
				Detail);
		}

		if (!GActiveRun.IsValid() || GActiveRun->Handle != Handle)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("handle"), Handle);
			return MakeInlineError(
				TEXT("NOT_FOUND"),
				TEXT("No such live run handle."),
				Detail);
		}

		FLiveRunState& Run = *GActiveRun;

		const int32 Total = Run.OrderedFullPaths.Num();
		int32 Completed = 0;

		// Build completed_tests as a compact window. Keep it small per
		// contract — the full report lives in `tests.run_report`.
		//
		// When final: return the tail of the merged final results (most
		// recent N=32). When in-progress: return every test whose status
		// has been set in the current pass.
		constexpr int32 WindowSize = 32;
		TArray<TSharedPtr<FJsonValue>> CompletedJson;
		if (Run.bFinal)
		{
			Completed = Run.FinalResults.Num();
			const int32 Start = FMath::Max(0, Run.FinalResults.Num() - WindowSize);
			CompletedJson.Reserve(Run.FinalResults.Num() - Start);
			for (int32 i = Start; i < Run.FinalResults.Num(); ++i)
			{
				TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
				Obj->SetStringField(TEXT("name"), Run.FinalResults[i].Name);
				Obj->SetStringField(TEXT("full_path"), Run.FinalResults[i].FullPath);
				Obj->SetStringField(TEXT("status"), Run.FinalResults[i].Status);
				CompletedJson.Add(MakeShared<FJsonValueObject>(Obj));
			}
		}
		else
		{
			for (const FString& Path : Run.OrderedFullPaths)
			{
				const FTestResultRecord* R = Run.CurrentPass.Results.Find(Path);
				// A finalised result has a non-empty status string; the
				// pre-seeded name/full_path alone doesn't count as done.
				if (R && !R->Status.IsEmpty())
				{
					++Completed;
					TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
					Obj->SetStringField(TEXT("name"), R->Name);
					Obj->SetStringField(TEXT("full_path"), R->FullPath);
					Obj->SetStringField(TEXT("status"), R->Status);
					CompletedJson.Add(MakeShared<FJsonValueObject>(Obj));
				}
			}
		}

		TSharedRef<FJsonObject> Progress = MakeShared<FJsonObject>();
		Progress->SetNumberField(TEXT("completed"), Completed);
		Progress->SetNumberField(TEXT("total"), Total);

		const double ElapsedMs = (FPlatformTime::Seconds() - Run.StartedAtSeconds) * 1000.0;

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("handle"), Run.Handle);
		Data->SetStringField(TEXT("state"), Run.State);
		Data->SetObjectField(TEXT("progress"), Progress);
		Data->SetNumberField(TEXT("elapsed_ms"), FMath::RoundToDouble(FMath::Max(ElapsedMs, 0.0)));
		Data->SetArrayField(TEXT("completed_tests"), CompletedJson);
		Data->SetNumberField(TEXT("attempt"), Run.Attempts);
		return Data;
	}

	/**
	 * Build the `{handle, state, summary, tests, attempts, ...}` payload
	 * shared by `tests.run_report` and `tests.await_completion`. When
	 * `Run.bFinal` is false the payload reports the in-progress slice (only
	 * tests whose status has already been set in the current pass).
	 */
	static TSharedRef<FJsonObject> BuildRunReportPayload(const FLiveRunState& Run)
	{
		TArray<FTestResultRecord> Results;
		if (Run.bFinal)
		{
			Results = Run.FinalResults;
		}
		else
		{
			Results.Reserve(Run.OrderedFullPaths.Num());
			for (const FString& Path : Run.OrderedFullPaths)
			{
				if (const FTestResultRecord* R = Run.CurrentPass.Results.Find(Path))
				{
					if (!R->Status.IsEmpty())
					{
						Results.Add(*R);
					}
				}
			}
		}

		const FSummaryCounts S = ComputeSummary(Results);

		TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
		Summary->SetNumberField(TEXT("total"), S.Total);
		Summary->SetNumberField(TEXT("passed"), S.Passed);
		Summary->SetNumberField(TEXT("failed"), S.Failed);
		Summary->SetNumberField(TEXT("skipped"), S.Skipped);
		Summary->SetNumberField(TEXT("duration_ms"), S.DurationMs);

		TArray<TSharedPtr<FJsonValue>> TestsJson;
		TestsJson.Reserve(Results.Num());
		for (const FTestResultRecord& R : Results)
		{
			TestsJson.Add(MakeShared<FJsonValueObject>(BuildTestResultJson(R, /*bIncludeEvents=*/ true)));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("handle"), Run.Handle);
		Data->SetStringField(TEXT("state"), Run.State);
		Data->SetObjectField(TEXT("summary"), Summary);
		Data->SetArrayField(TEXT("tests"), TestsJson);
		Data->SetNumberField(TEXT("attempts"), Run.Attempts);
		Data->SetNumberField(TEXT("started_at_unix_ms"), Run.StartedAtUnixMs);
		Data->SetNumberField(TEXT("elapsed_ms"),
			FMath::RoundToDouble(FMath::Max((FPlatformTime::Seconds() - Run.StartedAtSeconds) * 1000.0, 0.0)));
		return Data;
	}

	/** `tests.run_report` body. */
	static TSharedRef<FJsonObject> HandleTestsRunReport(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Handle;
		if (!Args->TryGetStringField(TEXT("handle"), Handle) || Handle.IsEmpty())
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("field"), TEXT("handle"));
			return MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("Missing required field `handle`."),
				Detail);
		}

		if (!GActiveRun.IsValid() || GActiveRun->Handle != Handle)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("handle"), Handle);
			return MakeInlineError(
				TEXT("NOT_FOUND"),
				TEXT("No such live run handle."),
				Detail);
		}

		return BuildRunReportPayload(*GActiveRun);
	}

	/** `tests.run_cancel` body. */
	static TSharedRef<FJsonObject> HandleTestsRunCancel(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Handle;
		if (!Args->TryGetStringField(TEXT("handle"), Handle) || Handle.IsEmpty())
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("field"), TEXT("handle"));
			return MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("Missing required field `handle`."),
				Detail);
		}

		if (!GActiveRun.IsValid() || GActiveRun->Handle != Handle)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("handle"), Handle);
			return MakeInlineError(
				TEXT("NOT_FOUND"),
				TEXT("No such live run handle."),
				Detail);
		}

		FLiveRunState& Run = *GActiveRun;

		// Idempotent: cancelling an already-terminal run is a no-op success.
		if (Run.bFinal)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("handle"), Run.Handle);
			Data->SetStringField(TEXT("state"), Run.State);
			Data->SetBoolField(TEXT("cancel_signalled"), false);
			Data->SetBoolField(TEXT("already_final"), true);
			return Data;
		}

		Run.bCancelRequested = true;

		UE_LOG(LogUeMcpEditor, Log,
			TEXT("tests.run_cancel[%s]: cancel flag set; observed at next safe point."),
			*Run.Handle);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("handle"), Run.Handle);
		Data->SetStringField(TEXT("state"), Run.State);
		Data->SetBoolField(TEXT("cancel_signalled"), true);
		Data->SetBoolField(TEXT("already_final"), false);
		return Data;
	}

	// -------------------------------------------------------------------- //
	// tests.await_completion — pending handler that parks until the active
	// run reaches a terminal state or the caller's `timeout_seconds` budget
	// elapses. Closes the `tests.run` + poll-loop fusion candidate from
	// nightly issue #14.
	//
	// HEARTBEAT-WALL CONSTRAINT: any pending handler that parks the
	// transport thread for >~14s drops the Python client connection
	// (heartbeat ping every 15s, 5s tolerance — see
	// `server/src/unreal_test_mcp/connection.py::_HEARTBEAT_*`). The
	// FastMCP wrapper for this tool therefore polls
	// `tests.run_status` Python-side and never parks the plugin for
	// more than one status tick; the plugin handler here is the
	// lower-level primitive direct `conn.call` callers can use when
	// they know the run is short (smokes, micro-benchmarks). Long
	// callers should use the FastMCP wrapper.
	// -------------------------------------------------------------------- //

	/** Per-request state for the await_completion pending step. */
	struct FAwaitCompletionState
	{
		FString  Handle;
		double   StartSeconds = 0.0;
		double   TimeoutSeconds = AwaitDefaultTimeoutSeconds;
		double   PollIntervalSeconds = static_cast<double>(AwaitDefaultPollMs) / 1000.0;
		double   LastPollSeconds = -1.0; // -1 => poll on first invocation.
		int32    PollCount = 0;
		FUeMcpCancelToken* Cancel = nullptr;
	};

	/**
	 * Build a step closure that returns `Payload` immediately as Done. Used
	 * for the early-finish path (run already terminal at factory time) so
	 * the response shape matches the polled path verbatim.
	 */
	static FUeMcpPendingStep MakeImmediateAwaitDoneStep(TSharedRef<FJsonObject> Payload)
	{
		return [Payload](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			Out = Payload;
			return EUeMcpStep::Done;
		};
	}

	/**
	 * Build a step closure that returns `Payload` immediately as Failed.
	 * Used for factory-time validation errors (missing handle, unknown
	 * handle, bad numeric fields) so the inline-error envelope flows
	 * through the normal pending channel rather than the executor's
	 * generic `InternalError`.
	 */
	static FUeMcpPendingStep MakeImmediateAwaitFailStep(TSharedRef<FJsonObject> Payload)
	{
		return [Payload](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			Out = Payload;
			return EUeMcpStep::Failed;
		};
	}

	/**
	 * `tests.await_completion` factory. Validates args, snapshots the
	 * active run by handle, returns either an immediate-Done step (if the
	 * run is already final) or a polling step that re-checks the run on
	 * each tick. The state-machine ticker continues to advance the run
	 * between our poll invocations because the executor invokes pending
	 * steps as part of the same per-tick service pass — the run's own
	 * FTSTicker fires on every game-thread tick regardless of whether a
	 * pending handler is parked.
	 */
	static FUeMcpPendingStep BuildAwaitCompletionStep(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		// --- Required: handle ---
		FString Handle;
		if (!Args->TryGetStringField(TEXT("handle"), Handle) || Handle.IsEmpty())
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("field"), TEXT("handle"));
			return MakeImmediateAwaitFailStep(MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("Missing required field `handle`."),
				Detail));
		}

		// --- Optional: timeout_seconds ---
		double TimeoutSeconds = AwaitDefaultTimeoutSeconds;
		{
			double Raw = 0.0;
			if (Args->TryGetNumberField(TEXT("timeout_seconds"), Raw))
			{
				if (Raw <= 0.0 || Raw > AwaitMaxTimeoutSeconds)
				{
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("field"), TEXT("timeout_seconds"));
					Detail->SetNumberField(TEXT("max"), AwaitMaxTimeoutSeconds);
					return MakeImmediateAwaitFailStep(MakeInlineError(
						TEXT("SCHEMA_ERROR"),
						FString::Printf(
							TEXT("`timeout_seconds` must be in (0, %.0f] (got %g)"),
							AwaitMaxTimeoutSeconds, Raw),
						Detail));
				}
				TimeoutSeconds = Raw;
			}
		}

		// --- Optional: poll_ms ---
		int32 PollMs = AwaitDefaultPollMs;
		{
			double Raw = 0.0;
			if (Args->TryGetNumberField(TEXT("poll_ms"), Raw))
			{
				const int32 Clamped = static_cast<int32>(Raw);
				if (Clamped < AwaitMinPollMs || Clamped > AwaitMaxPollMs)
				{
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("field"), TEXT("poll_ms"));
					Detail->SetNumberField(TEXT("min"), AwaitMinPollMs);
					Detail->SetNumberField(TEXT("max"), AwaitMaxPollMs);
					return MakeImmediateAwaitFailStep(MakeInlineError(
						TEXT("SCHEMA_ERROR"),
						FString::Printf(
							TEXT("`poll_ms` must be in [%d, %d] (got %d)"),
							AwaitMinPollMs, AwaitMaxPollMs, Clamped),
						Detail));
				}
				PollMs = Clamped;
			}
		}

		// --- Resolve handle against the active-run slot ---
		if (!GActiveRun.IsValid() || GActiveRun->Handle != Handle)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("handle"), Handle);
			return MakeImmediateAwaitFailStep(MakeInlineError(
				TEXT("NOT_FOUND"),
				TEXT("No such live run handle."),
				Detail));
		}

		// --- Fast-path: run already terminal — answer in one tick ---
		if (GActiveRun->bFinal)
		{
			TSharedRef<FJsonObject> Payload = BuildRunReportPayload(*GActiveRun);
			Payload->SetBoolField(TEXT("timed_out"), false);
			Payload->SetNumberField(TEXT("poll_count"), 0);
			return MakeImmediateAwaitDoneStep(Payload);
		}

		// --- Polling path: build per-request state + step closure ---
		TSharedRef<FAwaitCompletionState> S = MakeShared<FAwaitCompletionState>();
		S->Handle = Handle;
		S->StartSeconds = FPlatformTime::Seconds();
		S->TimeoutSeconds = TimeoutSeconds;
		S->PollIntervalSeconds = static_cast<double>(PollMs) / 1000.0;
		S->Cancel = &Cancel;

		return [S](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			check(IsInGameThread());

			const double NowSeconds = FPlatformTime::Seconds();
			const double ElapsedSeconds = NowSeconds - S->StartSeconds;

			// Cancel observed between ticks — emit a synthetic timed-out
			// report so callers get a structurally-identical response on
			// the cancel path. The plugin's own run state is not affected;
			// callers that wanted to abort the run should also issue
			// `tests.run_cancel`.
			if (S->Cancel != nullptr && S->Cancel->IsCancellationRequested())
			{
				if (GActiveRun.IsValid() && GActiveRun->Handle == S->Handle)
				{
					Out = BuildRunReportPayload(*GActiveRun);
				}
				else
				{
					Out = MakeShared<FJsonObject>();
					Out->SetStringField(TEXT("handle"), S->Handle);
				}
				Out->SetBoolField(TEXT("timed_out"), false);
				Out->SetBoolField(TEXT("cancelled"), true);
				Out->SetNumberField(TEXT("poll_count"), S->PollCount);
				return EUeMcpStep::Done;
			}

			// Throttle to `poll_ms`. The very first invocation always
			// polls (LastPoll == -1) so a `timeout_seconds=0` call still
			// yields one observation.
			if (S->LastPollSeconds >= 0.0
				&& (NowSeconds - S->LastPollSeconds) < S->PollIntervalSeconds)
			{
				if (ElapsedSeconds >= S->TimeoutSeconds)
				{
					if (GActiveRun.IsValid() && GActiveRun->Handle == S->Handle)
					{
						Out = BuildRunReportPayload(*GActiveRun);
					}
					else
					{
						Out = MakeShared<FJsonObject>();
						Out->SetStringField(TEXT("handle"), S->Handle);
					}
					Out->SetBoolField(TEXT("timed_out"), true);
					Out->SetNumberField(TEXT("poll_count"), S->PollCount);
					return EUeMcpStep::Done;
				}
				return EUeMcpStep::Continue;
			}

			S->LastPollSeconds = NowSeconds;
			++S->PollCount;

			// The active-run slot can in principle be replaced by a sibling
			// `tests.run` if the prior run finalised — but our busy guard
			// prevents that while a non-final run is in flight, and we
			// already established the run was non-final at factory time.
			// If the slot is still ours and now terminal, return Done.
			if (GActiveRun.IsValid() && GActiveRun->Handle == S->Handle)
			{
				if (GActiveRun->bFinal)
				{
					Out = BuildRunReportPayload(*GActiveRun);
					Out->SetBoolField(TEXT("timed_out"), false);
					Out->SetNumberField(TEXT("poll_count"), S->PollCount);
					return EUeMcpStep::Done;
				}
			}
			else
			{
				// Slot stolen from under us (shouldn't happen — busy
				// guard prevents it). Surface NOT_FOUND.
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("handle"), S->Handle);
				Out = MakeInlineError(
					TEXT("NOT_FOUND"),
					TEXT("Run handle no longer resident in the active-run slot."),
					Detail);
				return EUeMcpStep::Failed;
			}

			// Timeout check after the poll so timeout_seconds=0 still
			// yields at least one read attempt.
			if (ElapsedSeconds >= S->TimeoutSeconds)
			{
				Out = BuildRunReportPayload(*GActiveRun);
				Out->SetBoolField(TEXT("timed_out"), true);
				Out->SetNumberField(TEXT("poll_count"), S->PollCount);
				return EUeMcpStep::Done;
			}

			return EUeMcpStep::Continue;
		};
	}
}

void UeMcp::RegisterTestRunHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpTestRunHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.run"));
		Reg.DefaultTimeoutSeconds = RunDispatcherTimeoutSeconds;
		// Marked mutating: the run mutates editor test state even though
		// the handler body's own work is a quick state insert.
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsRun);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.run_status"));
		Reg.DefaultTimeoutSeconds = RunStatusDispatcherTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsRunStatus);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.run_report"));
		Reg.DefaultTimeoutSeconds = RunReportDispatcherTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsRunReport);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.run_cancel"));
		Reg.DefaultTimeoutSeconds = RunCancelDispatcherTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsRunCancel);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.await_completion"));
		// Pure read; the handler parks via PendingHandler. Dispatcher
		// timeout is a hard ceiling above the per-call max so the in-
		// handler timeout (default 60s, max 600s) wins the race.
		Reg.DefaultTimeoutSeconds = AwaitDispatcherTimeoutSeconds;
		Reg.bMutating = false;
		Reg.PendingHandler = FUeMcpToolPendingHandler::CreateStatic(&BuildAwaitCompletionStep);
		Dispatcher.RegisterTool(Reg);
	}

	// Install the state-machine ticker once. It's cheap when no run is
	// active (early-out on `GActiveRun.IsValid()`), and survives across
	// runs so we don't churn handles per `tests.run`.
	if (!UeMcpTestRunHandlersPrivate::GTickerHandle.IsValid())
	{
		UeMcpTestRunHandlersPrivate::GTickerHandle =
			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateStatic(&UeMcpTestRunHandlersPrivate::TickRunner),
				UeMcpTestRunHandlersPrivate::TickerIntervalSeconds);
	}
}

void UeMcp::ShutdownTestRunner()
{
	using namespace UeMcpTestRunHandlersPrivate;

	check(IsInGameThread());

	if (GTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(GTickerHandle);
		GTickerHandle.Reset();
	}

	// If a run is still mid-flight at shutdown, best-effort stop the
	// framework so we don't leak a GIsAutomationTesting=true state into
	// the next subsystem lifetime (editor hot-reload scenario).
	if (GActiveRun.IsValid() && !GActiveRun->bFinal)
	{
		if (FAutomationTestFramework::Get().GetCurrentTest() != nullptr)
		{
			FAutomationTestExecutionInfo Discard;
			FAutomationTestFramework::Get().StopTest(Discard);
		}
	}

	GActiveRun.Reset();
}
