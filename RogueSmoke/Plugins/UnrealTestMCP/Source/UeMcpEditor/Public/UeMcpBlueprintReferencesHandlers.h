// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// `blueprint.find_variable_references` / `blueprint.find_function_references`
// — the engine-equivalent of UE's "Find References" magnifier in the BP
// editor, exposed as MCP tools.
//
// Both handlers walk the AssetRegistry for every UBlueprint in scope (or
// just the loaded subset when `scan_loaded_only=true`), iterate every
// graph (UbergraphPages / FunctionGraphs / MacroGraphs), and report each
// node that references the named symbol on the target Blueprint's class.
//
// This is a read-only diagnostic surface: it loads BPs as a side effect
// (UAsset lookups), but never mutates them. Hard caps: 5000 BPs scanned,
// 10s wall-clock, both surfaced as `truncated: true` in the response.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the two `blueprint.find_*_references` handlers on the
	 * dispatcher. Call once from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterBlueprintReferencesHandlers(FUeMcpDispatcher& Dispatcher);
}
