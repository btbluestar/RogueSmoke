// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * `blueprint.graph` — per-graph node/pin/link JSON for a `UBlueprint`.
 *
 * Closes the gap that `blueprint.outline` (M5) leaves open: outline reports
 * function *names* and signatures; this reports the *bodies* (nodes +
 * pins + wires) for each function graph, ubergraph page, macro graph and
 * delegate signature graph.
 *
 * WHY a dedicated handler rather than a Python tool over existing M5
 * surface: `UEdGraph::Nodes` is `protected` on the `UEdGraph` class, so
 * Python reflection cannot reach it. C++ plugin code can — hence this
 * handler. The wire shape is deliberately compact (no positions unless
 * asked, link edges emitted from output pins only) to keep token cost
 * bounded on large graphs.
 *
 * Stage 1 (this file): read-only. Authoring (add-node / connect-pins)
 * is stage 2 and intentionally out of scope here.
 *
 * Dispatches on the game thread via the standard executor.
 */
namespace UeMcp
{
	/**
	 * Register `blueprint.graph` on the given dispatcher. Caller is the
	 * `UUeMcpEditorSubsystem`, alongside the other introspection handlers.
	 */
	UEMCPEDITOR_API void RegisterBlueprintGraphHandler(FUeMcpDispatcher& Dispatcher);
}
