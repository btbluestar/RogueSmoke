// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Test-discovery tool handlers.
 *
 * Registers:
 *   - `tests.list` — non-mutating enumeration of tests known to the
 *     Automation Controller. Optional case-insensitive substring filter
 *     on the full dotted test path; optional flag-category filter via a
 *     bool-per-flag object; limit + truncation fields on the response.
 *   - `tests.refresh` — explicit re-enumeration. Wraps
 *     `IAutomationControllerManager::RequestTests`, then waits for the
 *     controller to settle (bounded at 10s).
 *
 * Both tools dispatch on the game thread via the standard executor;
 * the Automation Controller state machine is game-thread-only.
 */
namespace UeMcp
{
	/**
	 * Register the `tests.*` tool handlers on the given dispatcher. The
	 * dispatcher must outlive any dispatched requests — typically the
	 * caller is the `UUeMcpEditorSubsystem` whose lifetime matches the
	 * editor.
	 */
	UEMCPEDITOR_API void RegisterTestHandlers(FUeMcpDispatcher& Dispatcher);
}
