// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// SaveGame slot roundtrip handlers.
//
// Registers four synchronous handlers:
//
//   - `save.create` — instantiate a `USaveGame` subclass by class path,
//     reflection-write any provided properties, then persist via
//     `UGameplayStatics::SaveGameToSlot`.
//   - `save.load`   — `UGameplayStatics::LoadGameFromSlot`, then walk the
//     loaded object's properties back into JSON for the response.
//   - `save.exists` — thin wrapper over `DoesSaveGameExist`.
//   - `save.delete` — thin wrapper over `DeleteGameInSlot`.
//
// The plugin ships `UUeMcpDefaultSaveGame` (in the runtime module) so a
// generic test never needs to author a project-specific class. Tests that
// want their own schema pass `class="/Script/MyGame.MyPlayerSaveGame"`.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
    /**
     * Register `save.create` / `save.load` / `save.exists` / `save.delete`
     * on the dispatcher. Call once from
     * `UUeMcpEditorSubsystem::Initialize`.
     */
    UEMCPEDITOR_API void RegisterSaveHandlers(FUeMcpDispatcher& Dispatcher);
}
