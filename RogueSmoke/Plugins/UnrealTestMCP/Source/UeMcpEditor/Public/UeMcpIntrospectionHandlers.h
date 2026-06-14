// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Thin introspection wrappers sitting on top of the reflection core
 * (M5 surface). Each is a tiny handler that resolves an object, calls
 * into `FUeMcpPropertyAccessor`, and shapes the result.
 *
 * Registers:
 *   - `actor.summary` — class + component list + overridden-property
 *     count (diff against CDO). Cheap; ~10 lines of JSON per actor.
 *   - `actor.properties` — bulk-read specific paths OR a top-level-only
 *     snapshot of all UPROPERTYs on the actor. Does NOT recurse into
 *     nested structs by default (that's what `list_property_paths` is
 *     for).
 *   - `blueprint.outline` — static introspection of a `UBlueprint`
 *     asset: functions, variables, component tree, events. No node
 *     graphs (that's v1+). Traversal uses `UBlueprint::FunctionGraphs`,
 *     `NewVariables`, `SimpleConstructionScript`, and
 *     `UbergraphPages` walking `UK2Node_Event`.
 *   - `cdo.defaults` — resolve a class, get its CDO, bulk-read paths.
 *     Thin wrapper over the accessor's `@CDO`-rooted walk.
 *
 * All four dispatch on the game thread via the standard executor.
 */
namespace UeMcp
{
	/**
	 * Register the introspection tool handlers on the given dispatcher.
	 * Caller is expected to be the `UUeMcpEditorSubsystem`.
	 */
	UEMCPEDITOR_API void RegisterIntrospectionHandlers(FUeMcpDispatcher& Dispatcher);
}
