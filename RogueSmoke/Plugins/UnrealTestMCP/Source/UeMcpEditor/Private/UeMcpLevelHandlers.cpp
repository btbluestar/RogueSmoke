// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpLevelHandlers.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpLevelHandlersPrivate
{
	/** Default dispatcher timeouts. Reads are short; loads can swap the world. */
	static constexpr double CurrentDefaultTimeoutSeconds = 5.0;
	static constexpr double LoadDefaultTimeoutSeconds    = 60.0;
	static constexpr double SaveDefaultTimeoutSeconds    = 30.0;

	/**
	 * Normalise a caller-supplied level path to the `/Game/<...>/<Name>`
	 * form expected by `UEditorLoadingAndSavingUtils::LoadMap` and
	 * `SaveMap`. Accepts `/Game/Maps/X`, `/Game/Maps/X.X`, `X.umap`, or
	 * bare `X`; normalises backslashes to forward slashes; strips a
	 * trailing `.umap` and any `.AssetName` suffix.
	 *
	 * Returns `true` when the path starts with `/Game/` after
	 * normalisation; `false` otherwise (callers emit `SCHEMA_ERROR`).
	 */
	static bool NormaliseLevelPath(const FString& Raw, FString& OutPath)
	{
		OutPath = Raw.TrimStartAndEnd();
		if (OutPath.IsEmpty())
		{
			return false;
		}

		// Forward-slash-only, matching engine asset path conventions.
		OutPath.ReplaceInline(TEXT("\\"), TEXT("/"));

		// Strip `.umap` if the caller pasted a filesystem path.
		if (OutPath.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutPath = OutPath.LeftChop(5);
		}

		// Strip `.AssetName` suffix (the `/Game/Maps/X.X` short form).
		int32 DotIdx = INDEX_NONE;
		if (OutPath.FindLastChar(TEXT('.'), DotIdx))
		{
			// Only strip if the dot is AFTER the last slash — otherwise
			// we'd chop a directory-with-a-dot, which UE doesn't allow
			// but we'd rather reject than mangle.
			int32 SlashIdx = INDEX_NONE;
			OutPath.FindLastChar(TEXT('/'), SlashIdx);
			if (SlashIdx < DotIdx)
			{
				OutPath = OutPath.Left(DotIdx);
			}
		}

		return OutPath.StartsWith(TEXT("/Game/"));
	}

	/** `level.current` handler body. */
	static TSharedRef<FJsonObject> HandleLevelCurrent(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution Resolution = UeMcp::ResolveWorldFromArgs(Args);
		if (!Resolution.IsOk())
		{
			return UeMcp::MakeInlineError(Resolution.ErrorCode, Resolution.ErrorMessage);
		}

		UWorld* World = Resolution.World;
		UPackage* Package = World->GetOutermost();
		const FString PackageName = Package ? Package->GetName() : FString();
		const FString WorldName = World->GetName();
		const FString WorldPath = World->GetPathName();
		const bool bIsTemp = PackageName.StartsWith(TEXT("/Temp/"));

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("path"), WorldPath);
		Data->SetStringField(TEXT("package_name"), PackageName);
		Data->SetStringField(TEXT("world_name"), WorldName);
		Data->SetBoolField(TEXT("is_temp"), bIsTemp);
		Data->SetBoolField(TEXT("is_pie"), Resolution.bIsPIE);
		Data->SetStringField(TEXT("resolved_scope"),
			UeMcp::WorldScopeToString(Resolution.ResolvedScope));
		return Data;
	}

	/** `level.load` handler body. */
	static TSharedRef<FJsonObject> HandleLevelLoad(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (GEditor == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; level.load cannot run"));
		}

		// Refuse during PIE: loading a map under PIE stomps the play world
		// and leaves the editor in a state most callers aren't ready for.
		if (GEditor->IsPlayingSessionInEditor())
		{
			return UeMcp::MakeInlineError(
				TEXT("PIE_ACTIVE"),
				TEXT("Cannot load a level while PIE is running; stop PIE first"));
		}

		FString RawPath;
		if (!Args->TryGetStringField(TEXT("path"), RawPath) || RawPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`path` is required and must be a non-empty string"));
		}

		FString NormalisedPath;
		if (!NormaliseLevelPath(RawPath, NormalisedPath))
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`path` must resolve to a `/Game/...` asset; got '%s'"),
					*RawPath));
		}

		bool bSaveCurrentDirty = false;
		Args->TryGetBoolField(TEXT("save_current_dirty"), bSaveCurrentDirty);

		// Dirty pre-check. `UeMcpEditorSubsystem::StartDialogAutoDecline`
		// now auto-declines the FMessageDialog "Save changes?" prompt, so
		// a dirty load no longer wedges the game thread. We still
		// short-circuit with a structured error by default so the
		// discard is *explicit*: callers either save first, or pass
		// `save_current_dirty=true` and knowingly drop unsaved edits in
		// the current world (rather than silently losing them to the
		// auto-decline).
		const FWorldContext& EditorWC = GEditor->GetEditorWorldContext(false);
		UWorld* CurrentEditorWorld = EditorWC.World();
		FString PreviousPath;
		if (CurrentEditorWorld != nullptr)
		{
			PreviousPath = CurrentEditorWorld->GetPathName();

			if (!bSaveCurrentDirty)
			{
				UPackage* CurrentPackage = CurrentEditorWorld->GetOutermost();
				if (CurrentPackage != nullptr && CurrentPackage->IsDirty())
				{
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
					TArray<TSharedPtr<FJsonValue>> DirtyPackages;
					DirtyPackages.Add(MakeShared<FJsonValueString>(CurrentPackage->GetName()));
					Detail->SetArrayField(TEXT("dirty_packages"), DirtyPackages);

					TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
					Out->SetStringField(TEXT("error"), TEXT("LEVEL_DIRTY"));
					Out->SetStringField(TEXT("message"),
						TEXT("Current editor level has unsaved changes; save first or pass save_current_dirty=true"));
					Out->SetObjectField(TEXT("detail"), Detail);
					return Out;
				}
			}
		}

		// Pre-check: UEditorLoadingAndSavingUtils::LoadMap doesn't
		// cleanly fail on a non-existent path — it can return the
		// current world unchanged, which would mask the error. Verify
		// the package exists before attempting the load.
		if (!FPackageName::DoesPackageExist(NormalisedPath))
		{
			return UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Map package not found: '%s'"),
					*NormalisedPath));
		}

		const double StartSeconds = FPlatformTime::Seconds();
		UWorld* LoadedWorld = UEditorLoadingAndSavingUtils::LoadMap(NormalisedPath);
		const double EndSeconds = FPlatformTime::Seconds();

		if (LoadedWorld == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Failed to load map '%s' — not found or load error"),
					*NormalisedPath));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("loaded"), true);
		Data->SetStringField(TEXT("path"), LoadedWorld->GetPathName());
		if (!PreviousPath.IsEmpty())
		{
			Data->SetStringField(TEXT("previous_path"), PreviousPath);
		}
		Data->SetNumberField(TEXT("elapsed_ms"),
			FMath::RoundToDouble((EndSeconds - StartSeconds) * 1000.0));
		return Data;
	}

	/** `level.save` handler body. */
	static TSharedRef<FJsonObject> HandleLevelSave(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (GEditor == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; level.save cannot run"));
		}

		if (GEditor->IsPlayingSessionInEditor())
		{
			return UeMcp::MakeInlineError(
				TEXT("PIE_ACTIVE"),
				TEXT("Cannot save a level while PIE is running; stop PIE first"));
		}

		const FWorldContext& EditorWC = GEditor->GetEditorWorldContext(false);
		UWorld* CurrentEditorWorld = EditorWC.World();
		if (CurrentEditorWorld == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("No editor world available to save"));
		}

		FString RawPath;
		const bool bHasPath = Args->TryGetStringField(TEXT("path"), RawPath)
			&& !RawPath.IsEmpty();

		FString NormalisedPath;
		if (bHasPath)
		{
			if (!NormaliseLevelPath(RawPath, NormalisedPath))
			{
				return UeMcp::MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					FString::Printf(TEXT("`path` must resolve to a `/Game/...` asset; got '%s'"),
						*RawPath));
			}
		}

		const double StartSeconds = FPlatformTime::Seconds();
		bool bSaved = false;
		FString SavedPath;
		if (bHasPath)
		{
			bSaved = UEditorLoadingAndSavingUtils::SaveMap(CurrentEditorWorld, NormalisedPath);
			// After SaveMap, the world may have been renamed onto the new
			// package; read the real path back.
			SavedPath = CurrentEditorWorld->GetPathName();
		}
		else
		{
			bSaved = UEditorLoadingAndSavingUtils::SaveCurrentLevel();
			SavedPath = CurrentEditorWorld->GetPathName();
		}
		const double EndSeconds = FPlatformTime::Seconds();

		if (!bSaved)
		{
			return UeMcp::MakeInlineError(
				TEXT("LEVEL_SAVE_FAILED"),
				FString::Printf(TEXT("SaveMap/SaveCurrentLevel returned false (target='%s')"),
					bHasPath ? *NormalisedPath : *SavedPath));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("saved"), true);
		Data->SetStringField(TEXT("path"), SavedPath);
		Data->SetNumberField(TEXT("elapsed_ms"),
			FMath::RoundToDouble((EndSeconds - StartSeconds) * 1000.0));
		// Deliberately no `rollback` field: on-disk state is owned by
		// source control, not by us. See docs/handler-conventions.md §4.
		return Data;
	}
}

void UeMcp::RegisterLevelHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpLevelHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("level.current"));
		Reg.DefaultTimeoutSeconds = CurrentDefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleLevelCurrent);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("level.load"));
		Reg.DefaultTimeoutSeconds = LoadDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleLevelLoad);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("level.save"));
		Reg.DefaultTimeoutSeconds = SaveDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleLevelSave);
		Dispatcher.RegisterTool(Reg);
	}
}
