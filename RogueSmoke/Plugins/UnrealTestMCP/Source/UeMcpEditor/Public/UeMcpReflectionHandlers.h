// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Reflection tool handlers (M5 surface, built on top of
 * `FUeMcpPropertyPath` + `FUeMcpPropertyAccessor`).
 *
 * Registers:
 *   - `get_property` — read a typed value at a path relative to a
 *     resolved root object. The path syntax, including synthesized
 *     `@Class`/`@Components`/`@CDO` tokens and the `:enum_name` suffix,
 *     is documented in `UeMcpPropertyPath.h`. Returns the value under
 *     `data` directly — not wrapped in a `{value: ...}` envelope —
 *     because agents consume the value verbatim.
 *   - `set_property` — write a JSON-shaped value into a property.
 *     Gated on `CPF_EditConst`, not `CPF_BlueprintReadOnly` (we're
 *     editor code, not a BP script). Best-effort pre-state read means
 *     successful writes emit a `rollback` hint pointing back at
 *     `set_property` with the previous value.
 *   - `list_property_paths` — enumerate addressable paths under a root,
 *     flat. Saves exploratory round trips; see
 *     `THIRD_PARTY/notes/Incurian-AgentBridge.md §3.4` on why Incurian
 *     doesn't have this and why we add it.
 *
 * All three dispatch on the game thread via the standard executor and
 * use the shared `ResolveObject` entry point for the root argument.
 */
namespace UeMcp
{
	/**
	 * Register the reflection tool handlers on the given dispatcher.
	 * Caller is expected to be the `UUeMcpEditorSubsystem`.
	 */
	UEMCPEDITOR_API void RegisterReflectionHandlers(FUeMcpDispatcher& Dispatcher);
}
