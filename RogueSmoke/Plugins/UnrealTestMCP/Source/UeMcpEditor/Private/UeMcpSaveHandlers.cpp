// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Save-game slot roundtrip — see header for the four-tool surface.
//
// Threading: every handler hops to the game thread via the dispatcher and
// calls `UGameplayStatics` synchronously. The Save/Load APIs do their own
// platform-IO under the hood; on disk-backed platforms they're "fast
// enough" for editor latency budgets and there's no native sync hook for
// the async variants we'd need to expose. Callers that need real
// fire-and-forget should use the engine's async path directly via
// `python_exec` for now.
//
// Class resolution is intentionally `StaticLoadClass` not the soft-object
// resolver — SaveGame classes are typically `/Script/...` paths that
// don't live in the asset registry, and the soft-path resolver would
// fail on those. We accept either a full class path or a Blueprint asset
// path (with or without the `_C` suffix).
//
// Property roundtrip uses `FUeMcpPropertyAccessor::SetValue` /
// `ListPropertyPaths` + `GetValue` so the wire format matches every
// other reflection-shaped tool exactly. No bespoke marshalling.

#include "UeMcpSaveHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/SaveGame.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpPropertyAccessor.h"
#include "UeMcpPropertyValue.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpSaveHandlersPrivate
{
    /** Read+write are a single sync game-thread call each — short timeout. */
    static constexpr double DefaultTimeoutSeconds = 10.0;

    /** Default save class shipped with the plugin. */
    static const TCHAR* DefaultSaveClassPath =
        TEXT("/Script/UeMcpRuntime.UeMcpDefaultSaveGame");

    /** Default user index when the caller omits one. */
    static constexpr int32 DefaultUserIndex = 0;

    /**
     * Build the inline error from an accessor failure (with optional path).
     * Accessor-error -> wire-code goes through the shared
     * `UeMcp::AccessorErrorToCode` (issue #62); no local switch here.
     */
    static TSharedRef<FJsonObject> BuildErrorFromAccessor(
        const FUeMcpAccessorErrorInfo& Info, const FString& Path)
    {
        TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
        Out->SetStringField(TEXT("error"), UeMcp::AccessorErrorToCode(Info.Code));
        Out->SetStringField(TEXT("message"), Info.Message);
        TSharedRef<FJsonObject> Detail = Info.Detail.IsValid()
            ? Info.Detail.ToSharedRef()
            : MakeShared<FJsonObject>();
        if (!Path.IsEmpty())
        {
            Detail->SetStringField(TEXT("property_path"), Path);
        }
        if (Detail->Values.Num() > 0)
        {
            Out->SetObjectField(TEXT("detail"), Detail);
        }
        return Out;
    }

    /**
     * Pull the optional `slot` string. Required, non-empty.
     * Wire field: `slot`.
     */
    static bool ParseSlot(
        const TSharedRef<FJsonObject>& Args, FString& OutSlot, FString& OutError)
    {
        if (!Args->TryGetStringField(TEXT("slot"), OutSlot) || OutSlot.IsEmpty())
        {
            OutError = TEXT("`slot` is required and must be a non-empty string");
            return false;
        }
        return true;
    }

    /** Pull optional `user_index`; defaults to 0. */
    static int32 ParseUserIndex(const TSharedRef<FJsonObject>& Args)
    {
        double Raw = static_cast<double>(DefaultUserIndex);
        Args->TryGetNumberField(TEXT("user_index"), Raw);
        return static_cast<int32>(Raw);
    }

    /**
     * Resolve a SaveGame class by path. Falls back to the bundled default
     * if `class` is missing/empty. Accepts paths with or without the `_C`
     * suffix.
     */
    static UClass* ResolveSaveGameClass(
        const TSharedRef<FJsonObject>& Args, FString& OutPath, FString& OutError)
    {
        FString ClassPath;
        Args->TryGetStringField(TEXT("class"), ClassPath);
        if (ClassPath.IsEmpty())
        {
            ClassPath = DefaultSaveClassPath;
        }
        OutPath = ClassPath;

        // Try as-given first.
        UClass* Cls = StaticLoadClass(USaveGame::StaticClass(), nullptr, *ClassPath);
        if (Cls == nullptr && !ClassPath.EndsWith(TEXT("_C")))
        {
            // Common case: caller passed a Blueprint asset path; retry with `_C`.
            const FString WithC = ClassPath + TEXT("_C");
            Cls = StaticLoadClass(USaveGame::StaticClass(), nullptr, *WithC);
            if (Cls != nullptr)
            {
                OutPath = WithC;
            }
        }

        if (Cls == nullptr)
        {
            OutError = FString::Printf(
                TEXT("SaveGame class not found at path '%s'"), *ClassPath);
            return nullptr;
        }
        if (!Cls->IsChildOf(USaveGame::StaticClass()))
        {
            OutError = FString::Printf(
                TEXT("Class '%s' is not a USaveGame subclass"), *ClassPath);
            return nullptr;
        }
        if (Cls->HasAnyClassFlags(CLASS_Abstract))
        {
            OutError = FString::Printf(
                TEXT("Class '%s' is abstract; pass a concrete USaveGame subclass"),
                *ClassPath);
            return nullptr;
        }
        return Cls;
    }

    /**
     * Apply caller-supplied properties to a freshly-instantiated SaveGame.
     * Returns true on success; on first failure populates `OutError` with a
     * fully-formed inline-error object and returns false.
     */
    static bool ApplyProperties(
        USaveGame* Save,
        const TSharedRef<FJsonObject>& Args,
        TSharedPtr<FJsonObject>& OutError)
    {
        const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
        if (!Args->TryGetObjectField(TEXT("properties"), PropsPtr)
            || PropsPtr == nullptr || !PropsPtr->IsValid())
        {
            // Properties are optional — empty caller dict is just "save defaults".
            return true;
        }

        for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*PropsPtr)->Values)
        {
            FUeMcpAccessorErrorInfo Err;
            if (!FUeMcpPropertyAccessor::SetValue(Save, Pair.Key, Pair.Value, Err))
            {
                OutError = BuildErrorFromAccessor(Err, Pair.Key);
                return false;
            }
        }
        return true;
    }

    /**
     * Walk a loaded SaveGame's UPROPERTY tree and serialise every leaf into a
     * `{path: value}` object so the response can show what was persisted.
     * Uses `ListPropertyPaths` + `GetValue` so the wire format is identical
     * to every other reflection-based tool.
     */
    static TSharedRef<FJsonObject> SerializeProperties(USaveGame* Save)
    {
        TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
        if (Save == nullptr)
        {
            return Out;
        }

        // Don't surface the synthesized `@Class` etc. fields in the loaded
        // bag — they're noise for a roundtrip caller.
        FUeMcpListPathsOptions Opts;
        Opts.MaxDepth = 4;
        Opts.bIncludeSynthesized = false;

        TArray<FString> Paths;
        FUeMcpAccessorErrorInfo Err;
        if (!FUeMcpPropertyAccessor::ListPropertyPaths(Save, Paths, Err, Opts))
        {
            return Out;
        }

        for (const FString& Path : Paths)
        {
            FUeMcpPropertyValue Value;
            FUeMcpAccessorErrorInfo ReadErr;
            if (FUeMcpPropertyAccessor::GetValue(Save, Path, Value, ReadErr))
            {
                if (Value.Json.IsValid())
                {
                    Out->SetField(Path, Value.Json);
                }
                else
                {
                    Out->SetField(Path, MakeShared<FJsonValueNull>());
                }
            }
        }
        return Out;
    }

    // -----------------------------------------------------------------
    // save.create
    // -----------------------------------------------------------------

    static TSharedRef<FJsonObject> HandleSaveCreate(
        const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
    {
        check(IsInGameThread());

        FString Slot;
        FString ParseErr;
        if (!ParseSlot(Args, Slot, ParseErr))
        {
            return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
        }
        const int32 UserIndex = ParseUserIndex(Args);

        FString ClassPath;
        FString ClassErr;
        UClass* Cls = ResolveSaveGameClass(Args, ClassPath, ClassErr);
        if (Cls == nullptr)
        {
            return UeMcp::MakeInlineError(TEXT("NOT_FOUND"), ClassErr);
        }

        USaveGame* Save = NewObject<USaveGame>(GetTransientPackage(), Cls);
        if (Save == nullptr)
        {
            return UeMcp::MakeInlineError(
                TEXT("SAVE_GAME_FAILED"),
                FString::Printf(
                    TEXT("NewObject<USaveGame>(%s) returned null"), *ClassPath));
        }

        TSharedPtr<FJsonObject> PropErr;
        if (!ApplyProperties(Save, Args, PropErr))
        {
            return PropErr.ToSharedRef();
        }

        const bool bSaved = UGameplayStatics::SaveGameToSlot(Save, Slot, UserIndex);
        if (!bSaved)
        {
            return UeMcp::MakeInlineError(
                TEXT("SAVE_GAME_FAILED"),
                FString::Printf(
                    TEXT("SaveGameToSlot returned false (slot='%s', user_index=%d)"),
                    *Slot, UserIndex));
        }

        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("saved"), true);
        Data->SetStringField(TEXT("slot"), Slot);
        Data->SetNumberField(TEXT("user_index"), UserIndex);
        Data->SetStringField(TEXT("class"), ClassPath);
        Data->SetObjectField(TEXT("properties"), SerializeProperties(Save));
        return Data;
    }

    // -----------------------------------------------------------------
    // save.load
    // -----------------------------------------------------------------

    static TSharedRef<FJsonObject> HandleSaveLoad(
        const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
    {
        check(IsInGameThread());

        FString Slot;
        FString ParseErr;
        if (!ParseSlot(Args, Slot, ParseErr))
        {
            return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
        }
        const int32 UserIndex = ParseUserIndex(Args);

        if (!UGameplayStatics::DoesSaveGameExist(Slot, UserIndex))
        {
            return UeMcp::MakeInlineError(
                TEXT("NOT_FOUND"),
                FString::Printf(
                    TEXT("No save exists at slot='%s' user_index=%d"),
                    *Slot, UserIndex));
        }

        USaveGame* Save = UGameplayStatics::LoadGameFromSlot(Slot, UserIndex);
        if (Save == nullptr)
        {
            return UeMcp::MakeInlineError(
                TEXT("SAVE_LOAD_FAILED"),
                FString::Printf(
                    TEXT("LoadGameFromSlot returned null for slot='%s' user_index=%d"),
                    *Slot, UserIndex));
        }

        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("loaded"), true);
        Data->SetStringField(TEXT("slot"), Slot);
        Data->SetNumberField(TEXT("user_index"), UserIndex);
        Data->SetStringField(TEXT("class"),
            Save->GetClass() != nullptr
                ? Save->GetClass()->GetPathName()
                : FString());
        Data->SetObjectField(TEXT("properties"), SerializeProperties(Save));
        return Data;
    }

    // -----------------------------------------------------------------
    // save.exists
    // -----------------------------------------------------------------

    static TSharedRef<FJsonObject> HandleSaveExists(
        const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
    {
        check(IsInGameThread());

        FString Slot;
        FString ParseErr;
        if (!ParseSlot(Args, Slot, ParseErr))
        {
            return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
        }
        const int32 UserIndex = ParseUserIndex(Args);

        const bool bExists = UGameplayStatics::DoesSaveGameExist(Slot, UserIndex);

        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("exists"), bExists);
        Data->SetStringField(TEXT("slot"), Slot);
        Data->SetNumberField(TEXT("user_index"), UserIndex);
        return Data;
    }

    // -----------------------------------------------------------------
    // save.delete
    // -----------------------------------------------------------------

    static TSharedRef<FJsonObject> HandleSaveDelete(
        const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
    {
        check(IsInGameThread());

        FString Slot;
        FString ParseErr;
        if (!ParseSlot(Args, Slot, ParseErr))
        {
            return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
        }
        const int32 UserIndex = ParseUserIndex(Args);

        const bool bExisted = UGameplayStatics::DoesSaveGameExist(Slot, UserIndex);
        // DeleteGameInSlot returns true iff a file was actually removed; we
        // surface both flags so callers can distinguish "deleted now" from
        // "wasn't there".
        const bool bDeleted = bExisted
            ? UGameplayStatics::DeleteGameInSlot(Slot, UserIndex)
            : false;

        TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
        Data->SetBoolField(TEXT("deleted"), bDeleted);
        Data->SetBoolField(TEXT("existed"), bExisted);
        Data->SetStringField(TEXT("slot"), Slot);
        Data->SetNumberField(TEXT("user_index"), UserIndex);
        return Data;
    }
}

void UeMcp::RegisterSaveHandlers(FUeMcpDispatcher& Dispatcher)
{
    using namespace UeMcpSaveHandlersPrivate;

    {
        FUeMcpToolRegistration Reg;
        Reg.ToolName = FName(TEXT("save.create"));
        Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
        Reg.bMutating = true;
        Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSaveCreate);
        Dispatcher.RegisterTool(Reg);
    }
    {
        FUeMcpToolRegistration Reg;
        Reg.ToolName = FName(TEXT("save.load"));
        Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
        Reg.bMutating = false;
        Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSaveLoad);
        Dispatcher.RegisterTool(Reg);
    }
    {
        FUeMcpToolRegistration Reg;
        Reg.ToolName = FName(TEXT("save.exists"));
        Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
        Reg.bMutating = false;
        Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSaveExists);
        Dispatcher.RegisterTool(Reg);
    }
    {
        FUeMcpToolRegistration Reg;
        Reg.ToolName = FName(TEXT("save.delete"));
        Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
        Reg.bMutating = true;
        Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSaveDelete);
        Dispatcher.RegisterTool(Reg);
    }
}
