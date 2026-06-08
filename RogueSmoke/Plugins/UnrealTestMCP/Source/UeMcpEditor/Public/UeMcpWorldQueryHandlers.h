// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// World-spatial-query handlers (fan-out agent 6).
//
// Registers two synchronous read-only physics-scene queries:
//
//   - `world.line_trace`   — `World->LineTraceSingleByChannel` against a
//     start/end pair on a caller-named collision channel. Returns the
//     impact point/normal/distance, the hit actor + component path, the
//     bone name (when the hit lives on a SkeletalMeshComponent), and the
//     physics material soft-path (when one is present).
//
//   - `world.sphere_overlap` — `World->OverlapMultiByChannel` with
//     `FCollisionShape::MakeSphere(radius)` at a caller-named centre.
//     Returns a deduplicated list of hit actor labels + the count of
//     individual component overlaps.
//
// Both honour the standard `world` arg (auto/pie/editor) via
// `UeMcp::ResolveWorldFromArgs`. Optional `ignore_actors` is a list of
// actor labels/paths that the resolver runs through `ResolveObject`,
// skipping any unresolvable entries with a warning rather than failing
// the whole call (test authors routinely pass labels that haven't spawned
// yet — refusing the trace would be unfriendly).
//
// Synchronous handler shape (the executor's per-tool timeout is the only
// safety net): both queries are O(visible-physics-bodies) on the game
// thread; for an arena-sized scene this is microseconds.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register `world.line_trace` and `world.sphere_overlap` on the given
	 * dispatcher. Call once from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterWorldQueryHandlers(FUeMcpDispatcher& Dispatcher);
}
