// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Level-authoring tool handlers (M3 surface).
 *
 * Currently registers:
 *   - `level.current` — non-mutating probe of the current editor world.
 *     Returns the path, package name, world name, temp-world flag, PIE
 *     flag, and resolved scope. Honours the `world` arg.
 *   - `level.load` — mutating. Loads a map by `/Game/...` path using
 *     `UEditorLoadingAndSavingUtils::LoadMap`. Refuses during PIE.
 *     Pre-checks the current editor world's dirty state and returns
 *     `LEVEL_DIRTY` unless `save_current_dirty` is true. The
 *     dialog-auto-decline hook
 *     (`UeMcpEditorSubsystem::StartDialogAutoDecline`) now suppresses
 *     the FMessageDialog "Save changes?" modal so the load can't
 *     wedge; the `LEVEL_DIRTY` gate is kept so discarding unsaved
 *     edits stays an explicit caller choice.
 *   - `level.save` — mutating. Saves current world in place, or to a
 *     caller-supplied `/Game/...` path when `path` is given. Refuses
 *     during PIE. Never reversible (on-disk state is owned by source
 *     control — see `docs/handler-conventions.md` §4).
 *
 * All three dispatch on the game thread via the standard executor.
 */
namespace UeMcp
{
	/**
	 * Register the `level.*` tool handlers on the given dispatcher. The
	 * dispatcher must outlive any dispatched requests — typically the
	 * caller is the `UUeMcpEditorSubsystem` whose lifetime matches the
	 * editor.
	 */
	UEMCPEDITOR_API void RegisterLevelHandlers(FUeMcpDispatcher& Dispatcher);
}
