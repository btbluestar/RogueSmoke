// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave F / Agent-11 — AI primitives.
//
// Three synchronous handlers that let a Claude-authored test drive the
// AAIController/UBlackboardComponent/UBehaviorTree triangle without a
// detour through python_exec:
//
//   - `ai.set_blackboard` — write a typed value to a blackboard key by
//     name. Accepted types match the blackboard's native primitives:
//     bool, int, float, string, name, vector, rotator, object.
//
//   - `ai.get_blackboard` — read the same set of types back as JSON.
//     Vector / Rotator are emitted as 3-element arrays; Object emits its
//     full path. Unset keys come back as `null`.
//
//   - `ai.start_bt` — load a `UBehaviorTree` asset by `/Game/...` path
//     and call `AAIController::RunBehaviorTree(BT)`. Replaces any
//     currently-running tree on the controller (engine semantics).
//
// Issue #15 — blackboard authoring (no python_exec):
//
//   - `ai.add_blackboard_key` — load (or create) a `UBlackboardData`
//     asset and append a typed key (float/bool/vector/object/class/
//     int/string/name/rotator) via `UpdatePersistentKey<T>`, then save.
//
//   - `ai.init_blackboard` — load (or create) a `UBlackboardData`
//     asset and call `AAIController::UseBlackboard(asset, comp)` so the
//     controller gets a live `UBlackboardComponent` WITHOUT a behavior
//     tree. After this, `ai.get_blackboard` / `ai.set_blackboard`
//     operate on a real, initialised blackboard.
//
// All resolve the target as either a `Pawn` (then GetController)
// or directly as an `AAIController`. Game-thread synchronous; cheap.
//
// Coexists with the existing assertion / pie / arena handler families —
// each registers from the editor subsystem alongside its siblings.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register `ai.set_blackboard`, `ai.get_blackboard`, `ai.start_bt`,
	 * `ai.add_blackboard_key`, and `ai.init_blackboard` on the given
	 * dispatcher. Call once from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterAIHandlers(FUeMcpDispatcher& Dispatcher);
}
