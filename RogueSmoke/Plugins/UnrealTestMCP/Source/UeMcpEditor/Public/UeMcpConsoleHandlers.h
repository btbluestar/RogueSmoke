// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Console exec — `console.exec` (12-way fan-out, agent 3 / area 3).
//
// Wraps `GEngine->Exec(World, Cmd, Ar)` so a Claude-authored test can
// drive a console command into either the editor world or the live PIE
// world and receive whatever the engine wrote into the FOutputDevice
// (`Ar`) back as a JSON `output` array.
//
// One handler today (`console.exec`); the area exists so additional
// console-side tools (cvar inspection, exec-on-PC, history) can land in
// the same module later without reshuffling registration.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the `console.exec` tool on the given dispatcher. Call once
	 * from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterConsoleHandlers(FUeMcpDispatcher& Dispatcher);
}
