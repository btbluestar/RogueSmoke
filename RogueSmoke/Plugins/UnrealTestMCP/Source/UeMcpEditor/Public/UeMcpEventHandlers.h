// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave E / v2_pivot Gap 1b — `event.wait_for_delegate`.
//
// Binds a one-shot UFunction to a multicast delegate property on a target
// UObject by reflection. The handler returns when the delegate fires or
// when the caller-supplied timeout expires (capped by a 2s wall ceiling
// to keep the plugin socket from outliving the Python client's call
// timeout). Pairs with the existing wait/assert primitives in
// `UeMcpAssertionHandlers.cpp` to cover the patterns those primitives
// can't express — `OnDestroyed`, `OnComponentHit`, `OnActorBeginOverlap`,
// any `DECLARE_DYNAMIC_MULTICAST_*` exposed on the target.
//
// Sibling tools `event.subscribe` / `event.unsubscribe` are accepted at
// the wire level for forward-compatibility with a future durable
// subscription model (collect callbacks across many ticks, drain them
// later), but in v0 they are returned as `NOT_IMPLEMENTED` — the
// pending-handler `event.wait_for_delegate` covers every realistic test
// pattern today and a durable subscription needs a request-survival
// channel the plugin doesn't yet have.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the `event.*` handlers on the given dispatcher. Call once
	 * from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterEventHandlers(FUeMcpDispatcher& Dispatcher);
}
