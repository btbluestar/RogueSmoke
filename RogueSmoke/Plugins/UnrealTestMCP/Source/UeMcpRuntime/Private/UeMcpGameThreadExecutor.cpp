// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpGameThreadExecutor.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Misc/Timespan.h"
#include "UObject/UObjectGlobals.h"

#include "UeMcpRequestErrorDevice.h"

DEFINE_LOG_CATEGORY(LogUeMcpRuntime);

namespace UeMcpGameThreadExecutorPrivate
{
	/** Hard sanity cap on caller-provided timeouts. */
	static constexpr double MaxTimeoutSeconds = 600.0;

	/** Max frames we'll defer for GC/save/async-load before surfacing EDITOR_BUSY. */
	static constexpr int32 MaxDeferRetries = 600;
}

/**
 * Per-execution state. Lives on the heap behind a ThreadSafe `TSharedRef` so
 * both the caller (blocked in `ExecuteOnGameThread`) and the ticker lambda
 * hold strong references. The last-ref-drops destructor returns the
 * `FEvent*` to the platform pool, which makes the lifetime of the event
 * exactly the lifetime of the state — no risk of a late tick writing
 * through a pool-handed-out event pointer.
 */
struct FUeMcpGameThreadExecutor::FSharedExecState
{
	// Exactly one of Work / PendingFactory is set per request. `PendingStep`
	// is lazily built from `PendingFactory` on the first tick that services
	// a pending request; once built, subsequent ticks re-invoke it until it
	// returns Done/Failed or the request is otherwise retired.
	TFunction<TSharedRef<FJsonObject>(FUeMcpCancelToken&)> Work;
	FUeMcpPendingFactory PendingFactory;
	FUeMcpPendingStep PendingStep;
	bool bPending = false;

	TSharedRef<FUeMcpCancelToken, ESPMode::ThreadSafe> CancelToken;

	/** Auto-reset event — one producer (ticker) signals, one consumer (caller) waits. */
	FEvent* DoneEvent = nullptr;

	/** Guards bAbandoned, Result, ErrorMessage, Outcome, DeferRetries, captured buffers. */
	FCriticalSection AbandonmentLock;

	/**
	 * True once the caller has timed out (or the executor is being destroyed)
	 * and will no longer consume the result. The ticker uses this to decide
	 * whether to still run the handler (it doesn't) and to clean up.
	 */
	bool bAbandoned = false;

	/** Payload populated by the ticker; read by the caller on successful signal. */
	TSharedPtr<FJsonObject> Result;
	FString ErrorMessage;
	EUeMcpExecResult Outcome = EUeMcpExecResult::InternalError;
	double StartTime = 0.0;
	int32 DeferRetries = 0;

	TArray<FString> CapturedWarnings;
	TArray<FString> CapturedErrors;

	FGuid RequestId;

	FSharedExecState()
		: CancelToken(MakeShared<FUeMcpCancelToken, ESPMode::ThreadSafe>())
	{
		DoneEvent = FPlatformProcess::GetSynchEventFromPool(/*bIsManualReset*/ false);
	}

	~FSharedExecState()
	{
		if (DoneEvent != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(DoneEvent);
			DoneEvent = nullptr;
		}
	}
};

FUeMcpGameThreadExecutor::FUeMcpGameThreadExecutor()
{
	// Register a per-frame ticker on the core ticker (game thread). Interval
	// 0 means "every frame." Deliberately not TaskGraph — some subsystems
	// (InterchangeEngine, etc.) schedule their own TaskGraph work and the
	// recursion asserts.
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUeMcpGameThreadExecutor::Tick),
		/*InDelay*/ 0.0f);
}

FUeMcpGameThreadExecutor::~FUeMcpGameThreadExecutor()
{
	// Remove ourselves from the ticker first so no new tick observes us
	// mid-teardown.
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}

	// Abandon + signal every outstanding caller so they unblock.
	TArray<TSharedRef<FSharedExecState, ESPMode::ThreadSafe>> Outstanding;
	{
		FScopeLock Lock(&InFlightLock);
		Outstanding.Reserve(InFlight.Num());
		for (auto& Pair : InFlight)
		{
			Outstanding.Add(Pair.Value);
		}
		InFlight.Reset();
	}

	for (const TSharedRef<FSharedExecState, ESPMode::ThreadSafe>& State : Outstanding)
	{
		FScopeLock StateLock(&State->AbandonmentLock);
		if (!State->bAbandoned)
		{
			State->bAbandoned = true;
			// Report as TimedOut — the caller semantics are "we did not deliver
			// a real result." A distinct Cancelled could be argued for, but
			// the caller didn't request the cancel so TimedOut is honest.
			State->Outcome = EUeMcpExecResult::TimedOut;
			State->ErrorMessage = TEXT("Executor shutting down before request completed");
			if (State->DoneEvent != nullptr)
			{
				State->DoneEvent->Trigger();
			}
		}
	}
}

