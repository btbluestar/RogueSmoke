// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Stage 2 of the Blueprint graph surface — AUTHORING. The read side is
// `UeMcpBlueprintGraphHandler` (stage 1). This header declares the
// registrar for six mutating tools:
//
//   - `blueprint.graph.add_node`        insert a K2Node into a named graph
//   - `blueprint.graph.connect_pins`    wire one output pin to one input pin
//   - `blueprint.graph.disconnect_pins` inverse; natural rollback for connect
//   - `blueprint.graph.set_pin_default` set a pin's literal default
//   - `blueprint.graph.remove_node`     delete a node (and its links)
//   - `blueprint.graph.compile`         FKismetEditorUtilities::CompileBlueprint,
//                                       with optional save-on-compile
//
// Each mutator marks the BP package dirty and emits `rollback` metadata
// per `docs/handler-conventions.md §4`. Compile is explicitly a separate
// step — callers chain N authoring edits and then compile once. Live
// edits before compile are not reflected in PIE / gameplay.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the six `blueprint.graph.*` authoring tools on the given
	 * dispatcher. Call once during `UUeMcpEditorSubsystem::Initialize`,
	 * alongside `RegisterBlueprintGraphHandler` (the stage-1 reader).
	 */
	UEMCPEDITOR_API void RegisterBlueprintAuthoringHandler(FUeMcpDispatcher& Dispatcher);
}
