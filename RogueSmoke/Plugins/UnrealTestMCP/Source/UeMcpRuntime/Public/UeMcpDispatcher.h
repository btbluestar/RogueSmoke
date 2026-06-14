// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HAL/CriticalSection.h"
#include "Misc/Guid.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"

#include "UeMcpGameThreadExecutor.h"

class FJsonObject;
struct FUeMcpCancelToken;

/**
 * Delegate signature for a tool handler.
 *
 * Invoked on the game thread. Returns the payload that becomes the
 * response's `data` field on success. Handlers should throw no exceptions
 * and should poll `FUeMcpCancelToken::IsCancellationRequested()` between
 * loop iterations for any long-running work.
 *
 * Error reporting: handlers signal errors by writing into the returned
 * JSON object with an `error` / `message` convention (see
 * `FUeMcpDispatcher::Dispatch` for how those are hoisted into the
 * top-level response shape), or by logging via `UE_LOG` with verbosity
 * `Error` (the request error device will capture and surface those).
 */
DECLARE_DELEGATE_RetVal_TwoParams(
	TSharedRef<FJsonObject>,
	FUeMcpToolHandler,
	const TSharedRef<FJsonObject>& /* args */,
	FUeMcpCancelToken& /* cancel */);

/**
 * Delegate signature for a pending (tick-driven) tool handler.
 *
 * Invoked once on the game thread to produce a step closure. The closure
 * is then invoked once per tick by the executor until it returns
 * `EUeMcpStep::Done` or `EUeMcpStep::Failed` (see
 * `UeMcpGameThreadExecutor.h`). Use this variant for tick-driven
 * primitives — property waits, frame counting, time advance — where the
 * handler must hand control back to the engine's frame between polls
 * rather than blocking the game thread.
 *
 * The delegate receives the arguments object once up-front. The closure
 * it returns owns the per-request polling state (captured by the lambda)
 * and reads cancellation via the `FUeMcpCancelToken&` reference passed
 * to the delegate — that reference is stable for the lifetime of the
 * request, so the closure can safely capture it by address.
 */
DECLARE_DELEGATE_RetVal_TwoParams(
	FUeMcpPendingStep,
	FUeMcpToolPendingHandler,
	const TSharedRef<FJsonObject>& /* args */,
	FUeMcpCancelToken& /* cancel */);

/**
 * Registration record for a single tool.
 *
 * Exactly one of `Handler` / `PendingHandler` must be bound. `bMutating`
 * is metadata for handler-conventions and a default-timeout hint; the
 * dispatcher does not currently gate behaviour on it, but the editor
 * subsystem's handler registration path is expected to set longer
 * timeouts for writes.
 */
struct FUeMcpToolRegistration
{
	FName ToolName;
	FUeMcpToolHandler Handler;
	FUeMcpToolPendingHandler PendingHandler;
	double DefaultTimeoutSeconds = 10.0;
	bool bMutating = false;
};

/**
 * Thin facade combining an `FUeMcpGameThreadExecutor` with a tool
 * registry and the per-request error-capture bracket. This is what
 * tool handlers are registered against.
 *
 * `Dispatch` is designed to be called from the transport (non-game)
 * thread with a parsed arguments object. It hops to the game thread via
 * the executor, runs the registered handler, and returns a fully-formed
 * response JSON object matching the wire contract in
 * `02_ARCHITECTURE.md` / `07_V0_PLAN.md`. Never throws.
 *
 * Built-in tools:
 *   - `ping` — `{"pong": true, "server_uptime_ms": <int>}`, 1s timeout.
 */
class UEMCPRUNTIME_API FUeMcpDispatcher
{
public:
	FUeMcpDispatcher();
	~FUeMcpDispatcher();

	FUeMcpDispatcher(const FUeMcpDispatcher&) = delete;
	FUeMcpDispatcher& operator=(const FUeMcpDispatcher&) = delete;

	/**
	 * Register a tool handler. Asserts (via `checkf`) on duplicate name;
	 * silent collisions were a ChiR24 bug (two `run_tests` paths both
	 * live) that we refuse to reproduce.
	 */
	void RegisterTool(const FUeMcpToolRegistration& Registration);

	/** Remove a previously-registered tool. No-op if absent. */
	void UnregisterTool(const FName& ToolName);

	/**
	 * Top-level request entry point.
	 *
	 * Looks up the tool by name, hops to the game thread with the
	 * configured per-tool timeout, runs the handler under an error-capture
	 * bracket, and returns the wire-ready response JSON.
	 *
	 * Success shape:
	 *   { "id": "<guid>", "ok": true, "data": {...}, "warnings": [...] }
	 * Failure shape:
	 *   { "id": "<guid>", "ok": false, "error": "CODE",
	 *     "message": "...", "detail": {...} }
	 */
	TSharedRef<FJsonObject> Dispatch(
		const FGuid& RequestId,
		const FName& ToolName,
		const TSharedRef<FJsonObject>& Args);

	/** Cancel an in-flight request. */
	void Cancel(const FGuid& RequestId);

	/**
	 * Mark the editor as ready to service tool calls. Called once by the
	 * editor subsystem's `WaitForEditorReady` ticker the moment `GEditor`
	 * and the editor world context are live.
	 *
	 * Before this flips, `Dispatch` short-circuits non-meta tools with a
	 * retryable `EDITOR_NOT_READY` rather than letting them hit the 2 s
	 * game-thread dispatch timeout (issue #57 mitigation #3). The bridge
	 * retries with backoff instead of degrading to per-request mode. The
	 * `ping` and `cancel` meta-paths are always allowed so the heartbeat
	 * and cancellation work during startup.
	 *
	 * Thread-safe; idempotent. Backed by `TAtomic<bool>`.
	 */
	void SetEditorReady(bool bReady) { bEditorReady.Store(bReady); }

	/** Whether `SetEditorReady(true)` has been called. */
	bool IsEditorReady() const { return bEditorReady.Load(); }

	/** Expose for tests / diagnostics. */
	int32 NumInFlight() const { return Executor.NumInFlight(); }

	/** Expose for tests / diagnostics. */
	bool HasTool(const FName& ToolName) const;

private:
	TMap<FName, FUeMcpToolRegistration> Tools;
	mutable FCriticalSection ToolsLock;

	FUeMcpGameThreadExecutor Executor;
	double StartupTime = 0.0;

	/**
	 * Gates the pre-ready `EDITOR_NOT_READY` short-circuit in `Dispatch`.
	 *
	 * Defaults TRUE so runtime-only unit tests — which construct a bare
	 * `FUeMcpDispatcher` and never have an editor subsystem to flip it —
	 * keep dispatching `ping` / custom tools exactly as before. The editor
	 * subsystem explicitly calls `SetEditorReady(false)` right after
	 * construction and `SetEditorReady(true)` once `GEditor` + the editor
	 * world context are live, so the production path opts INTO the gating.
	 */
	TAtomic<bool> bEditorReady { true };

	void RegisterBuiltInTools();
};