FUeMcpExecReport FUeMcpGameThreadExecutor::ExecuteOnGameThread(
	const FGuid& RequestId,
	TFunction<TSharedRef<FJsonObject>(FUeMcpCancelToken&)> Work,
	double TimeoutSeconds)
{
	using namespace UeMcpGameThreadExecutorPrivate;

	// We deadlock with the ticker if invoked from the game thread. Callers
	// must already be on the transport / async task path.
	check(!IsInGameThread());

	FUeMcpExecReport Report;

	if (!Work)
	{
		Report.Result = EUeMcpExecResult::InternalError;
		Report.ErrorMessage = TEXT("ExecuteOnGameThread called with null work");
		return Report;
	}

	// Clamp timeout. A zero-or-negative value means "use the sanity ceiling."
	double EffectiveTimeout = TimeoutSeconds;
	if (EffectiveTimeout <= 0.0)
	{
		EffectiveTimeout = MaxTimeoutSeconds;
	}
	else if (EffectiveTimeout > MaxTimeoutSeconds)
	{
		EffectiveTimeout = MaxTimeoutSeconds;
	}

	TSharedRef<FSharedExecState, ESPMode::ThreadSafe> State = MakeShared<FSharedExecState, ESPMode::ThreadSafe>();
	State->Work = MoveTemp(Work);
	State->StartTime = FPlatformTime::Seconds();
	State->RequestId = RequestId;

	{
		FScopeLock Lock(&InFlightLock);
		// If the caller reuses an id (shouldn't happen, but be defensive) we
		// overwrite. The prior state's ticker lambda still has its own ref
		// and will clean up as bAbandoned.
		if (InFlight.Contains(RequestId))
		{
			UE_LOG(LogUeMcpRuntime, Warning,
				TEXT("ExecuteOnGameThread: request id %s already in flight; replacing"),
				*RequestId.ToString(EGuidFormats::Digits));

			TSharedRef<FSharedExecState, ESPMode::ThreadSafe> Existing = InFlight[RequestId];
			FScopeLock StateLock(&Existing->AbandonmentLock);
			Existing->bAbandoned = true;
			if (Existing->DoneEvent != nullptr)
			{
				Existing->DoneEvent->Trigger();
			}
		}
		InFlight.Add(RequestId, State);
	}

	UE_LOG(LogUeMcpRuntime, Verbose,
		TEXT("ExecuteOnGameThread: submitted request %s with timeout %.3fs"),
		*RequestId.ToString(EGuidFormats::Digits), EffectiveTimeout);

	// Wait for the ticker to signal. `FEvent::Wait(FTimespan)` returns true
	// on signal, false on timeout.
	const FTimespan WaitTimespan = FTimespan::FromSeconds(EffectiveTimeout);
	const bool bSignalled = State->DoneEvent->Wait(WaitTimespan);

	if (bSignalled)
	{
		FScopeLock StateLock(&State->AbandonmentLock);
		Report.Result = State->Outcome;
		Report.Data = State->Result;
		Report.ErrorMessage = State->ErrorMessage;
		Report.ElapsedMs = (FPlatformTime::Seconds() - State->StartTime) * 1000.0;
		Report.CapturedWarnings = MoveTemp(State->CapturedWarnings);
		Report.CapturedErrors = MoveTemp(State->CapturedErrors);

		UE_LOG(LogUeMcpRuntime, Verbose,
			TEXT("ExecuteOnGameThread: request %s completed outcome=%d in %.2fms"),
			*RequestId.ToString(EGuidFormats::Digits),
			(int32)Report.Result, Report.ElapsedMs);
	}
	else
	{
		// Timeout. Flag bAbandoned under the state's own lock; the ticker
		// will observe this on its next pass and remove the entry from
		// InFlight — we do NOT remove it here, because the ticker might be
		// mid-write into the state right now.
		FScopeLock StateLock(&State->AbandonmentLock);
		State->bAbandoned = true;
		State->CancelToken->RequestCancel();

		Report.Result = EUeMcpExecResult::TimedOut;
		Report.ErrorMessage = FString::Printf(
			TEXT("Request timed out after %.3fs"), EffectiveTimeout);
		Report.ElapsedMs = (FPlatformTime::Seconds() - State->StartTime) * 1000.0;

		UE_LOG(LogUeMcpRuntime, Warning,
			TEXT("ExecuteOnGameThread: request %s timed out after %.2fms"),
			*RequestId.ToString(EGuidFormats::Digits), Report.ElapsedMs);
	}

	// Our local `State` shared ref drops at function exit. The ticker lambda
	// still holds a ref if the request was abandoned; when the ticker
	// observes bAbandoned, removes from InFlight, and drops its ref, the
	// state destructor returns the FEvent to the pool.
	return Report;
}

