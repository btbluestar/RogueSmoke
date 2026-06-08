// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpPieHandlers.h"

#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Delegates/IDelegateInstance.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "EditorSubsystem.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "PlayInEditorDataTypes.h"

#include "UeMcpDispatcher.h"
#include "UeMcpEditorSubsystem.h" // LogUeMcpEditor
#include "UeMcpGameThreadExecutor.h"

namespace UeMcpPieHandlersPrivate
{
	/** Defaults. PIE start can be slow on a cold cache; the dispatcher
	 *  timeout must exceed the caller's wait-budget default. */
	static constexpr double StartDispatcherTimeoutSeconds        = 60.0;
	static constexpr double StopDispatcherTimeoutSeconds         = 60.0;
	static constexpr double StatusDispatcherTimeoutSeconds       = 2.0;
	static constexpr double SetTimeDilationTimeoutSeconds        = 5.0;
	static constexpr double AdvanceFramesDispatcherTimeoutSeconds = 120.0;
	static constexpr double AdvanceSecondsDispatcherTimeoutSeconds = 120.0;

	/** Default wait budgets. */
	static constexpr float DefaultStartWaitSeconds = 30.0f;
	static constexpr float DefaultStopWaitSeconds  = 30.0f;

	/** Valid range for TimeDilation. */
	static constexpr float MinTimeDilation = 0.001f;
	static constexpr float MaxTimeDilation = 100.0f;

	/** Maximum frames / seconds we'll accept from a caller (sanity caps). */
	static constexpr int32 MaxAdvanceFrameCount  = 100000;
	static constexpr float MaxAdvanceSecondsArg  = 3600.0f;

	/**
	 * Module-scoped PIE start-time. Seeded by the `PostPIEStarted`
	 * delegate installed by `InitializePieTracking`; cleared by the
	 * `EndPIE` delegate. A negative value means "PIE not active".
	 */
	static double GPieStartTimeSeconds = -1.0;
	static FDelegateHandle GPostPIEStartedTrackingHandle;
	static FDelegateHandle GEndPIETrackingHandle;

