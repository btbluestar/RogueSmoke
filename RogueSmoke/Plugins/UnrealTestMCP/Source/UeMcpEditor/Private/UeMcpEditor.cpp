// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpEditor.h"

#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FUeMcpEditorModule, UeMcpEditor)

void FUeMcpEditorModule::StartupModule()
{
	// Module startup is intentionally empty — UUeMcpEditorSubsystem drives the real lifecycle.
}

void FUeMcpEditorModule::ShutdownModule()
{
	// Module shutdown is intentionally empty — UUeMcpEditorSubsystem drives the real lifecycle.
}
