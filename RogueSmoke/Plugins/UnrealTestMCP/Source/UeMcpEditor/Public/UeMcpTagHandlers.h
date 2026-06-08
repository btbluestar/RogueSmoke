// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Fan-out unit 7 — gameplay tag operations.
//
// Registers the four tag-surface tools that gameplay tests need to ask
// or change tag state on a live actor:
//
//   - `tags.has`    — exact tag membership check.
//   - `tags.query`  — exact OR parent-match (e.g. "State.Combat" on an
//                     actor that owns "State.Combat.Attacking").
//   - `tags.add`    — append a loose gameplay tag (GAS only).
//   - `tags.remove` — remove a loose gameplay tag (GAS only).
//
// Resolution: target actors via the existing ObjectResolver. Read-side
// works on anything implementing `IGameplayTagAssetInterface` OR carrying
// a `UAbilitySystemComponent` (looked up by class name at runtime so the
// plugin doesn't need to link `GameplayAbilities`). Mutation requires
// GAS — the interface itself is read-only by design.
//
// All four handlers are synchronous: a single game-thread read or write,
// no tick-driven primitives. The pending-handler variant is reserved for
// tools that genuinely need to wait across ticks (`state.wait_until`,
// `pie.advance_*`).

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the four `tags.*` handlers on the given dispatcher. Call
	 * once from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterTagHandlers(FUeMcpDispatcher& Dispatcher);
}
