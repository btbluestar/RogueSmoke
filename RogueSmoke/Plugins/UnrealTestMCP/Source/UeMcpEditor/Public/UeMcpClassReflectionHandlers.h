// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Class-reflection tool handler (closes the `python_exec` gap tracked in
 * issue #35). The reflection core (`FUeMcpPropertyAccessor` &c.) answers
 * "what is the *value* at this path on this *instance*"; this handler
 * answers the orthogonal "what is this *class*" question — does a
 * `/Script/Module.ClassName` path resolve, what is its parent chain up to
 * `UObject`, and is it a subclass of some supplied base.
 *
 * Registers:
 *   - `class.reflect` — accept one or more class paths and an optional
 *     `is_a` base-class path; return, per class: whether it resolved, the
 *     resolved class path, the parent chain (immediate-super first, up to
 *     and including `/Script/CoreUObject.Object`), a handful of cheap
 *     reflection flags (`is_native`, `is_abstract`, `is_blueprintable`),
 *     and — when `is_a` is supplied — the `IsChildOf` result. This is the
 *     native replacement for the inline `unreal.load_class(...)` +
 *     `isinstance(...)` blocks in `fixture_base_smoke.py` /
 *     `ai_smoke.py`. Pure reflection — no asset load beyond whatever
 *     `StaticLoadClass` triggers to discover a Blueprint generated class.
 *
 * Dispatches on the game thread via the standard executor (UClass /
 * `UObject` iteration must not race the GC / loader).
 */
namespace UeMcp
{
	/**
	 * Register the class-reflection tool handler on the given dispatcher.
	 * Caller is expected to be the `UUeMcpEditorSubsystem`.
	 */
	UEMCPEDITOR_API void RegisterClassReflectionHandlers(FUeMcpDispatcher& Dispatcher);
}
