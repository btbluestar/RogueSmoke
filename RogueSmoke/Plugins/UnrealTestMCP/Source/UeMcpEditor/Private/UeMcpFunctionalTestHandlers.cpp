// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpFunctionalTestHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "FunctionalTest.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpFunctionalTestHandlersPrivate
{
	/** Default dispatcher timeouts. `create_level` disk-saves; the spawn and
	 *  defaults setters are fast UObject touches; save mirrors `level.save`. */
	static constexpr double CreateLevelDefaultTimeoutSeconds   = 60.0;
	static constexpr double SpawnDefaultTimeoutSeconds         = 15.0;
	static constexpr double SetDefaultsTimeoutSeconds          = 10.0;
	static constexpr double SaveLevelDefaultTimeoutSeconds     = 30.0;

	/** Convention: `FTEST_` prefix is load-bearing for automation discovery.
	 *  See `03_UE_AUTOMATION_FRAMEWORK.md §"Discovery and registration gotchas"`. */
	static const TCHAR* const FTestLevelPrefix = TEXT("FTEST_");

	/** Default parent directory for newly-authored test levels. */
	static const TCHAR* const DefaultParentDir = TEXT("/Game/Tests/");

	/** Class path for the built-in Functional Test GameMode. We resolve via
	 *  `StaticLoadClass` rather than a static import so a missing
	 *  `FunctionalTesting` plugin at runtime degrades into a structured
	 *  error rather than a link-time failure — same reason we avoid a hard
	 *  `#include "FunctionalTestGameMode.h"` in handlers that don't depend on
	 *  the class layout. */
	static const TCHAR* const FunctionalTestGameModePath =
		TEXT("/Script/FunctionalTesting.FunctionalTestGameMode");

	/** Full class path for `AFunctionalTest`. Used as the default `class`. */
	static const TCHAR* const FunctionalTestClassPath =
		TEXT("/Script/FunctionalTesting.FunctionalTest");

	// --- small parsing helpers (mirrored from UeMcpActorHandlers.cpp) ----
	// We duplicate these rather than shipping a shared header because their
	// specific acceptance rules differ from call site to call site (e.g.
	// here `rotation` is flat, not nested under `transform`). The shape is
	// the same; the API is not worth sharing yet. Revisit at M5.

	static bool ReadTripleFromArray(
		const TArray<TSharedPtr<FJsonValue>>& Arr,
		double& A, double& B, double& C)
	{
		if (Arr.Num() < 3)
		{
			return false;
		}
		if (!Arr[0].IsValid() || !Arr[1].IsValid() || !Arr[2].IsValid())
		{
			return false;
		}
		double Ad = 0.0, Bd = 0.0, Cd = 0.0;
		if (!Arr[0]->TryGetNumber(Ad) || !Arr[1]->TryGetNumber(Bd) || !Arr[2]->TryGetNumber(Cd))
		{
			return false;
		}
		A = Ad; B = Bd; C = Cd;
		return true;
	}

	/** 3-element JSON array from an `FVector`. */
	static TSharedRef<FJsonValueArray> BuildVectorArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Elems;
		Elems.Reserve(3);
		Elems.Add(MakeShared<FJsonValueNumber>(V.X));
		Elems.Add(MakeShared<FJsonValueNumber>(V.Y));
		Elems.Add(MakeShared<FJsonValueNumber>(V.Z));
		return MakeShared<FJsonValueArray>(Elems);
	}

	/** 3-element JSON array from an `FRotator` (pitch, yaw, roll). */
	static TSharedRef<FJsonValueArray> BuildRotatorArray(const FRotator& R)
	{
		TArray<TSharedPtr<FJsonValue>> Elems;
		Elems.Reserve(3);
		Elems.Add(MakeShared<FJsonValueNumber>(R.Pitch));
		Elems.Add(MakeShared<FJsonValueNumber>(R.Yaw));
		Elems.Add(MakeShared<FJsonValueNumber>(R.Roll));
		return MakeShared<FJsonValueArray>(Elems);
	}

	/**
	 * Normalise a caller-supplied level path to the `/Game/<...>/<Name>`
	 * form. Same acceptance as `UeMcpLevelHandlers::NormaliseLevelPath` —
	 * see that function's comment for the rationale. Returns `true` when
	 * the path starts with `/Game/` after normalisation.
	 */
	static bool NormaliseLevelPath(const FString& Raw, FString& OutPath)
	{
		OutPath = Raw.TrimStartAndEnd();
		if (OutPath.IsEmpty())
		{
			return false;
		}
		OutPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (OutPath.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutPath = OutPath.LeftChop(5);
		}
		int32 DotIdx = INDEX_NONE;
		if (OutPath.FindLastChar(TEXT('.'), DotIdx))
		{
			int32 SlashIdx = INDEX_NONE;
			OutPath.FindLastChar(TEXT('/'), SlashIdx);
			if (SlashIdx < DotIdx)
			{
				OutPath = OutPath.Left(DotIdx);
			}
		}
		return OutPath.StartsWith(TEXT("/Game/"));
	}

	/**
	 * Join a `/Game/...` parent directory with a simple base name, ensuring
	 * exactly one `/` between them. `/Game/Tests/` + `FTEST_Foo` → `/Game/Tests/FTEST_Foo`.
	 * `/Game/Tests` + `FTEST_Foo` → `/Game/Tests/FTEST_Foo`.
	 */
	static FString JoinPackagePath(const FString& Parent, const FString& BaseName)
	{
		FString Trimmed = Parent;
		while (Trimmed.EndsWith(TEXT("/")))
		{
			Trimmed = Trimmed.LeftChop(1);
		}
		return Trimmed + TEXT("/") + BaseName;
	}

	/**
	 * Parse an `onConflict` string into a tri-state enum. Accepts
	 * `"skip"`, `"update"`, `"error"` (case-insensitive). Unknown values
	 * are rejected at the handler top with `SCHEMA_ERROR`.
	 */
	enum class EOnConflict : uint8
	{
		Skip,
		Update,
		Error,
	};

	static bool ParseOnConflict(
		const TSharedRef<FJsonObject>& Args,
		EOnConflict& OutValue,
		FString& OutError)
	{
		OutValue = EOnConflict::Skip;
		FString Raw;
		if (!Args->TryGetStringField(TEXT("onConflict"), Raw) || Raw.IsEmpty())
		{
			return true;
		}
		const FString Lower = Raw.ToLower();
		if (Lower == TEXT("skip"))   { OutValue = EOnConflict::Skip;   return true; }
		if (Lower == TEXT("update")) { OutValue = EOnConflict::Update; return true; }
		if (Lower == TEXT("error"))  { OutValue = EOnConflict::Error;  return true; }
		OutError = FString::Printf(
			TEXT("`onConflict` must be one of 'skip'|'update'|'error'; got '%s'"),
			*Raw);
		return false;
	}

	/**
	 * Resolve a user-supplied class string to a `UClass*`, preferring
	 * `AFunctionalTest` subclasses. Mirrors the resolution ladder from
	 * `UeMcpActorHandlers::ResolveActorClass` but keeps its own strategy
	 * list so we can report which strategies tried when something fails.
	 */
	static UClass* ResolveFunctionalTestClass(
		const FString& ClassName,
		TArray<FString>& OutTriedStrategies)
	{
		OutTriedStrategies.Reset();

		OutTriedStrategies.Add(TEXT("FindObject"));
		if (UClass* Found = FindObject<UClass>(nullptr, *ClassName))
		{
			return Found;
		}

		OutTriedStrategies.Add(TEXT("StaticLoadClass"));
		if (UClass* Loaded = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassName))
		{
			return Loaded;
		}

		OutTriedStrategies.Add(TEXT("short_name_scan"));
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			if (Cls == nullptr)
			{
				continue;
			}
			if (Cls->GetName().Equals(ClassName, ESearchCase::IgnoreCase))
			{
				return Cls;
			}
		}
		return nullptr;
	}

	/**
	 * Build `detail.tried` from a strategy list. Used when class resolution
	 * fails so the caller can reason about which lookup paths were exhausted.
	 */
	static TSharedRef<FJsonObject> BuildTriedStrategiesDetail(
		const TArray<FString>& Tried)
	{
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(Tried.Num());
		for (const FString& S : Tried)
		{
			Values.Add(MakeShared<FJsonValueString>(S));
		}
		Detail->SetArrayField(TEXT("tried"), Values);
		return Detail;
	}

	/** `tests.create_level` body. */
	static TSharedRef<FJsonObject> HandleTestsCreateLevel(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (GEditor == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; tests.create_level cannot run"));
		}

		// PIE guard. Creating a new map mid-PIE stomps the editor world
		// in ways most callers aren't ready for, same reasoning as
		// level.load / level.save.
		if (GEditor->IsPlayingSessionInEditor())
		{
			return UeMcp::MakeInlineError(
				TEXT("PIE_ACTIVE"),
				TEXT("Cannot create a level while PIE is running; stop PIE first"));
		}

		// --- Arg parse ---
		FString Name;
		if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`name` is required and must be a non-empty string"));
		}

		FString ParentDir = DefaultParentDir;
		Args->TryGetStringField(TEXT("parentDir"), ParentDir);
		if (ParentDir.IsEmpty())
		{
			ParentDir = DefaultParentDir;
		}
		// Accept both `/Game/Foo` and `/Game/Foo/`. The JoinPackagePath
		// helper normalises either one.
		ParentDir.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (!ParentDir.StartsWith(TEXT("/Game/")))
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`parentDir` must resolve to a `/Game/...` path; got '%s'"),
					*ParentDir));
		}

		bool bEnforceFtestPrefix = true;
		Args->TryGetBoolField(TEXT("enforceFtestPrefix"), bEnforceFtestPrefix);

		FString Template;
		Args->TryGetStringField(TEXT("template"), Template);
		FString NormalisedTemplate;
		if (!Template.IsEmpty())
		{
			if (!NormaliseLevelPath(Template, NormalisedTemplate))
			{
				return UeMcp::MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("`template` must resolve to a `/Game/...` asset; got '%s'"),
						*Template));
			}
			if (!FPackageName::DoesPackageExist(NormalisedTemplate))
			{
				return UeMcp::MakeInlineError(
					TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("Template map not found: '%s'"), *NormalisedTemplate));
			}
		}

		EOnConflict OnConflict = EOnConflict::Skip;
		{
			FString ParseError;
			if (!ParseOnConflict(Args, OnConflict, ParseError))
			{
				return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseError);
			}
		}

		// Compose the effective base name. When `enforceFtestPrefix=true`
		// and the caller's name does not start with `FTEST_`, we prepend
		// it — the prefix is load-bearing for functional-test discovery,
		// so silently fixing it is the helpful default. The explicit
		// opt-out exists for projects with their own convention.
		FString BaseName = Name.TrimStartAndEnd();
		// Strip a trailing `.umap` if the caller pasted a filesystem name.
		if (BaseName.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			BaseName = BaseName.LeftChop(5);
		}
		// Case-sensitive check: automation discovery matches the prefix
		// strictly, so `fTEST_` / `ftest_` are not equivalent to `FTEST_`.
		const bool bPrefixApplied =
			bEnforceFtestPrefix && !BaseName.StartsWith(FTestLevelPrefix, ESearchCase::CaseSensitive);
		if (bPrefixApplied)
		{
			BaseName = FString(FTestLevelPrefix) + BaseName;
		}

		const FString LevelPath = JoinPackagePath(ParentDir, BaseName);
		if (!FPackageName::IsValidLongPackageName(LevelPath))
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("Composed level path is not a valid long package name: '%s'"),
					*LevelPath));
		}

		// --- Conflict handling ---
		const bool bExists = FPackageName::DoesPackageExist(LevelPath);
		if (bExists && OnConflict == EOnConflict::Error)
		{
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), TEXT("LEVEL_EXISTS"));
			Out->SetStringField(TEXT("message"),
				FString::Printf(
					TEXT("Level package already exists at '%s'"), *LevelPath));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("levelPath"), LevelPath);
			Out->SetObjectField(TEXT("detail"), Detail);
			return Out;
		}
		if (bExists && OnConflict == EOnConflict::Skip)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("created"), false);
			Data->SetBoolField(TEXT("existed"), true);
			Data->SetBoolField(TEXT("updated"), false);
			Data->SetStringField(TEXT("levelPath"), LevelPath);
			Data->SetBoolField(TEXT("prefixApplied"), bPrefixApplied);
			return Data;
		}
		// `update` on an existing level: re-run the game-mode write + save.
		// This keeps the handler idempotent — calling it twice converges on
		// the intended state. For a non-existing level, `update` is identical
		// to `skip`-that-creates: we proceed to build.

		// --- Dirty guard on the current world ---
		// NewBlankMap / NewMapFromTemplate with bSaveExistingMap=false will
		// silently discard unsaved edits. The dialog-auto-decline hook is
		// still a TODO (see ue-api-gotchas §14), so if an overwrite modal
		// ever fires it will wedge the game thread. Refuse up-front unless
		// the caller explicitly opts in.
		bool bDiscardCurrentDirty = false;
		Args->TryGetBoolField(TEXT("discardCurrentDirty"), bDiscardCurrentDirty);

		const FWorldContext& EditorWC = GEditor->GetEditorWorldContext(false);
		UWorld* CurrentEditorWorld = EditorWC.World();
		if (CurrentEditorWorld != nullptr && !bDiscardCurrentDirty)
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
					TEXT("Current editor level has unsaved changes; save first or pass discardCurrentDirty=true"));
				Out->SetObjectField(TEXT("detail"), Detail);
				return Out;
			}
		}

		// --- Create the new map (or reuse the current world for `update`) ---
		// NewBlankMap replaces the current editor world with a fresh one.
		// Passing bSaveExistingMap=false is deliberate: we already
		// checked / agreed to discard dirty state above.
		const double StartSeconds = FPlatformTime::Seconds();

		UWorld* NewWorld = nullptr;
		if (!bExists)
		{
			if (!NormalisedTemplate.IsEmpty())
			{
				NewWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(
					NormalisedTemplate, /*bSaveExistingMap=*/false);
			}
			else
			{
				NewWorld = UEditorLoadingAndSavingUtils::NewBlankMap(
					/*bSaveExistingMap=*/false);
			}
			if (NewWorld == nullptr)
			{
				return UeMcp::MakeInlineError(
					TEXT("LEVEL_LOAD_FAILED"),
					TEXT("NewBlankMap/NewMapFromTemplate returned null"));
			}
		}
		else
		{
			// `update` path on existing level. Load the target map into the
			// editor world so the game-mode mutation sticks to the right
			// package. Pre-check existence (even though the outer gate did)
			// because the gate-to-load window can theoretically change.
			if (!FPackageName::DoesPackageExist(LevelPath))
			{
				return UeMcp::MakeInlineError(
					TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("Level vanished between existence check and load: '%s'"),
						*LevelPath));
			}
			NewWorld = UEditorLoadingAndSavingUtils::LoadMap(LevelPath);
			if (NewWorld == nullptr)
			{
				return UeMcp::MakeInlineError(
					TEXT("LEVEL_LOAD_FAILED"),
					FString::Printf(
						TEXT("Failed to load existing level for update: '%s'"),
						*LevelPath));
			}
		}

		// --- Wire up the GameMode ---
		// `LoadClass` on the script path so we don't take a hard include
		// dependency on `FunctionalTestGameMode.h` here (the FunctionalTesting
		// module is already linked per Build.cs but declaring the #include
		// adds an engine-path search for what is really just a symbol
		// resolution).
		UClass* GameModeClass = StaticLoadClass(
			UObject::StaticClass(), nullptr, FunctionalTestGameModePath);
		if (GameModeClass == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("GAME_MODE_RESOLUTION_FAILED"),
				FString::Printf(
					TEXT("Could not resolve FunctionalTestGameMode class at '%s'"),
					FunctionalTestGameModePath));
		}

		if (AWorldSettings* WorldSettings = NewWorld->GetWorldSettings())
		{
			WorldSettings->DefaultGameMode = GameModeClass;
			// Mark the package dirty so SaveMap serialises our change.
			if (UPackage* Pkg = NewWorld->GetOutermost())
			{
				Pkg->MarkPackageDirty();
			}
		}
		else
		{
			return UeMcp::MakeInlineError(
				TEXT("GAME_MODE_RESOLUTION_FAILED"),
				TEXT("New world has no WorldSettings actor; cannot set GameMode"));
		}

		// --- Save to the target path ---
		const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(NewWorld, LevelPath);
		const double EndSeconds = FPlatformTime::Seconds();
		if (!bSaved)
		{
			return UeMcp::MakeInlineError(
				TEXT("LEVEL_SAVE_FAILED"),
				FString::Printf(
					TEXT("SaveMap returned false for '%s'"), *LevelPath));
		}

		// --- Response shape per handler-conventions.md §3 ---
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("created"), !bExists);
		Data->SetBoolField(TEXT("existed"), bExists);
		Data->SetBoolField(TEXT("updated"), bExists);  // on `update` path we did rewrite
		Data->SetStringField(TEXT("levelPath"), NewWorld->GetPathName());
		Data->SetStringField(TEXT("packageName"), LevelPath);
		Data->SetBoolField(TEXT("prefixApplied"), bPrefixApplied);
		Data->SetStringField(TEXT("gameModeClass"), GameModeClass->GetPathName());
		Data->SetNumberField(TEXT("elapsed_ms"),
			FMath::RoundToDouble((EndSeconds - StartSeconds) * 1000.0));
		// Deliberately no `rollback` field — we do not delete levels from
		// tests. Callers that need cleanup do so via source control.
		return Data;
	}

	/** `tests.spawn_functional_test` body. */
	static TSharedRef<FJsonObject> HandleTestsSpawnFunctionalTest(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (GEditor == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; tests.spawn_functional_test cannot run"));
		}

		// Authoring is an editor-world operation. Refuse during PIE.
		if (GEditor->IsPlayingSessionInEditor())
		{
			return UeMcp::MakeInlineError(
				TEXT("PIE_ACTIVE"),
				TEXT("Cannot spawn a functional test while PIE is running; stop PIE first"));
		}

		// --- Arg parse ---
		FString LevelPathRaw;
		if (!Args->TryGetStringField(TEXT("levelPath"), LevelPathRaw) || LevelPathRaw.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`levelPath` is required and must be a non-empty string"));
		}
		FString NormalisedLevelPath;
		if (!NormaliseLevelPath(LevelPathRaw, NormalisedLevelPath))
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`levelPath` must resolve to a `/Game/...` asset; got '%s'"),
					*LevelPathRaw));
		}

		// Resolve the editor world. We refuse to auto-load the target map:
		// loading a map is a spicier operation (dirty-world dialogs, etc.)
		// and the Python side is expected to sequence `level.load` first.
		const FWorldContext& EditorWC = GEditor->GetEditorWorldContext(false);
		UWorld* EditorWorld = EditorWC.World();
		if (EditorWorld == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("No editor world available"));
		}

		// Compare the current world's package name with the requested level.
		// GetPathName is like `/Game/Tests/FTEST_Foo.FTEST_Foo`; `levelPath`
		// after normalisation is `/Game/Tests/FTEST_Foo`. Compare via the
		// outermost package name, which is unambiguous.
		UPackage* EditorPackage = EditorWorld->GetOutermost();
		const FString EditorPackageName = EditorPackage ? EditorPackage->GetName() : FString();
		if (EditorPackageName != NormalisedLevelPath)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("requestedLevelPath"), NormalisedLevelPath);
			Detail->SetStringField(TEXT("currentEditorLevel"), EditorPackageName);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), TEXT("NOT_FOUND"));
			Out->SetStringField(TEXT("message"),
				TEXT("Target level is not the current editor world; call level.load first"));
			Out->SetObjectField(TEXT("detail"), Detail);
			return Out;
		}

		// Optional class; default is AFunctionalTest.
		FString ClassName = FunctionalTestClassPath;
		Args->TryGetStringField(TEXT("class"), ClassName);
		if (ClassName.IsEmpty())
		{
			ClassName = FunctionalTestClassPath;
		}

		TArray<FString> TriedStrategies;
		UClass* ResolvedClass = ResolveFunctionalTestClass(ClassName, TriedStrategies);
		if (ResolvedClass == nullptr)
		{
			TSharedRef<FJsonObject> Detail = BuildTriedStrategiesDetail(TriedStrategies);
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), TEXT("NOT_FOUND"));
			Out->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Class '%s' not found"), *ClassName));
			Out->SetObjectField(TEXT("detail"), Detail);
			return Out;
		}

		// Must be an AFunctionalTest subclass — anything else is a misuse
		// we want to reject loudly, not spawn and silently not-work.
		if (!ResolvedClass->IsChildOf(AFunctionalTest::StaticClass()))
		{
			return UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("Class '%s' is not an AFunctionalTest subclass"),
					*ResolvedClass->GetPathName()));
		}

		// Optional label (natural key).
		FString Label;
		Args->TryGetStringField(TEXT("name"), Label);

		EOnConflict OnConflict = EOnConflict::Skip;
		{
			FString ParseError;
			if (!ParseOnConflict(Args, OnConflict, ParseError))
			{
				return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseError);
			}
		}

		// --- Conflict: an existing actor with this label in this world? ---
		AActor* ExistingMatch = nullptr;
		if (!Label.IsEmpty())
		{
			const FString LowerLabel = Label.ToLower();
			for (TActorIterator<AActor> It(EditorWorld); It; ++It)
			{
				AActor* Actor = *It;
				if (Actor == nullptr)
				{
					continue;
				}
				if (Actor->GetActorLabel().ToLower() == LowerLabel)
				{
					ExistingMatch = Actor;
					break;
				}
			}
		}

		if (ExistingMatch != nullptr && OnConflict == EOnConflict::Error)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("actorPath"), ExistingMatch->GetPathName());

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), TEXT("ACTOR_EXISTS"));
			Out->SetStringField(TEXT("message"),
				FString::Printf(
					TEXT("An actor labelled '%s' already exists in this level"),
					*Label));
			Out->SetObjectField(TEXT("detail"), Detail);
			return Out;
		}

		// --- Transform parse (flat `location` / `rotation` / `scale`) ---
		FTransform SpawnTransform = FTransform::Identity;
		{
			const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
			if (Args->TryGetArrayField(TEXT("location"), LocArr) && LocArr)
			{
				double X = 0.0, Y = 0.0, Z = 0.0;
				if (!ReadTripleFromArray(*LocArr, X, Y, Z))
				{
					return UeMcp::MakeInlineError(
						TEXT("SCHEMA_ERROR"),
						TEXT("`location` must be a 3-element number array"));
				}
				SpawnTransform.SetLocation(FVector(X, Y, Z));
			}

			const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
			if (Args->TryGetArrayField(TEXT("rotation"), RotArr) && RotArr)
			{
				double P = 0.0, Y = 0.0, R = 0.0;
				if (!ReadTripleFromArray(*RotArr, P, Y, R))
				{
					return UeMcp::MakeInlineError(
						TEXT("SCHEMA_ERROR"),
						TEXT("`rotation` must be a 3-element number array (pitch, yaw, roll)"));
				}
				SpawnTransform.SetRotation(FRotator(P, Y, R).Quaternion());
			}

			const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
			if (Args->TryGetArrayField(TEXT("scale"), ScaleArr) && ScaleArr)
			{
				double X = 0.0, Y = 0.0, Z = 0.0;
				if (!ReadTripleFromArray(*ScaleArr, X, Y, Z))
				{
					return UeMcp::MakeInlineError(
						TEXT("SCHEMA_ERROR"),
						TEXT("`scale` must be a 3-element number array"));
				}
				SpawnTransform.SetScale3D(FVector(X, Y, Z));
			}
		}

		// --- Spawn (or `update` the existing match) ---
		AActor* Resulting = nullptr;
		bool bSpawned = false;
		bool bUpdated = false;

		if (ExistingMatch != nullptr && OnConflict == EOnConflict::Skip)
		{
			Resulting = ExistingMatch;
		}
		else if (ExistingMatch != nullptr && OnConflict == EOnConflict::Update)
		{
			// `update`: keep the existing actor but apply the requested
			// transform. Class mismatch on update is a SCHEMA_ERROR —
			// transitioning classes would require destroying + respawning
			// which surprises the caller.
			if (!ExistingMatch->GetClass()->IsChildOf(ResolvedClass))
			{
				return UeMcp::MakeInlineError(
					TEXT("TYPE_MISMATCH"),
					FString::Printf(
						TEXT("Existing actor '%s' is class '%s'; cannot be updated to '%s'"),
						*Label,
						*ExistingMatch->GetClass()->GetPathName(),
						*ResolvedClass->GetPathName()));
			}
			ExistingMatch->SetActorTransform(SpawnTransform);
			Resulting = ExistingMatch;
			bUpdated = true;
		}
		else
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.OverrideLevel = EditorWorld->PersistentLevel;
			AActor* NewActor = EditorWorld->SpawnActor<AActor>(
				ResolvedClass, SpawnTransform, SpawnParams);
			if (NewActor == nullptr)
			{
				return UeMcp::MakeInlineError(
					TEXT("SPAWN_FAILED"),
					FString::Printf(
						TEXT("SpawnActor returned null for class '%s'"),
						*ResolvedClass->GetPathName()));
			}
			if (!Label.IsEmpty())
			{
				NewActor->SetActorLabel(Label);
			}
			Resulting = NewActor;
			bSpawned = true;
		}

		if (Resulting == nullptr)
		{
			// Defensive — every branch above should have set Resulting.
			return UeMcp::MakeInlineError(
				TEXT("SPAWN_FAILED"),
				TEXT("Spawn dispatch did not produce an actor"));
		}

		// --- Mark the owning package dirty so SaveLevel picks it up ---
		if (UPackage* Pkg = Resulting->GetOutermost())
		{
			Pkg->MarkPackageDirty();
		}

		const FString ResultingPath = Resulting->GetPathName();

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("spawned"), bSpawned);
		Data->SetBoolField(TEXT("existed"), ExistingMatch != nullptr);
		Data->SetBoolField(TEXT("updated"), bUpdated);
		Data->SetStringField(TEXT("name"), Resulting->GetName());
		Data->SetStringField(TEXT("label"), Resulting->GetActorLabel());
		Data->SetStringField(TEXT("class"), Resulting->GetClass()->GetName());
		Data->SetStringField(TEXT("classPath"), Resulting->GetClass()->GetPathName());
		Data->SetStringField(TEXT("actorPath"), ResultingPath);
		Data->SetStringField(TEXT("levelPath"), NormalisedLevelPath);

		const FTransform ActualXf = Resulting->GetActorTransform();
		Data->SetField(TEXT("location"), BuildVectorArray(ActualXf.GetLocation()));
		Data->SetField(TEXT("rotation"),
			BuildRotatorArray(ActualXf.GetRotation().Rotator()));
		Data->SetField(TEXT("scale"), BuildVectorArray(ActualXf.GetScale3D()));

		// Rollback hint. Emit only when we actually spawned — updating an
		// existing actor shouldn't delete it on rollback.
		if (bSpawned)
		{
			TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
			Rollback->SetStringField(TEXT("tool"), TEXT("actor.destroy"));
			TSharedRef<FJsonObject> RollbackArgs = MakeShared<FJsonObject>();
			RollbackArgs->SetStringField(TEXT("actor_path"), ResultingPath);
			Rollback->SetObjectField(TEXT("args"), RollbackArgs);
			Data->SetObjectField(TEXT("rollback"), Rollback);
		}

		return Data;
	}

	/**
	 * Resolve an actor by its full path within the current editor world.
	 * Handler-local helper because the other resolvers in ActorHandlers
	 * also match on label / short name, which we explicitly don't want
	 * here — set-defaults is a precision operation and must not mis-target.
	 */
	static AActor* FindActorByPath(UWorld* World, const FString& ActorPath)
	{
		if (World == nullptr || ActorPath.IsEmpty())
		{
			return nullptr;
		}
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor != nullptr && Actor->GetPathName() == ActorPath)
			{
				return Actor;
			}
		}
		return nullptr;
	}

	/** `tests.set_functional_test_defaults` body. */
	static TSharedRef<FJsonObject> HandleTestsSetFunctionalTestDefaults(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (GEditor == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; tests.set_functional_test_defaults cannot run"));
		}
		if (GEditor->IsPlayingSessionInEditor())
		{
			return UeMcp::MakeInlineError(
				TEXT("PIE_ACTIVE"),
				TEXT("Cannot mutate actor defaults while PIE is running; stop PIE first"));
		}

		FString ActorPath;
		if (!Args->TryGetStringField(TEXT("actorPath"), ActorPath) || ActorPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`actorPath` is required and must be a non-empty string"));
		}

		const FWorldContext& EditorWC = GEditor->GetEditorWorldContext(false);
		UWorld* EditorWorld = EditorWC.World();
		if (EditorWorld == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("No editor world available"));
		}

		AActor* Actor = FindActorByPath(EditorWorld, ActorPath);
		if (Actor == nullptr)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("actorPath"), ActorPath);
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), TEXT("NOT_FOUND"));
			Out->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Actor '%s' not found in editor world"), *ActorPath));
			Out->SetObjectField(TEXT("detail"), Detail);
			return Out;
		}

		AFunctionalTest* Ft = Cast<AFunctionalTest>(Actor);
		if (Ft == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("Actor '%s' is class '%s', not an AFunctionalTest"),
					*ActorPath,
					*Actor->GetClass()->GetPathName()));
		}

		// Only touch fields the caller explicitly passed. Each field tracks
		// its own `applied` bool so the response documents exactly what we
		// changed.
		TArray<FString> Applied;

		double Timeout = 0.0;
		if (Args->TryGetNumberField(TEXT("timeout"), Timeout))
		{
			if (Timeout < 0.0)
			{
				return UeMcp::MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("`timeout` must be >= 0; got %.6f"), Timeout));
			}
			Ft->TimeLimit = static_cast<float>(Timeout);
			Applied.Add(TEXT("timeout"));
		}

		bool bIsEnabled = false;
		if (Args->TryGetBoolField(TEXT("isEnabled"), bIsEnabled))
		{
			// `bIsEnabled` is a `uint32:1` bitfield in the protected section
			// of AFunctionalTest. Reflection path: find the FBoolProperty
			// and set via SetPropertyValue_InContainer, which handles the
			// bitmask correctly. Going through reflection avoids a `friend`
			// declaration or a helper subclass just to touch one bit.
			FProperty* Prop = Ft->GetClass()->FindPropertyByName(
				FName(TEXT("bIsEnabled")));
			FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop);
			if (BoolProp == nullptr)
			{
				return UeMcp::MakeInlineError(
					TEXT("PLUGIN_INTERNAL_ERROR"),
					TEXT("Could not find bIsEnabled FBoolProperty on AFunctionalTest"));
			}
			BoolProp->SetPropertyValue_InContainer(Ft, bIsEnabled);
			Applied.Add(TEXT("isEnabled"));
		}

		int32 Reruns = 0;
		if (Args->TryGetNumberField(TEXT("reruns"), Reruns))
		{
			if (Reruns < 0)
			{
				return UeMcp::MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					FString::Printf(TEXT("`reruns` must be >= 0; got %d"), Reruns));
			}
			// `AFunctionalTest` doesn't expose a single `RerunCount` field;
			// its re-run model is a list of named reasons in `RerunCauses`.
			// We map `reruns=N` to `N` synthesised causes. This is the only
			// Blueprint-friendly hook — `AddRerun(Name)` appends one cause,
			// and each cause produces one additional run cycle.
			Ft->RerunCauses.Reset();
			for (int32 i = 0; i < Reruns; ++i)
			{
				Ft->RerunCauses.Add(FName(*FString::Printf(TEXT("Rerun_%d"), i + 1)));
			}
			Applied.Add(TEXT("reruns"));
		}

		FString Description;
		if (Args->TryGetStringField(TEXT("description"), Description))
		{
			Ft->Description = Description;
			Applied.Add(TEXT("description"));
		}

		// Mark the owning package dirty so a subsequent `level.save` or
		// `tests.save_level` serialises the mutations. No-op if the package
		// resolver returns null (transient actor).
		if (UPackage* Pkg = Ft->GetOutermost())
		{
			Pkg->MarkPackageDirty();
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actorPath"), ActorPath);
		TArray<TSharedPtr<FJsonValue>> AppliedJson;
		AppliedJson.Reserve(Applied.Num());
		for (const FString& F : Applied)
		{
			AppliedJson.Add(MakeShared<FJsonValueString>(F));
		}
		Data->SetArrayField(TEXT("applied"), AppliedJson);
		Data->SetNumberField(TEXT("numApplied"), Applied.Num());
		// No `rollback` — the caller can re-invoke with the previous
		// values (handler-conventions.md §3).
		return Data;
	}

	/** `tests.save_level` body. Thin wrapper with the same behaviour as
	 *  `level.save`; we deliberately duplicate rather than delegate so the
	 *  tests namespace stays coherent on the wire. */
	static TSharedRef<FJsonObject> HandleTestsSaveLevel(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (GEditor == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; tests.save_level cannot run"));
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
					FString::Printf(
						TEXT("`path` must resolve to a `/Game/...` asset; got '%s'"),
						*RawPath));
			}
		}

		const double StartSeconds = FPlatformTime::Seconds();
		bool bSaved = false;
		if (bHasPath)
		{
			bSaved = UEditorLoadingAndSavingUtils::SaveMap(CurrentEditorWorld, NormalisedPath);
		}
		else
		{
			bSaved = UEditorLoadingAndSavingUtils::SaveCurrentLevel();
		}
		const double EndSeconds = FPlatformTime::Seconds();

		const FString SavedPath = CurrentEditorWorld->GetPathName();

		if (!bSaved)
		{
			return UeMcp::MakeInlineError(
				TEXT("LEVEL_SAVE_FAILED"),
				FString::Printf(
					TEXT("SaveMap/SaveCurrentLevel returned false (target='%s')"),
					bHasPath ? *NormalisedPath : *SavedPath));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("saved"), true);
		Data->SetStringField(TEXT("levelPath"), SavedPath);
		Data->SetNumberField(TEXT("elapsed_ms"),
			FMath::RoundToDouble((EndSeconds - StartSeconds) * 1000.0));
		// Non-reversible per handler-conventions.md §4.
		return Data;
	}
}

void UeMcp::RegisterFunctionalTestHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpFunctionalTestHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.create_level"));
		Reg.DefaultTimeoutSeconds = CreateLevelDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsCreateLevel);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.spawn_functional_test"));
		Reg.DefaultTimeoutSeconds = SpawnDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsSpawnFunctionalTest);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.set_functional_test_defaults"));
		Reg.DefaultTimeoutSeconds = SetDefaultsTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsSetFunctionalTestDefaults);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tests.save_level"));
		Reg.DefaultTimeoutSeconds = SaveLevelDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestsSaveLevel);
		Dispatcher.RegisterTool(Reg);
	}
}
