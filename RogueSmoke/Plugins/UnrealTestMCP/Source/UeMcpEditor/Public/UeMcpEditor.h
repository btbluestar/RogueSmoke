// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * Editor module for unreal-test-mcp.
 *
 * Thin shell — the real lifecycle lives on UUeMcpEditorSubsystem, which owns
 * the transport listener, game-thread dispatcher, and handler registry.
 */
class FUeMcpEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
