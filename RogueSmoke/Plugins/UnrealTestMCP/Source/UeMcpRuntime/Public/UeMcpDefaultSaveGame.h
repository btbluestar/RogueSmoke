// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Default `USaveGame` subclass shipped with the plugin so callers can
// roundtrip saved data without authoring a project-specific class.
//
// `USaveGame` is `abstract`, so `NewObject<USaveGame>(...)` returns null
// at runtime — every SaveGame consumer needs a concrete subclass. This
// type provides three generic property buckets (`StringValues`,
// `FloatValues`, `IntValues`) that the JSON property accessor can write
// into via dotted paths like `StringValues["player_name"]`. Picked the
// smallest API that lets the smoke prove a true roundtrip.
//
// Lives in the Runtime module so the asset path
// `/Script/UeMcpRuntime.UeMcpDefaultSaveGame` resolves at editor startup
// without an editor-only module pulling. Keep additions minimal — the
// "named buckets of POD" pattern is exactly what you want from a generic
// test save class; richer schemas belong on the project's own SaveGame.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "UeMcpDefaultSaveGame.generated.h"

UCLASS(Blueprintable, BlueprintType,
       DisplayName="UeMcp Default Save Game")
class UEMCPRUNTIME_API UUeMcpDefaultSaveGame : public USaveGame
{
    GENERATED_BODY()

public:
    /** Generic string-keyed string bucket. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|SaveGame")
    TMap<FString, FString> StringValues;

    /** Generic string-keyed float bucket. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|SaveGame")
    TMap<FString, float> FloatValues;

    /** Generic string-keyed int32 bucket. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|SaveGame")
    TMap<FString, int32> IntValues;

    /** Convenience scalar fields for tests that don't need maps. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|SaveGame")
    FString PlayerName;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|SaveGame")
    int32 Level = 0;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="UeMcp|SaveGame")
    float Score = 0.0f;
};