	/** Build an inline error payload: `{error, message}` root. */
	static TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}

	/** True if PIE is currently active (editor + live PlayWorld). */
	static bool IsPieActive()
	{
		return GEditor != nullptr
			&& GEditor->IsPlayingSessionInEditor()
			&& GEditor->PlayWorld != nullptr;
	}

	/** Build the response data object for status queries. */
	static TSharedRef<FJsonObject> BuildStatusJson(bool bIncludeUptime)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

		const bool bActive = IsPieActive();
		Data->SetBoolField(TEXT("pie_active"), bActive);

		if (!bActive)
		{
			Data->SetField(TEXT("mode"), MakeShared<FJsonValueNull>());
			Data->SetField(TEXT("map"), MakeShared<FJsonValueNull>());
			Data->SetNumberField(TEXT("num_clients"), 1);
			if (bIncludeUptime)
			{
				Data->SetField(TEXT("uptime_ms"), MakeShared<FJsonValueNull>());
			}
			Data->SetBoolField(TEXT("has_begun_play"), false);
			return Data;
		}

		const bool bSimulate = GEditor->bIsSimulatingInEditor;
		Data->SetStringField(TEXT("mode"), bSimulate ? TEXT("sie") : TEXT("pie"));
		Data->SetStringField(TEXT("map"), GEditor->PlayWorld->GetPathName());
		// num_clients is static for M3; v2 will read it from
		// PlayInEditorSessionInfo::NumClientInstancesCreated.
		Data->SetNumberField(TEXT("num_clients"), 1);

		if (bIncludeUptime)
		{
			if (GPieStartTimeSeconds > 0.0)
			{
				const double NowSeconds = FPlatformTime::Seconds();
				const double Elapsed = (NowSeconds - GPieStartTimeSeconds) * 1000.0;
				Data->SetNumberField(TEXT("uptime_ms"),
					FMath::RoundToDouble(FMath::Max(Elapsed, 0.0)));
			}
			else
			{
				// PIE is active but we didn't observe the start — e.g. MCP
				// booted mid-PIE. Report null rather than fabricate 0.
				Data->SetField(TEXT("uptime_ms"), MakeShared<FJsonValueNull>());
			}
		}

		Data->SetBoolField(TEXT("has_begun_play"),
			GEditor->PlayWorld->HasBegunPlay());

		return Data;
	}

	/** `pie.start` body. */
	static TSharedRef<FJsonObject> HandlePieStart(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());
		UE_LOG(LogUeMcpEditor, Verbose, TEXT("pie.start dispatch"));

		if (GEditor == nullptr)
		{
			return MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; pie.start cannot run"));
		}

		// --- Parse args ---
		FString LevelPath;
		Args->TryGetStringField(TEXT("level_path"), LevelPath);
		// Empty string is treated the same as omitted (no map override).
		// This is a spec-hole fill: the plan leaves empty-vs-missing
		// undefined; we opt for the more permissive behavior because the
		// Python client sends `""` by default.

		int32 NumClients = 1;
		{
			int32 Parsed = 1;
			if (Args->TryGetNumberField(TEXT("num_clients"), Parsed))
			{
				NumClients = Parsed;
			}
		}
		if (NumClients != 1)
		{
			return MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("num_clients=%d not supported in M3; multi-client PIE is v2"),
					NumClients));
		}

		bool bSimulate = false;
		Args->TryGetBoolField(TEXT("simulate"), bSimulate);

		bool bWaitForBeginPlay = true;
		Args->TryGetBoolField(TEXT("wait_for_begin_play"), bWaitForBeginPlay);

		double WaitTimeoutSeconds = static_cast<double>(DefaultStartWaitSeconds);
		{
			double Parsed = WaitTimeoutSeconds;
			if (Args->TryGetNumberField(TEXT("wait_timeout_seconds"), Parsed) && Parsed > 0.0)
			{
				WaitTimeoutSeconds = Parsed;
			}
		}

		// --- Guard: already in PIE ---
		if (IsPieActive())
		{
			return MakeInlineError(
				TEXT("PIE_ACTIVE"),
				TEXT("PIE is already running; call pie.stop first"));
		}

		// FIRE-AND-FORGET design. Early revisions of this handler spin-
		// waited on `FEditorDelegates::PostPIEStarted` while pumping the
		// core ticker — that caused a crash, because our own game-thread
		// executor is ticked by `FTSTicker::GetCoreTicker()`, so
		// `PumpTickerAndSleep` re-entered our executor's Tick while the
		// same request was still in-flight. The re-entrance double-
		// invoked the handler, stacked PIE-delegate registrations, and
		// tripped a TSharedPtr::IsValid() assert when PIE eventually
		// broadcast its delegate across mutating subscribers.
		//
		// Fix: the plugin never blocks the game thread waiting for PIE.
		// We queue RequestPlaySession and return immediately. The
		// Python-side `pie.start` tool polls `pie.status` until
		// `pie_active=true` (and, when requested, `has_begun_play=true`)
		// or its own timeout. That keeps waits off the game thread and
		// the editor free to tick through the play-session machinery.
		//
		// The wait-related args (`wait_for_begin_play`, `wait_timeout_
		// seconds`) are accepted here for forward-compatible schema
		// stability but ignored — the Python tool honours them.

		FRequestPlaySessionParams Params;
		Params.WorldType = bSimulate
			? EPlaySessionWorldType::SimulateInEditor
			: EPlaySessionWorldType::PlayInEditor;
		Params.SessionDestination = EPlaySessionDestinationType::InProcess;
		if (!LevelPath.IsEmpty())
		{
			Params.GlobalMapOverride = LevelPath;
		}

		const int64 RequestedAtUnixMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000;
		GEditor->RequestPlaySession(Params);

		// Silence unused-parameter warnings for the wait-args we now
		// defer to the Python side.
		(void)bWaitForBeginPlay;
		(void)WaitTimeoutSeconds;
		(void)Cancel;

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("pie_start_requested"), true);
		Data->SetNumberField(TEXT("requested_at_unix_ms"),
			static_cast<double>(RequestedAtUnixMs));
		Data->SetStringField(TEXT("mode"),
			bSimulate ? TEXT("sie") : TEXT("pie"));
		if (!LevelPath.IsEmpty())
		{
			Data->SetStringField(TEXT("level_path"), LevelPath);
		}
		return Data;
	}

	/** `pie.stop` body. */
	static TSharedRef<FJsonObject> HandlePieStop(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());
		UE_LOG(LogUeMcpEditor, Verbose, TEXT("pie.stop dispatch"));

		if (GEditor == nullptr)
		{
			return MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; pie.stop cannot run"));
		}

		double WaitTimeoutSeconds = static_cast<double>(DefaultStopWaitSeconds);
		{
			double Parsed = WaitTimeoutSeconds;
			if (Args->TryGetNumberField(TEXT("wait_timeout_seconds"), Parsed) && Parsed > 0.0)
			{
				WaitTimeoutSeconds = Parsed;
			}
		}

		// Idempotent: stopping when not in PIE is success, flagged.
		if (!IsPieActive())
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("pie_active"), false);
			Data->SetBoolField(TEXT("pie_stop_requested"), false);
			Data->SetBoolField(TEXT("already_stopped"), true);
			Data->SetNumberField(TEXT("elapsed_ms"), 0);
			return Data;
		}

		// Fire-and-forget, per the same rationale as `pie.start`: we do
		// not spin-wait on the game thread. The Python tool polls
		// `pie.status` until `pie_active=false` (or its own timeout).
		GEditor->RequestEndPlayMap();

		(void)WaitTimeoutSeconds;
		(void)Cancel;

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("pie_active"), true);  // still true this tick
		Data->SetBoolField(TEXT("pie_stop_requested"), true);
		Data->SetBoolField(TEXT("already_stopped"), false);
		return Data;
	}

	/** `pie.status` body. */
	static TSharedRef<FJsonObject> HandlePieStatus(
		const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		UE_LOG(LogUeMcpEditor, VeryVerbose, TEXT("pie.status dispatch"));

		if (GEditor == nullptr)
		{
			return MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; pie.status cannot run"));
		}

		return BuildStatusJson(/*bIncludeUptime=*/true);
	}

	// -----------------------------------------------------------------------
	// pie.set_time_dilation
	// -----------------------------------------------------------------------

	/**
	 * `pie.set_time_dilation` body.
	 *
	 * Writes `AWorldSettings::TimeDilation` on the PIE world. Returns the
	 * previous value and the value actually applied.
	 *
	 * Engine API: `AWorldSettings::SetTimeDilation(float)` / `TimeDilation`
	 * getter. Both are accessible via `UWorld::GetWorldSettings()`.
	 * See Engine/Source/Runtime/Engine/Classes/GameFramework/WorldSettings.h.
	 */
	static TSharedRef<FJsonObject> HandleSetTimeDilation(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (!IsPieActive())
		{
			return MakeInlineError(
				TEXT("NOT_IN_PIE"),
				TEXT("pie.set_time_dilation requires an active PIE session"));
		}

		double ScaleArg = 1.0;
		if (!Args->TryGetNumberField(TEXT("scale"), ScaleArg))
		{
			return MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("pie.set_time_dilation: 'scale' is required"));
		}

		const float Scale = static_cast<float>(ScaleArg);
		if (Scale < MinTimeDilation || Scale > MaxTimeDilation)
		{
			return MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("pie.set_time_dilation: 'scale' must be in [%.3f..%.0f], got %f"),
					MinTimeDilation, MaxTimeDilation, Scale));
		}

		UWorld* PIEWorld = GEditor->PlayWorld;
		check(PIEWorld); // IsPieActive() guarantees this

		AWorldSettings* WS = PIEWorld->GetWorldSettings();
		if (!WS)
		{
			return MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("pie.set_time_dilation: GetWorldSettings() returned null"));
		}

		const float Prior = WS->TimeDilation;
		WS->SetTimeDilation(Scale);
		const float Applied = WS->TimeDilation; // re-read in case clamped by engine

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("prior"),   static_cast<double>(Prior));
		Data->SetNumberField(TEXT("applied"), static_cast<double>(Applied));
		return Data;
	}

	// -----------------------------------------------------------------------
	// pie.advance_frames
	// -----------------------------------------------------------------------

	/**
	 * Wall-time ceiling for `pie.advance_frames`. The plugin's socket
	 * server is one-request-at-a-time, so any call longer than the
	 * client's heartbeat budget (5s ping / 4s call timeout on the
	 * Python side) will break the heartbeat and drop the connection.
	 * Cap at 2s so callers always get a prompt response even if the
	 * editor is backgrounded and ticking at low frequency (idle-editor
	 * rates can be as low as 3 Hz, at which 60 requested frames would
	 * otherwise take 20 wall-seconds). On early-exit, frames_completed
	 * reports what actually ticked and `timed_out: true` is set.
	 */
	static constexpr double AdvanceFramesWallCapSeconds = 2.0;

	/** Per-request state for the pending `pie.advance_frames` handler. */
	struct FAdvanceFramesState
	{
		int32 TargetCount = 0;
		uint64 BaselineFrame = 0;  // GFrameCounter at first service
		bool bBaselineCaptured = false;
		double StartWallSeconds = 0.0;
	};

	/**
	 * `pie.advance_frames` factory.
	 *
	 * Registered as a pending handler. Step invocations are driven by
	 * `FTSTicker` which fires at the editor's ticker rate (can be slower
	 * than engine-frame rate when the editor is backgrounded or idle),
	 * so we can't count step invocations as "engine frames." Instead we
	 * snapshot `GFrameCounter` — the global game-thread frame counter
	 * that increments once per engine tick — at first service and
	 * return Done once `current - baseline >= count`. This gives exact
	 * frame counting without blocking the game thread, and is immune to
	 * core-ticker frequency variation.
	 */
	static FUeMcpPendingStep BuildAdvanceFramesStep(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (!IsPieActive())
		{
			TSharedRef<FJsonObject> Err = MakeInlineError(
				TEXT("NOT_IN_PIE"),
				TEXT("pie.advance_frames requires an active PIE session"));
			return [Err](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
			{
				Out = Err;
				return EUeMcpStep::Failed;
			};
		}

		int32 Count = 1;
		{
			int32 Parsed = 1;
			if (Args->TryGetNumberField(TEXT("count"), Parsed))
			{
				Count = Parsed;
			}
		}

		if (Count < 1 || Count > MaxAdvanceFrameCount)
		{
			TSharedRef<FJsonObject> Err = MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("pie.advance_frames: 'count' must be 1..%d, got %d"),
					MaxAdvanceFrameCount, Count));
			return [Err](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
			{
				Out = Err;
				return EUeMcpStep::Failed;
			};
		}

		// wait_for_tick_complete is accepted for API compatibility. In the
		// pending implementation every step invocation runs on a completed-
		// tick boundary already, so the flag has no effect.
		bool bWaitForTickComplete = true;
		Args->TryGetBoolField(TEXT("wait_for_tick_complete"), bWaitForTickComplete);
		(void)bWaitForTickComplete;

		TSharedRef<FAdvanceFramesState> S = MakeShared<FAdvanceFramesState>();
		S->TargetCount = Count;
		S->StartWallSeconds = FPlatformTime::Seconds();

		return [S](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			check(IsInGameThread());

			if (!S->bBaselineCaptured)
			{
				S->BaselineFrame = GFrameCounter;
				S->bBaselineCaptured = true;
				return EUeMcpStep::Continue;
			}

			const uint64 Elapsed = GFrameCounter - S->BaselineFrame;
			const double WallElapsed =
				FPlatformTime::Seconds() - S->StartWallSeconds;

			const bool bTargetMet = Elapsed >= static_cast<uint64>(S->TargetCount);
			const bool bWallExceeded = WallElapsed >= AdvanceFramesWallCapSeconds;

			if (!bTargetMet && !bWallExceeded)
			{
				return EUeMcpStep::Continue;
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("frames_requested"),
				static_cast<double>(S->TargetCount));
			Data->SetNumberField(TEXT("frames_completed"),
				static_cast<double>(Elapsed));
			Data->SetNumberField(TEXT("elapsed_ms"),
				FMath::RoundToDouble(WallElapsed * 1000.0));
			// Surface the early-exit signal so callers can distinguish
			// "all N frames ticked" from "wall cap hit after M frames."
			if (bWallExceeded && !bTargetMet)
			{
				Data->SetBoolField(TEXT("timed_out"), true);
			}
			Out = Data;
			return EUeMcpStep::Done;
		};
	}

	// -----------------------------------------------------------------------
	// pie.advance_seconds
	// -----------------------------------------------------------------------

	/**
	 * Per-request state for the pending `pie.advance_seconds` handler.
	 *
	 * The destructor is the safety-net for dilation restore: if the
	 * request was abandoned (caller timeout) or the executor shut down
	 * while we were mid-wait, the normal step-completion restore never
	 * ran. The shared-ref-drop guarantees this destructor fires exactly
	 * once regardless of which thread held the last ref.
	 *
	 * UObject access is game-thread-only, so off-thread destruction
	 * (typical when the caller's transport thread holds the last ref)
	 * dispatches the restore via `AsyncTask` instead of touching the
	 * WorldSettings pointer directly.
	 */
	struct FAdvanceSecondsState
	{
		double SecondsArg = 0.0;
		float  RequestedDilation = 100.0f;
		float  PriorDilation = 1.0f;
		float  ActualDilation = 1.0f;

		TWeakObjectPtr<AWorldSettings> WorldSettings;

		double StartWallSeconds = 0.0;
		double TargetWallSeconds = 0.0; // seconds / dilation
		bool bDilationApplied = false;
		bool bDilationRestored = false;

		~FAdvanceSecondsState()
		{
			if (!bDilationApplied || bDilationRestored)
			{
				return;
			}

			if (IsInGameThread())
			{
				if (AWorldSettings* WS = WorldSettings.Get())
				{
					WS->SetTimeDilation(PriorDilation);
				}
				return;
			}

			// Off-thread: dispatch the restore to the game thread.
			// TWeakObjectPtr is thread-safe to copy + check; the lambda
			// no-ops if PIE is already gone by the time it runs.
			TWeakObjectPtr<AWorldSettings> WSWeak = WorldSettings;
			const float Restore = PriorDilation;
			AsyncTask(ENamedThreads::GameThread, [WSWeak, Restore]()
			{
				if (AWorldSettings* WS = WSWeak.Get())
				{
					WS->SetTimeDilation(Restore);
				}
			});
		}
	};

	/**
	 * `pie.advance_seconds` factory.
	 *
	 * Registered as a pending handler: the step polls the wall clock each
	 * tick and returns Done once `seconds / dilation` has elapsed. Game
	 * thread is free to tick PIE / AI / physics between invocations —
	 * exactly what was broken by the old `FPlatformProcess::Sleep` path.
	 * The prior dilation is restored on completion (including the
	 * cancel / timeout paths, via TWeakObjectPtr liveness check).
	 */
	static FUeMcpPendingStep BuildAdvanceSecondsStep(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		if (!IsPieActive())
		{
			TSharedRef<FJsonObject> Err = MakeInlineError(
				TEXT("NOT_IN_PIE"),
				TEXT("pie.advance_seconds requires an active PIE session"));
			return [Err](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
			{
				Out = Err;
				return EUeMcpStep::Failed;
			};
		}

		double SecondsArg = 0.0;
		if (!Args->TryGetNumberField(TEXT("seconds"), SecondsArg) || SecondsArg <= 0.0)
		{
			TSharedRef<FJsonObject> Err = MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("pie.advance_seconds: 'seconds' must be a positive number"));
			return [Err](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
			{
				Out = Err;
				return EUeMcpStep::Failed;
			};
		}
		if (static_cast<float>(SecondsArg) > MaxAdvanceSecondsArg)
		{
			TSharedRef<FJsonObject> Err = MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("pie.advance_seconds: 'seconds' must be <= %.0f, got %f"),
					MaxAdvanceSecondsArg, SecondsArg));
			return [Err](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
			{
				Out = Err;
				return EUeMcpStep::Failed;
			};
		}

		double DilationArg = 100.0;
		Args->TryGetNumberField(TEXT("dilation"), DilationArg);

		UWorld* PIEWorld = GEditor->PlayWorld;
		check(PIEWorld);
		AWorldSettings* WS = PIEWorld->GetWorldSettings();
		if (!WS)
		{
			TSharedRef<FJsonObject> Err = MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("pie.advance_seconds: GetWorldSettings() returned null"));
			return [Err](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
			{
				Out = Err;
				return EUeMcpStep::Failed;
			};
		}

		TSharedRef<FAdvanceSecondsState> S = MakeShared<FAdvanceSecondsState>();
		S->SecondsArg = SecondsArg;
		S->RequestedDilation = FMath::Clamp(
			static_cast<float>(DilationArg), MinTimeDilation, MaxTimeDilation);
		S->WorldSettings = WS;
		S->StartWallSeconds = FPlatformTime::Seconds();

		// `Cancel` is captured into the step's closure so we can honour
		// cooperative cancel mid-wait AND restore dilation on exit.
		FUeMcpCancelToken* CancelPtr = &Cancel;

		return [S, CancelPtr](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			check(IsInGameThread());

			AWorldSettings* WS = S->WorldSettings.Get();

			// Apply dilation on the first tick. If the world went away
			// between factory-time and first service (unlikely but
			// possible during a PIE teardown), surface EDITOR_NOT_READY.
			if (!S->bDilationApplied)
			{
				if (!WS)
				{
					Out = MakeInlineError(
						TEXT("EDITOR_NOT_READY"),
						TEXT("pie.advance_seconds: WorldSettings went away before dilation was applied"));
					return EUeMcpStep::Failed;
				}
				S->PriorDilation = WS->TimeDilation;
				WS->SetTimeDilation(S->RequestedDilation);
				S->ActualDilation = WS->TimeDilation;
				S->TargetWallSeconds = S->SecondsArg / static_cast<double>(
					S->ActualDilation > 0.0f ? S->ActualDilation : 1.0f);
				S->bDilationApplied = true;
				// Fall through so we at least check timeout + cancel this
				// tick; for very short TargetWallSeconds (e.g. dilation
				// high) we may complete immediately.
			}

			const bool bCancelled = CancelPtr != nullptr
				&& CancelPtr->IsCancellationRequested();

			const double WallElapsed =
				FPlatformTime::Seconds() - S->StartWallSeconds;

			const bool bWallDone = WallElapsed >= S->TargetWallSeconds;

			if (!bCancelled && !bWallDone)
			{
				return EUeMcpStep::Continue;
			}

			// Completion — restore dilation, build response. Use a
			// re-fetch of the weak ptr in case the world died mid-wait.
			if (!S->bDilationRestored)
			{
				AWorldSettings* WS2 = S->WorldSettings.Get();
				if (WS2)
				{
					WS2->SetTimeDilation(S->PriorDilation);
				}
				S->bDilationRestored = true;
			}

			const double WallElapsedMs = WallElapsed * 1000.0;
			const double SecondsElapsed = WallElapsed *
				static_cast<double>(S->ActualDilation);

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetNumberField(TEXT("seconds_requested"), S->SecondsArg);
			Data->SetNumberField(TEXT("seconds_elapsed"),   SecondsElapsed);
			Data->SetNumberField(TEXT("dilation_used"),
				static_cast<double>(S->ActualDilation));
			Data->SetNumberField(TEXT("wall_ms"),
				FMath::RoundToDouble(WallElapsedMs));
			if (bCancelled)
			{
				Data->SetBoolField(TEXT("cancelled"), true);
			}
			Out = Data;
			return EUeMcpStep::Done;
		};
	}
}

