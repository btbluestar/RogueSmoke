// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * Runtime module for unreal-test-mcp.
 *
 * Intentionally minimal today. Future home of the transport listener,
 * game-thread dispatcher, and reflection primitives — all kept editor-free
 * so this module stays cook-safe for a future runtime test harness.
 */
class FUeMcpRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
