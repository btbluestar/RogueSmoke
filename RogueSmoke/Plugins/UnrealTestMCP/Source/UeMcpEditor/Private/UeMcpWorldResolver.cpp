// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpWorldResolver.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace UeMcp
{
	const TCHAR* WorldScopeToString(EWorldScope Scope)
	{
		switch (Scope)
		{
			case EWorldScope::PIE:    return TEXT("pie");
			case EWorldScope::Editor: return TEXT("editor");
			case EWorldScope::Auto:
			default:                  return TEXT("auto");
		}
	}

	EWorldScope ParseWorldArg(const TSharedPtr<FJsonObject>& Args)
	{
		// Default: Auto. Missing / wrong-type / unknown string all fall through.
		if (!Args.IsValid())
		{
			return EWorldScope::Auto;
		}

		FString Raw;
		if (!Args->TryGetStringField(TEXT("world"), Raw))
		{
			return EWorldScope::Auto;
		}

		const FString Lower = Raw.ToLower();
		if (Lower == TEXT("pie"))
		{
			return EWorldScope::PIE;
		}
		if (Lower == TEXT("editor"))
		{
			return EWorldScope::Editor;
		}
		// Any other value, including "auto", folds to Auto. We deliberately
		// do NOT surface INVALID_PAYLOAD here — the resolver's contract is
		// permissive on the string, strict on the resolved world.
		return EWorldScope::Auto;
	}

	TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}

	FWorldResolution ResolveWorld(EWorldScope Scope)
	{
		check(IsInGameThread());

		FWorldResolution Result;

		if (GEditor == nullptr)
		{
			Result.ErrorCode = TEXT("EDITOR_NOT_READY");
			Result.ErrorMessage = TEXT("GEditor is null; editor is not yet initialized");
			return Result;
		}

		const bool bPieActive =
			GEditor->IsPlayingSessionInEditor() && GEditor->PlayWorld != nullptr;

		switch (Scope)
		{
			case EWorldScope::Auto:
			{
				if (bPieActive)
				{
					Result.World = GEditor->PlayWorld;
					Result.ResolvedScope = EWorldScope::PIE;
					Result.bIsPIE = true;
					return Result;
				}
				// Fall through to editor world.
				UWorld* EditorWorld = GEditor->GetEditorWorldContext(false).World();
				if (EditorWorld == nullptr)
				{
					Result.ErrorCode = TEXT("EDITOR_NOT_READY");
					Result.ErrorMessage = TEXT("Editor world is not available");
					return Result;
				}
				Result.World = EditorWorld;
				Result.ResolvedScope = EWorldScope::Editor;
				Result.bIsPIE = false;
				return Result;
			}

			case EWorldScope::PIE:
			{
				if (!bPieActive)
				{
					Result.ErrorCode = TEXT("NOT_IN_PIE");
					Result.ErrorMessage = TEXT("PIE is not active");
					return Result;
				}
				Result.World = GEditor->PlayWorld;
				Result.ResolvedScope = EWorldScope::PIE;
				Result.bIsPIE = true;
				return Result;
			}

			case EWorldScope::Editor:
			{
				UWorld* EditorWorld = GEditor->GetEditorWorldContext(false).World();
				if (EditorWorld == nullptr)
				{
					Result.ErrorCode = TEXT("EDITOR_NOT_READY");
					Result.ErrorMessage = TEXT("Editor world is not available");
					return Result;
				}
				Result.World = EditorWorld;
				Result.ResolvedScope = EWorldScope::Editor;
				Result.bIsPIE = false;
				return Result;
			}
		}

		// Defensive fallback — an unknown EWorldScope value should be
		// unreachable, but surface a structured error rather than a crash.
		Result.ErrorCode = TEXT("INVALID_PAYLOAD");
		Result.ErrorMessage = FString::Printf(
			TEXT("Unknown world scope value %u"), static_cast<uint32>(Scope));
		return Result;
	}

	FWorldResolution ResolveWorldFromArgs(const TSharedRef<FJsonObject>& Args)
	{
		// Pass a TSharedPtr view into the parser so it can bail cleanly on
		// missing keys without needing its own branch for "no args object".
		const TSharedPtr<FJsonObject> ArgsPtr = Args;
		return ResolveWorld(ParseWorldArg(ArgsPtr));
	}
}
