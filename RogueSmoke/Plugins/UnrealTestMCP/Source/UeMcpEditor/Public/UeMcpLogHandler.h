// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * `log.tail` handler plus windowed log-capture tools.
 *
 * A single `FOutputDevice` subclass is attached to `GLog` at subsystem
 * initialization (NOT per-request) so that history is always available
 * when a caller asks for recent log lines. Capacity: 5000 lines. On
 * overflow the oldest entries are evicted.
 *
 * `log.capture_begin` / `log.capture_end` allow test code to bracket a
 * section of execution and assert on the set of log lines emitted within
 * that window. Multiple captures can be in flight simultaneously — each
 * is assigned a UUID and stored in a map.  See `docs/v2_pivot.md §Gap 4`.
 *
 * Thread-safety: the FOutputDevice's `Serialize` override can be called
 * from any thread, so the ring buffer and capture map are guarded by
 * critical sections with bounded (O(1)) work inside the lock — JSON
 * serialization happens only at query time, outside the lock.
 *
 * See `07_V0_PLAN.md §4.9` for the ring-buffer spec.
 */
namespace UeMcp
{
	/**
	 * Register `log.tail`, `log.capture_begin`, and `log.capture_end` on
	 * the given dispatcher. Requires `InitializeLogCapture` to have been
	 * called (otherwise the tail handler returns an empty buffer — not an
	 * error, but not useful).
	 */
	UEMCPEDITOR_API void RegisterLogHandler(FUeMcpDispatcher& Dispatcher);

	/**
	 * Call from `UUeMcpEditorSubsystem::Initialize`. Allocates the ring
	 * buffer and attaches it to `GLog`. Idempotent — a second call is a
	 * warning and a no-op.
	 */
	UEMCPEDITOR_API void InitializeLogCapture();

	/**
	 * Call from `UUeMcpEditorSubsystem::Deinitialize`. Detaches the ring
	 * buffer from `GLog` and releases it. Idempotent.
	 */
	UEMCPEDITOR_API void ShutdownLogCapture();
}