void UeMcp::RegisterPieHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpPieHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("pie.start"));
		Reg.DefaultTimeoutSeconds = StartDispatcherTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandlePieStart);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("pie.stop"));
		Reg.DefaultTimeoutSeconds = StopDispatcherTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandlePieStop);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("pie.status"));
		Reg.DefaultTimeoutSeconds = StatusDispatcherTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandlePieStatus);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("pie.set_time_dilation"));
		Reg.DefaultTimeoutSeconds = SetTimeDilationTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSetTimeDilation);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("pie.advance_frames"));
		Reg.DefaultTimeoutSeconds = AdvanceFramesDispatcherTimeoutSeconds;
		Reg.bMutating = true;
		Reg.PendingHandler =
			FUeMcpToolPendingHandler::CreateStatic(&BuildAdvanceFramesStep);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("pie.advance_seconds"));
		Reg.DefaultTimeoutSeconds = AdvanceSecondsDispatcherTimeoutSeconds;
		Reg.bMutating = true;
		Reg.PendingHandler =
			FUeMcpToolPendingHandler::CreateStatic(&BuildAdvanceSecondsStep);
		Dispatcher.RegisterTool(Reg);
	}
}

void UeMcp::InitializePieTracking()
{
	using namespace UeMcpPieHandlersPrivate;

	check(IsInGameThread());

	if (GPostPIEStartedTrackingHandle.IsValid() || GEndPIETrackingHandle.IsValid())
	{
		UE_LOG(LogUeMcpEditor, Warning,
			TEXT("InitializePieTracking: already initialized; skipping"));
		return;
	}

	GPostPIEStartedTrackingHandle = FEditorDelegates::PostPIEStarted.AddLambda(
		[](const bool /*bIsSimulating*/)
		{
			GPieStartTimeSeconds = FPlatformTime::Seconds();
		});

	GEndPIETrackingHandle = FEditorDelegates::EndPIE.AddLambda(
		[](const bool /*bIsSimulating*/)
		{
			GPieStartTimeSeconds = -1.0;
		});

	UE_LOG(LogUeMcpEditor, Verbose, TEXT("PIE tracking delegates installed."));
}

void UeMcp::ShutdownPieTracking()
{
	using namespace UeMcpPieHandlersPrivate;

	check(IsInGameThread());

	if (GPostPIEStartedTrackingHandle.IsValid())
	{
		FEditorDelegates::PostPIEStarted.Remove(GPostPIEStartedTrackingHandle);
		GPostPIEStartedTrackingHandle.Reset();
	}
	if (GEndPIETrackingHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(GEndPIETrackingHandle);
		GEndPIETrackingHandle.Reset();
	}
	GPieStartTimeSeconds = -1.0;

	UE_LOG(LogUeMcpEditor, Verbose, TEXT("PIE tracking delegates removed."));
}
