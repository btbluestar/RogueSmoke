// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Subsystem reflection — `subsystem.list` + `subsystem.invoke`.
//
// Tests routinely need to drive engine/game/world/editor subsystems
// (`UAssetRegistry`, `UEditorActorSubsystem`, custom game subsystems
// authored by the project under test). Surfacing the subsystem method
// table to the agent without bespoke per-subsystem handlers lets a
// test-author call any UFUNCTION on any loaded subsystem with a single
// generic invoke + a discoverability list.
//
// Both handlers run on the game thread (subsystem state is game-thread
// only; `ProcessEvent` must be called from the game thread).

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register `subsystem.list` and `subsystem.invoke` on the given
	 * dispatcher. Call once from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterSubsystemHandlers(FUeMcpDispatcher& Dispatcher);
}
