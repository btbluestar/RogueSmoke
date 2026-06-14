// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Live-editor test-run handlers (M4).
 *
 * Registers:
 *   - `tests.run` — start an in-editor run of one or more tests.
 *     Fire-and-forget at the plugin level: returns a `{handle, ...}`
 *     immediately while the run proceeds on the editor's own ticking.
 *     Python server polls `tests.run_status` to stream progress
 *     notifications and calls `tests.run_report` when the run finishes.
 *   - `tests.run_status(handle)` — non-blocking snapshot of a live run:
 *     `{state, progress:{completed,total}, elapsed_ms, completed_tests}`.
 *   - `tests.run_report(handle)` — final results once the run is done:
 *     matches the subprocess-runner report shape where reasonable
 *     (`summary`, per-test `{status, duration_ms, events}`), plus a
 *     `handle` echo.
 *   - `tests.run_cancel(handle)` — signal cancellation; the next safe
 *     point in the engine's test framework observes the flag.
 *
 * All four dispatch on the game thread. The handle is a plugin-minted
 * string (prefixed `live-` so clients can distinguish it from the
 * subprocess runner's opaque handles without schema drift).
 *
 * FIRE-AND-FORGET design (see `docs/ue-api-gotchas.md §1`): handlers
 * never spin-wait. The live-run state is kept in a module-scoped
 * runner registry drained by an `FTSTicker` that polls the engine's
 * automation framework between Tick() calls. State transitions trigger
 * no delegates that our own handlers subscribe to, so re-entrance into
 * the game-thread executor cannot happen.
 */
namespace UeMcp
{
	/**
	 * Register the `tests.run` / `tests.run_status` / `tests.run_report`
	 * / `tests.run_cancel` handlers on the given dispatcher. Also
	 * installs the runner's ticker on `FTSTicker::GetCoreTicker()`. Call
	 * once during `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterTestRunHandlers(FUeMcpDispatcher& Dispatcher);

	/**
	 * Tear down the runner's ticker and clear any in-flight runs. Call
	 * once during `UUeMcpEditorSubsystem::Deinitialize`, after the
	 * server is stopped but before the dispatcher is reset.
	 */
	UEMCPEDITOR_API void ShutdownTestRunner();
}
