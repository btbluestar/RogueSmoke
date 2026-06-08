// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Functional-test authoring handlers (M4).
 *
 * Registers:
 *   - `tests.create_level(name, template?, enforce_ftest_prefix=true)`
 *     — create a new editor level asset, enforce the `FTEST_` prefix
 *     (see `03_UE_AUTOMATION_FRAMEWORK.md §"Discovery and registration
 *     gotchas"`), default GameMode to `FunctionalTestGameMode`.
 *     Natural key: `name`; `onConflict: "skip"|"update"|"error"` with
 *     `LEVEL_EXISTS` on `error`.
 *   - `tests.spawn_functional_test(level_path, class?, name?, location?,
 *     rotation?)` — drop an `AFunctionalTest` (or subclass) into the
 *     specified level. Natural key: `name`; emits `rollback` naming
 *     `actor.destroy`.
 *   - `tests.set_functional_test_defaults(actor_path, timeout?,
 *     is_enabled?, reruns?, description?)` — mutate properties on the
 *     spawned actor. No rollback (the caller may re-invoke with
 *     previous values).
 *   - `tests.save_level(path?)` — save the specified level (or the
 *     current editor level if omitted) via the correct
 *     `UEditorLoadingAndSavingUtils` entry point. Non-reversible per
 *     `docs/handler-conventions.md §4`.
 *
 * All dispatch on the game thread. Create + save tools are mutating;
 * the save handler must NOT save during PIE (return `PIE_ACTIVE`).
 *
 * Level creation and actor spawning in specific levels will hit the
 * `dialog auto-decline` stub gap (`ue-api-gotchas.md §14`) when the
 * editor has unsaved state — document the constraint in the tool
 * descriptions so the agent knows to clean up.
 */
namespace UeMcp
{
	/**
	 * Register the `tests.create_level` / `tests.spawn_functional_test` /
	 * `tests.set_functional_test_defaults` / `tests.save_level` handlers
	 * on the given dispatcher. Call once during
	 * `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterFunctionalTestHandlers(FUeMcpDispatcher& Dispatcher);
}
