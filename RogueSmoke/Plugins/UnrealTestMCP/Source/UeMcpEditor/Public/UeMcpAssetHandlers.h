// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Asset-graph + data-asset handlers.
//
// Five tools registered on one dispatcher entry point:
//
//   - `assets.referencers`     — what packages reference this asset?
//   - `assets.dependencies`    — what does this asset reference?
//   - `assets.find_by_class`   — list assets of a class (incl. subclasses).
//   - `data_asset.list`        — convenience: find_by_class defaulting to
//                                `/Script/Engine.DataAsset`.
//   - `data_asset.validate`    — load asset and run `IsDataValid`.
//
// All five are read-side and synchronous. Backed by `IAssetRegistry`
// (and `UObject::IsDataValid` for the validate path) — see the cpp for
// the engine-API choices.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the asset-graph and data-asset handlers on the given
	 * dispatcher. Call once from `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterAssetHandlers(FUeMcpDispatcher& Dispatcher);
}