FUeMcpExecReport FUeMcpGameThreadExecutor::ExecuteOnGameThreadPending(
	const FGuid& RequestId,
	FUeMcpPendingFactory Factory,
	double TimeoutSeconds)
{
	using namespace UeMcpGameThreadExecutorPrivate;

	// Deadlocks with the ticker if called from the game thread, same as the
	// synchronous path.
	check(!IsInGameThread());

	FUeMcpExecReport Report;

	if (!Factory)
	{
		Report.Result = EUeMcpExecResult::InternalError;
		Report.ErrorMessage = TEXT("ExecuteOnGameThreadPending called with null factory");
		return Report;
	}

	double EffectiveTimeout = TimeoutSeconds;
	if (EffectiveTimeout <= 0.0)
	{
		EffectiveTimeout = MaxTimeoutSeconds;
	}
	else if (EffectiveTimeout > MaxTimeoutSeconds)
	{
		EffectiveTimeout = MaxTimeoutSeconds;
	}

	TSharedRef<FSharedExecState, ESPMode::ThreadSafe> State =
		MakeShared<FSharedExecState, ESPMode::ThreadSafe>();
	State->PendingFactory = MoveTemp(Factory);
	State->bPending = true;
	State->StartTime = FPlatformTime::Seconds();
	State->RequestId = RequestId;

	{
		FScopeLock Lock(&InFlightLock);
		if (InFlight.Contains(RequestId))
		{
			UE_LOG(LogUeMcpRuntime, Warning,
				TEXT("ExecuteOnGameThreadPending: request id %s already in flight; replacing"),
				*RequestId.ToString(EGuidFormats::Digits));

			TSharedRef<FSharedExecState, ESPMode::ThreadSafe> Existing = InFlight[RequestId];
			FScopeLock StateLock(&Existing->AbandonmentLock);
			Existing->bAbandoned = true;
			if (Existing->DoneEvent != nullptr)
			{
				Existing->DoneEvent->Trigger();
			}
		}
		InFlight.Add(RequestId, State);
	}

	UE_LOG(LogUeMcpRuntime, Verbose,
		TEXT("ExecuteOnGameThreadPending: submitted request %s with timeout %.3fs"),
		*RequestId.ToString(EGuidFormats::Digits), EffectiveTimeout);

	const FTimespan WaitTimespan = FTimespan::FromSeconds(EffectiveTimeout);
	const bool bSignalled = State->DoneEvent->Wait(WaitTimespan);

	if (bSignalled)
	{
		FScopeLock StateLock(&State->AbandonmentLock);
		Report.Result = State->Outcome;
		Report.Data = State->Result;
		Report.ErrorMessage = State->ErrorMessage;
		Report.ElapsedMs = (FPlatformTime::Seconds() - State->StartTime) * 1000.0;
		Report.CapturedWarnings = MoveTemp(State->CapturedWarnings);
		Report.CapturedErrors = MoveTemp(State->CapturedErrors);

		UE_LOG(LogUeMcpRuntime, Verbose,
			TEXT("ExecuteOnGameThreadPending: request %s completed outcome=%d in %.2fms"),
			*RequestId.ToString(EGuidFormats::Digits),
			(int32)Report.Result, Report.ElapsedMs);
	}
	else
	{
		FScopeLock StateLock(&State->AbandonmentLock);
		State->bAbandoned = true;
		State->CancelToken->RequestCancel();

		Report.Result = EUeMcpExecResult::TimedOut;
		Report.ErrorMessage = FString::Printf(
			TEXT("Request timed out after %.3fs"), EffectiveTimeout);
		Report.ElapsedMs = (FPlatformTime::Seconds() - State->StartTime) * 1000.0;

		UE_LOG(LogUeMcpRuntime, Warning,
			TEXT("ExecuteOnGameThreadPending: request %s timed out after %.2fms"),
			*RequestId.ToString(EGuidFormats::Digits), Report.ElapsedMs);
	}

	return Report;
}

