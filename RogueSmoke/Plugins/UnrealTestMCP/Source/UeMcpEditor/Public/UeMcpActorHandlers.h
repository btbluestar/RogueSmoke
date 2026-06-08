// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Actor-authoring tool handlers (M3 surface).
 *
 * Currently registers:
 *   - `actor.list` — non-mutating enumeration of actors in the resolved
 *     world via `TActorIterator<AActor>`. Optional class/name filters
 *     (case-insensitive), `limit`/`truncated` truncation fields. Returns
 *     compact entries with location/rotation/scale as 3-element float
 *     arrays (NOT objects) — cheaper for Claude to reason about than
 *     `{x,y,z}` sub-objects, and matches tests.list's emission style.
 *   - `actor.spawn` — mutating. Spawns one actor of the given class at
 *     the given transform. Class resolution is permissive: exact full
 *     path → `StaticLoadClass` → short-name scan. Blueprint asset
 *     paths are transparently unwrapped via `BP->GeneratedClass`.
 *     Emits a `rollback` hint pointing at `actor.destroy` with the
 *     spawned actor's path.
 *   - `actor.destroy` — mutating. Takes exactly one of `actor_path` /
 *     `actor_name`. Short names match label first, then `GetName`, then
 *     the short tail of the path.
 *
 * Component enumeration is deliberately NOT part of `actor.list`;
 * components live under the reflection surface (M5).
 *
 * All three dispatch on the game thread via the standard executor.
 */
namespace UeMcp
{
	/**
	 * Register the `actor.*` tool handlers on the given dispatcher. The
	 * dispatcher must outlive any dispatched requests — typically the
	 * caller is the `UUeMcpEditorSubsystem` whose lifetime matches the
	 * editor.
	 */
	UEMCPEDITOR_API void RegisterActorHandlers(FUeMcpDispatcher& Dispatcher);
}
