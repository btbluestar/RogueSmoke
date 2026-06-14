// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave E — test-authoring primitives (docs/v2_pivot.md §Gap 1 + §Gap 2).
//
// Registers the wait/assert primitives that make writing QA tests cheap:
//
//   - `state.wait_until` — poll a property on the game thread against a
//     predicate (`eq | neq | lt | le | gt | ge | truthy | falsy`) with a
//     bounded timeout. Replaces the 20-50 lines of Python poll+sleep
//     boilerplate every test would otherwise re-author.
//
//   - `test.assert_property` — read a property via the shared
//     `FUeMcpPropertyAccessor::GetValue` path, compare with the same
//     operator set, and return a structured pass/fail record. When the
//     caller names an `AFunctionalTest` actor via the `functional_test`
//     argument, pass/fail is also routed into the engine's Automation
//     Framework via `AddInfo` / `AddError` so it surfaces in
//     `tests.run_report` automatically.
//
// Both handlers dispatch on the game thread and reuse the reflection
// core + ObjectResolver that the rest of the plugin already relies on —
// zero new grammar, zero new accessors.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the `state.wait_until` and `test.assert_property` handlers
	 * on the given dispatcher. Call once from
	 * `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterAssertionHandlers(FUeMcpDispatcher& Dispatcher);
}
