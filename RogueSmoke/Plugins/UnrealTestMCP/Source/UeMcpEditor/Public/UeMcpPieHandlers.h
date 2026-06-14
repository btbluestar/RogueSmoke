// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * PIE control handlers.
 *
 * Registers:
 *   - `pie.start`  — request PIE / SIE, wait on `PostPIEStarted` (and
 *                    optionally on `GameMode->BeginPlay` firing), return
 *                    when the world is up.
 *   - `pie.stop`   — request PIE shutdown, wait on `EndPIE`.
 *   - `pie.status` — cheap read of current PIE state + uptime.
 *
 * Uses `FEditorDelegates::PostPIEStarted` / `EndPIE` (both
 * `FOnPIEEvent = DECLARE_MULTICAST_DELEGATE_OneParam(const bool)` where
 * the bool is `bIsSimulating`). See `07_V0_PLAN.md §4.4` for the
 * motivation — prior-art MCPs fire-and-forget and every caller has to
 * poll; we block the response until the world is actually up.
 *
 * Subsystem hooks: `InitializePieTracking()` / `ShutdownPieTracking()`
 * install module-scoped delegate handles that capture PIE start time
 * so `pie.status` can compute `uptime_ms` without the individual
 * handlers duplicating bookkeeping.
 */
namespace UeMcp
{
	/**
	 * Register the `pie.*` tool handlers on the given dispatcher. The
	 * dispatcher must outlive any dispatched requests — typically the
	 * caller is the `UUeMcpEditorSubsystem`.
	 */
	UEMCPEDITOR_API void RegisterPieHandlers(FUeMcpDispatcher& Dispatcher);

	/**
	 * Call from `UUeMcpEditorSubsystem::Initialize`. Installs the PIE
	 * start-time delegates used by `pie.status::uptime_ms`.
	 */
	UEMCPEDITOR_API void InitializePieTracking();

	/**
	 * Call from `UUeMcpEditorSubsystem::Deinitialize`. Removes the
	 * delegates installed by `InitializePieTracking`.
	 */
	UEMCPEDITOR_API void ShutdownPieTracking();
}
