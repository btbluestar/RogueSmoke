// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Templates/Atomic.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

/**
 * Shared log category for the UeMcpRuntime module.
 *
 * Declared here because the game-thread executor is the first code path
 * that actually logs; other runtime sources piggyback on the same category.
 * (No API-export macro: log category symbols are linked module-wide and
 * the plain `EXTERN` variant matches engine-wide convention.)
 */
DECLARE_LOG_CATEGORY_EXTERN(LogUeMcpRuntime, Log, All);

/**
 * Outcome of a single `ExecuteOnGameThread` call.
 *
 * `DeferredEngineBusy` is distinct from `TimedOut`: the executor never got
 * a chance to run the handler because the engine was in an unsafe state
 * (GC / package save / async load) for longer than the defer budget.
 */
enum class EUeMcpExecResult : uint8
{
	Success,
	TimedOut,
	Cancelled,
	DeferredEngineBusy,
	InternalError
};

/**
 * Cooperative cancellation token.
 *
 * Handler code polls `IsCancellationRequested()` between loop iterations.
 * The executor flips the flag via `RequestCancel()` when a caller issues a
 * cancel for this request's in-flight id. Backed by `TAtomic<bool>`, safe
 * to read/write from any thread.
 */
struct UEMCPRUNTIME_API FUeMcpCancelToken
{
	/** Worker-side: non-blocking poll for cooperative cancel. */
	bool IsCancellationRequested() const
	{
		return bCancelRequested.Load();
	}

	/** Executor-side: set the cancel flag. Idempotent. */
	void RequestCancel()
	{
		bCancelRequested.Store(true);
	}

private:
	TAtomic<bool> bCancelRequested { false };
};

/**
 * Report returned by `ExecuteOnGameThread`.
 *
 * On `Success`, `Data` is non-null and `ErrorMessage` is empty. On any
 * other outcome, `Data` is null and `ErrorMessage` carries a short
 * human-readable explanation. `CapturedWarnings` / `CapturedErrors` are
 * populated from the GLog output device for the duration of the handler.
 */
struct FUeMcpExecReport
{
	EUeMcpExecResult Result = EUeMcpExecResult::InternalError;
	TSharedPtr<FJsonObject> Data;
	FString ErrorMessage;
	double ElapsedMs = 0.0;
	TArray<FString> CapturedWarnings;
	TArray<FString> CapturedErrors;
};

/**
 * Outcome of a single invocation of a pending handler's step closure.
 *
 * Pending handlers run over multiple game-thread ticks — unlike the
 * synchronous `Work` path, which produces its result in one tick. Each
 * tick the executor invokes the step closure once; the return value
 * tells the executor whether to keep the request alive or retire it.
 *
 * `Done` and `Failed` both populate `OutResult`; the distinction is
 * intent: `Done` maps to the canonical success wire shape, `Failed` is
 * the handler-signalled structured-error path (the dispatcher hoists
 * `{error, message}` at the root of `OutResult` to the top-level error
 * response, identical to the synchronous hoist).
 */
enum class EUeMcpStep : uint8
{
	Continue, // re-invoke me next tick
	Done,     // OutResult populated; retire as Success
	Failed,   // OutResult populated (inline-error shape); retire as Success
};

/**
 * One step of a pending handler. Invoked once per game-thread tick until
 * it returns `Done` / `Failed` — or until the executor retires the request
 * due to timeout, cancel, or engine-busy-budget exhaustion.
 *
 * The closure captures its own polling state + a reference to the request's
 * `FUeMcpCancelToken` (read via `IsCancellationRequested()` between polls).
 * On `Done` / `Failed` it must populate `OutResult` with the wire payload.
 */
using FUeMcpPendingStep = TFunction<EUeMcpStep(TSharedRef<FJsonObject>& /*OutResult*/)>;

