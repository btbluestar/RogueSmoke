// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Project-health tool handlers.
 *
 * Currently registers:
 *   - `project.status` — cheap, non-mutating snapshot of engine / project /
 *     PIE / busy state. Used as the session's "am I plugged into what I
 *     think I'm plugged into?" probe.
 */
namespace UeMcp
{
	/**
	 * Register the `project.*` tool handlers on the given dispatcher. The
	 * dispatcher must outlive any dispatched requests — typically the
	 * caller is the `UUeMcpEditorSubsystem` whose lifetime matches the
	 * editor.
	 */
	UEMCPEDITOR_API void RegisterProjectHandlers(FUeMcpDispatcher& Dispatcher);
}
