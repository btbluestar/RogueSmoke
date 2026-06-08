// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave fan-out (agent 8) — perf observability handlers.
//
// Read-only snapshots that surface engine-side performance + memory
// counters so a Claude-authored test can record before/after deltas
// without spinning up the heavy stat-system / Insights pipeline.
//
//   - `perf.frame_stats` — running averages from the rendering /
//     render-thread / RHI globals: GAverageFPS, GAverageMS, plus the
//     last-measured GameThreadTime, RenderThreadTime, and GPU frame
//     time. Cheap; one game-thread hop, no allocation beyond the
//     response object.
//
//   - `perf.memory_snapshot` — `FPlatformMemory::GetStats()` mapped to
//     JSON. UsedPhysical / PeakUsedPhysical / AvailablePhysical /
//     UsedVirtual / PeakUsedVirtual / AvailableVirtual /
//     TotalPhysical / TotalVirtual all surface as bytes (uint64).
//
// Both handlers are synchronous — no tick-driven polling, no
// pending-handler dance.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register `perf.frame_stats` + `perf.memory_snapshot` on the given
	 * dispatcher. Call once from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterPerfHandlers(FUeMcpDispatcher& Dispatcher);
}