/**
 * Builds a pending step closure from the request's arguments. Runs once
 * per request, on the game thread, the first time the executor is able
 * to service it (engine not busy, not already cancelled). The factory
 * typically validates args up front, allocates per-request state, and
 * returns a closure that captures that state.
 *
 * Returning a null `FUeMcpPendingStep` from the factory signals a
 * validation failure that the executor should surface as `InternalError`
 * (the factory itself should prefer returning a closure whose first
 * invocation returns `Failed` with an inline-error payload, so error
 * reporting flows through the normal handler channel).
 */
using FUeMcpPendingFactory = TFunction<FUeMcpPendingStep(FUeMcpCancelToken& /*cancel*/)>;

/**
 * Runs a `TFunction<TSharedRef<FJsonObject>(FUeMcpCancelToken&)>` on the
 * game thread with a timeout, cooperative cancel, and an engine-safety
 * fence against GC / package save / async-load.
 *
 * Intended to be constructed once per subsystem and shared across all
 * request handlers. Thread-safe: `ExecuteOnGameThread` must be called
 * from a non-game thread (asserts otherwise); `Cancel`, `NumInFlight`
 * may be called from any thread.
 *
 * Clean-room reimplementation of a ticker-plus-shared-state pattern. The
 * critical correctness property: when the caller times out, the ticker
 * may still be about to write into the per-request state — so the state
 * owns its `FEvent*` and only returns it to the platform pool when the
 * last shared reference drops.
 */
class UEMCPRUNTIME_API FUeMcpGameThreadExecutor
{
public:
	FUeMcpGameThreadExecutor();
	~FUeMcpGameThreadExecutor();

	FUeMcpGameThreadExecutor(const FUeMcpGameThreadExecutor&) = delete;
	FUeMcpGameThreadExecutor& operator=(const FUeMcpGameThreadExecutor&) = delete;

	/**
	 * Submit `Work` to the game thread and block until it completes, the
	 * timeout fires, or it is cancelled.
	 *
	 * Must NOT be called from the game thread — that would deadlock with the
	 * ticker that services the queue.
	 *
	 * `TimeoutSeconds <= 0` means "use the hard sanity ceiling" (600s).
	 * Any positive value is clamped to that ceiling.
	 */
	FUeMcpExecReport ExecuteOnGameThread(
		const FGuid& RequestId,
		TFunction<TSharedRef<FJsonObject>(FUeMcpCancelToken&)> Work,
		double TimeoutSeconds);

	/**
	 * Submit a pending handler to the game thread and block until it
	 * signals `Done` / `Failed`, times out, is cancelled, or hits the
	 * engine-busy budget.
	 *
	 * Unlike `ExecuteOnGameThread`, which runs its `Work` closure once per
	 * request (on the tick that services the submission), this method
	 * invokes the step closure once per tick and yields control back to
	 * the engine between calls. Use it for tick-driven primitives where
	 * blocking the game thread between polls is wrong — property waits,
	 * frame counting, PIE time advance.
	 *
	 * The factory runs once on the first tick that services the request
	 * and returns the step closure; the closure then runs once per tick.
	 * Timeout and cancel semantics are identical to `ExecuteOnGameThread`.
	 */
	FUeMcpExecReport ExecuteOnGameThreadPending(
		const FGuid& RequestId,
		FUeMcpPendingFactory Factory,
		double TimeoutSeconds);

	/**
	 * Signal cooperative cancellation for an in-flight request.
	 *
	 * No-op if the request has already completed. The handler is expected
	 * to notice via `FUeMcpCancelToken::IsCancellationRequested()` and
	 * return promptly. If it ignores the cancel, the caller's timeout
	 * will eventually trip and the tick loop will clean up.
	 */
	void Cancel(const FGuid& RequestId);

	/** Number of currently in-flight requests. Useful for shutdown drain. */
	int32 NumInFlight() const;

private:
	struct FSharedExecState;

	TMap<FGuid, TSharedRef<FSharedExecState, ESPMode::ThreadSafe>> InFlight;
	mutable FCriticalSection InFlightLock;

	FTSTicker::FDelegateHandle TickerHandle;

	/** Ticker entry: runs every frame on the game thread. */
	bool Tick(float DeltaTime);
};
