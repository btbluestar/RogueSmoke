// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * `actor.call_method` / `component.call_method` — first-class UFUNCTION
 * invocation by **live** UClass reflection (issue #30).
 *
 * Why this exists: Python's `unreal` plugin auto-generates snake_case
 * attribute bindings for every UFUNCTION on a class. Those bindings cache
 * a function pointer from the FIRST class load. After an AngelScript
 * hot-reload the live `UClass` reflects the new bytecode, but the cached
 * Python binding still dispatches to the STALE compiled body — so
 * `obj.can_mount(player)` returns silently-wrong answers while
 * `obj.call_method("CanMount", args=...)` (live reflection) is correct.
 * Agents that `python_exec` a snake_case call from muscle memory burn
 * sessions chasing a phantom logic bug.
 *
 * These handlers ALWAYS go through `UClass::FindFunctionByName` +
 * `UObject::ProcessEvent` on a freshly-resolved object — there is no
 * cached binding anywhere in the path, so a hot-reloaded AS class is
 * dispatched correctly the moment its `UClass` updates.
 *
 * Registers:
 *   - `actor.call_method` — resolve `object` to a UObject via the shared
 *     five-strategy resolver (`UeMcp::ResolveObject`), find the named
 *     UFUNCTION on its class, marshal `args` (JSON object keyed by
 *     parameter name) into a parameter buffer, `ProcessEvent`, then
 *     marshal the return value + any out-params back to JSON.
 *   - `component.call_method` — same, but after resolving the actor it
 *     drills into the named `component` (by `UActorComponent::GetName`,
 *     case-insensitive with prefix fallback — the same matching rule the
 *     property accessor's component fallback uses) and invokes the
 *     UFUNCTION on the component instance.
 *
 * Marshalling is the engine's `FJsonObjectConverter` (one FProperty +
 * raw parameter-slot memory), identical to `subsystem.invoke` — the
 * existing reflection-invoke path the brief points at. The property
 * accessor (`FUeMcpPropertyAccessor`) is path-walked-UObject-shaped and
 * does not fit a stack-allocated parameter buffer, so we reuse the
 * converter path rather than the accessor's value codec.
 *
 * Dispatched on the game thread via the standard executor — every
 * UObject touch (resolution, component walk, ProcessEvent) is on-thread.
 */
namespace UeMcp
{
	/**
	 * Register `actor.call_method` + `component.call_method` on the
	 * dispatcher. Caller is expected to be the `UUeMcpEditorSubsystem`.
	 */
	UEMCPEDITOR_API void RegisterCallMethodHandlers(FUeMcpDispatcher& Dispatcher);
}