void FUeMcpGameThreadExecutor::Cancel(const FGuid& RequestId)
{
	TSharedPtr<FSharedExecState, ESPMode::ThreadSafe> State;
	{
		FScopeLock Lock(&InFlightLock);
		if (const TSharedRef<FSharedExecState, ESPMode::ThreadSafe>* Found = InFlight.Find(RequestId))
		{
			State = *Found;
		}
	}

	if (!State.IsValid())
	{
		return;
	}

	// Flip the cooperative flag and let the handler return on its own.
	// We deliberately do NOT set bAbandoned here — the caller is still
	// waiting for a report and the handler should deliver Cancelled rather
	// than the caller seeing a post-hoc timeout.
	State->CancelToken->RequestCancel();

	UE_LOG(LogUeMcpRuntime, Verbose,
		TEXT("Cancel: cooperative cancel requested for %s"),
		*RequestId.ToString(EGuidFormats::Digits));
}

int32 FUeMcpGameThreadExecutor::NumInFlight() const
{
	FScopeLock Lock(&InFlightLock);
	return InFlight.Num();
}

bool FUeMcpGameThreadExecutor::Tick(float DeltaTime)
{
	using namespace UeMcpGameThreadExecutorPrivate;

	// Snapshot the in-flight list under the outer lock, then iterate
	// without it held so we only acquire the per-state lock and never
	// hold both at the same time.
	TArray<TSharedRef<FSharedExecState, ESPMode::ThreadSafe>> Snapshot;
	{
		FScopeLock Lock(&InFlightLock);
		Snapshot.Reserve(InFlight.Num());
		for (auto& Pair : InFlight)
		{
			Snapshot.Add(Pair.Value);
		}
	}

	// Engine-safety check is cheap — hoist it out of the loop. If any of
	// these flags is set we defer every pending request uniformly.
	const bool bEngineBusy =
		GIsSavingPackage || IsGarbageCollecting() || IsAsyncLoading();

	TArray<FGuid> ToErase;
	ToErase.Reserve(Snapshot.Num());

	for (const TSharedRef<FSharedExecState, ESPMode::ThreadSafe>& State : Snapshot)
	{
		// Phase 1 — inspect state under the lock to decide whether to run.
		// We explicitly DO NOT hold the lock while invoking the handler;
		// handlers may run for seconds and the caller's timeout path needs
		// to be able to grab the lock to flip bAbandoned.
		bool bShouldRun = false;
		bool bShortCircuitDone = false;

		{
			FScopeLock StateLock(&State->AbandonmentLock);

			if (State->bAbandoned)
			{
				// Caller has given up. Clean up without running the handler.
				ToErase.Add(State->RequestId);
				continue;
			}

			if (bEngineBusy)
			{
				State->DeferRetries++;
				if (State->DeferRetries > MaxDeferRetries)
				{
					State->Outcome = EUeMcpExecResult::DeferredEngineBusy;
					State->ErrorMessage = TEXT(
						"Editor remained busy (GC / package save / async load) beyond the defer budget");
					State->CapturedWarnings.Reset();
					State->CapturedErrors.Reset();

					UE_LOG(LogUeMcpRuntime, Warning,
						TEXT("Tick: request %s exceeded defer budget (%d); surfacing EDITOR_BUSY"),
						*State->RequestId.ToString(EGuidFormats::Digits), State->DeferRetries);

					if (State->DoneEvent != nullptr)
					{
						State->DoneEvent->Trigger();
					}
					ToErase.Add(State->RequestId);
				}
				// Otherwise just skip this tick; we'll retry next frame.
				continue;
			}

			// Honour a cooperative cancel that fired before the work ran.
			if (State->CancelToken->IsCancellationRequested())
			{
				State->Outcome = EUeMcpExecResult::Cancelled;
				State->ErrorMessage = TEXT("Cancelled before execution");
				if (State->DoneEvent != nullptr)
				{
					State->DoneEvent->Trigger();
				}
				ToErase.Add(State->RequestId);
				bShortCircuitDone = true;
			}
			else
			{
				bShouldRun = true;
			}
		}

		if (bShortCircuitDone || !bShouldRun)
		{
			continue;
		}

		// Phase 2 — run the handler WITHOUT holding the AbandonmentLock. If
		// the caller times out while the handler is running, it can still
		// acquire the lock to set bAbandoned and request cancel; the handler
		// will notice the cancel via its cooperative poll and return.
		//
		// Pending-handler branch: build the step closure lazily on first
		// entry, then invoke it once. Return `Continue` keeps the request
		// in InFlight for another tick; `Done`/`Failed` publishes and
		// retires the same way the synchronous path does.
		if (State->bPending)
		{
			// Build the step closure on first entry. Any validation failure
			// the factory wants to report should come via a closure that
			// returns `Failed` on its first invocation — a null return from
			// the factory is the "factory itself broke" path.
			if (!State->PendingStep)
			{
				FUeMcpPendingStep NewStep;
				FString FactoryError;
				bool bFactoryOk = false;
#if PLATFORM_EXCEPTIONS_DISABLED
				{
					NewStep = State->PendingFactory(State->CancelToken.Get());
					bFactoryOk = true;
				}
#else
				try
				{
					NewStep = State->PendingFactory(State->CancelToken.Get());
					bFactoryOk = true;
				}
				catch (const std::exception& Ex)
				{
					FactoryError = FString::Printf(TEXT("Pending factory threw std::exception: %s"),
						ANSI_TO_TCHAR(Ex.what()));
				}
				catch (...)
				{
					FactoryError = TEXT("Pending factory threw an unknown exception");
				}
#endif

				if (!bFactoryOk || !NewStep)
				{
					FScopeLock StateLock(&State->AbandonmentLock);
					if (!State->bAbandoned)
					{
						State->Outcome = EUeMcpExecResult::InternalError;
						State->ErrorMessage = FactoryError.IsEmpty()
							? TEXT("Pending factory returned a null step closure")
							: FactoryError;
					}
					if (State->DoneEvent != nullptr)
					{
						State->DoneEvent->Trigger();
					}
					ToErase.Add(State->RequestId);
					continue;
				}

				State->PendingStep = MoveTemp(NewStep);
				// Drop the factory — it's done its job, and holding it keeps
				// whatever it captured alive for the lifetime of the request.
				State->PendingFactory = FUeMcpPendingFactory();

				// Cancel may have fired in the gap between the Phase-1
				// cancel check and the factory completing. Re-check before
				// invoking the step so a cancel during a slow factory
				// doesn't get one wasted step invocation. (Not a
				// correctness bug — the post-step check below also catches
				// it — but it skips the step's first poll when we already
				// know we're abandoning.)
				if (State->CancelToken->IsCancellationRequested())
				{
					FScopeLock StateLock(&State->AbandonmentLock);
					if (!State->bAbandoned)
					{
						State->Outcome = EUeMcpExecResult::Cancelled;
						State->ErrorMessage = TEXT("Cancelled before first step");
					}
					if (State->DoneEvent != nullptr)
					{
						State->DoneEvent->Trigger();
					}
					ToErase.Add(State->RequestId);
					continue;
				}
			}

			// Invoke the step once, bracketed by the error-capture device.
			// Warnings/errors accumulate across ticks into the state's buffers.
			FUeMcpRequestErrorDevice ErrorDevice;
			ErrorDevice.Attach();

			TSharedRef<FJsonObject> StepResult = MakeShared<FJsonObject>();
			EUeMcpStep StepOutcome = EUeMcpStep::Failed;
			FString StepError;
			bool bStepOk = false;

#if PLATFORM_EXCEPTIONS_DISABLED
			{
				StepOutcome = State->PendingStep(StepResult);
				bStepOk = true;
			}
#else
			try
			{
				StepOutcome = State->PendingStep(StepResult);
				bStepOk = true;
			}
			catch (const std::exception& Ex)
			{
				StepError = FString::Printf(TEXT("Pending step threw std::exception: %s"),
					ANSI_TO_TCHAR(Ex.what()));
			}
			catch (...)
			{
				StepError = TEXT("Pending step threw an unknown exception");
			}
#endif

			ErrorDevice.Detach();
			TArray<FString> StepWarnings;
			TArray<FString> StepErrors;
			ErrorDevice.Drain(StepWarnings, StepErrors);

			{
				FScopeLock StateLock(&State->AbandonmentLock);

				// Accumulate captures across ticks. The synchronous path
				// replaces the buffers wholesale on completion because it
				// runs exactly once; the pending path appends because a
				// request spans many ticks and the handler-produced log
				// output can come from any of them.
				State->CapturedWarnings.Append(MoveTemp(StepWarnings));
				State->CapturedErrors.Append(MoveTemp(StepErrors));

				if (State->bAbandoned)
				{
					// Caller timed out; clean up without publishing.
					if (State->DoneEvent != nullptr)
					{
						State->DoneEvent->Trigger();
					}
					ToErase.Add(State->RequestId);
					continue;
				}

				if (!bStepOk)
				{
					State->Outcome = EUeMcpExecResult::InternalError;
					State->ErrorMessage = StepError;
					if (State->DoneEvent != nullptr)
					{
						State->DoneEvent->Trigger();
					}
					ToErase.Add(State->RequestId);
					continue;
				}

				// Cooperative cancel observed while the step was running:
				// honour it regardless of what the step returned.
				if (State->CancelToken->IsCancellationRequested())
				{
					State->Outcome = EUeMcpExecResult::Cancelled;
					State->ErrorMessage = TEXT("Cancelled");
					if (State->DoneEvent != nullptr)
					{
						State->DoneEvent->Trigger();
					}
					ToErase.Add(State->RequestId);
					continue;
				}

				switch (StepOutcome)
				{
				case EUeMcpStep::Continue:
					// Keep the request alive for another tick. Do NOT add
					// to ToErase, do NOT trigger the event.
					break;

				case EUeMcpStep::Done:
				case EUeMcpStep::Failed:
					// Both map to Success at the executor level; if the
					// payload has a top-level `error` field the dispatcher
					// hoists it into the wire-level error shape.
					State->Outcome = EUeMcpExecResult::Success;
					State->Result = StepResult;
					if (State->DoneEvent != nullptr)
					{
						State->DoneEvent->Trigger();
					}
					ToErase.Add(State->RequestId);
					break;
				}
			}

			continue;
		}

		// Synchronous handler branch — runs the work once and completes.
		FUeMcpRequestErrorDevice ErrorDevice;
		ErrorDevice.Attach();

		TSharedPtr<FJsonObject> Result;
		FString HandlerError;
		bool bHandlerOk = false;

#if PLATFORM_EXCEPTIONS_DISABLED
		{
			TSharedRef<FJsonObject> RawResult = State->Work(State->CancelToken.Get());
			Result = RawResult;
			bHandlerOk = true;
		}
#else
		try
		{
			TSharedRef<FJsonObject> RawResult = State->Work(State->CancelToken.Get());
			Result = RawResult;
			bHandlerOk = true;
		}
		catch (const std::exception& Ex)
		{
			HandlerError = FString::Printf(TEXT("Handler threw std::exception: %s"),
				ANSI_TO_TCHAR(Ex.what()));
		}
		catch (...)
		{
			HandlerError = TEXT("Handler threw an unknown exception");
		}
#endif

		ErrorDevice.Detach();
		TArray<FString> Warnings;
		TArray<FString> Errors;
		ErrorDevice.Drain(Warnings, Errors);

		// Phase 3 — publish the outcome under the lock. If the caller
		// abandoned the request while we were running, the caller has
		// already returned TimedOut with nothing; we still complete the
		// cleanup bookkeeping and signal the event so the state can be
		// retired cleanly.
		{
			FScopeLock StateLock(&State->AbandonmentLock);

			State->CapturedWarnings = MoveTemp(Warnings);
			State->CapturedErrors = MoveTemp(Errors);

			if (!State->bAbandoned)
			{
				if (bHandlerOk)
				{
					if (State->CancelToken->IsCancellationRequested())
					{
						State->Outcome = EUeMcpExecResult::Cancelled;
						State->ErrorMessage = TEXT("Cancelled");
					}
					else
					{
						State->Outcome = EUeMcpExecResult::Success;
						State->Result = Result;
					}
				}
				else
				{
					State->Outcome = EUeMcpExecResult::InternalError;
					State->ErrorMessage = HandlerError;
				}

				if (State->DoneEvent != nullptr)
				{
					State->DoneEvent->Trigger();
				}
			}
			// If abandoned, we still trigger the event — it's harmless
			// (auto-reset; nobody's waiting) and ensures the event is in a
			// predictable state before the ref drops.
			else if (State->DoneEvent != nullptr)
			{
				State->DoneEvent->Trigger();
			}
		}

		ToErase.Add(State->RequestId);
	}

	if (ToErase.Num() > 0)
	{
		FScopeLock Lock(&InFlightLock);
		for (const FGuid& Id : ToErase)
		{
			InFlight.Remove(Id);
		}
	}

	// Return true to stay registered with the ticker.
	return true;
}
