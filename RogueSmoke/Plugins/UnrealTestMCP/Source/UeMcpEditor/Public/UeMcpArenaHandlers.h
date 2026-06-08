// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Test-arena authoring tool handlers (v2 pivot, F4).
 *
 * Currently registers:
 *   - `plugin.rebuild_test_arena` — mutating. Procedurally builds a
 *     lit, playable arena level at `/Game/Plugins/UnrealTestMCP/
 *     TestFixtures/Maps/FTEST_Arena` (override with `arena_path`).
 *     Creates a ground plane, directional light, skylight, sky
 *     sphere, post-process volume, 3-5 deterministic cover boxes
 *     (seeded), NavMeshBoundsVolume covering the ground, sets the
 *     World Settings default game mode to FunctionalTestGameMode,
 *     saves the map, and kicks off a navmesh build. Lighting is
 *     left as preview (full builds are too slow for the test loop).
 *
 * Deterministic given the same `seed` — cover box positions are the
 * only random element, and the RNG is re-seeded every call.
 */
namespace UeMcp
{
	/**
	 * Register the arena-authoring tool handlers on the given
	 * dispatcher. Dispatcher must outlive any dispatched requests —
	 * in practice the caller is `UUeMcpEditorSubsystem` whose
	 * lifetime matches the editor.
	 */
	UEMCPEDITOR_API void RegisterArenaHandlers(FUeMcpDispatcher& Dispatcher);
}
