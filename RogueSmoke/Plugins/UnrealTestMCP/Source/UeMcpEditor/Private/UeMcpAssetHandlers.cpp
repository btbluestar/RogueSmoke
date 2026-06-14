// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Asset-graph + data-asset handlers.
//
// Read-only surface backed by `IAssetRegistry` for the graph queries
// (`referencers`, `dependencies`, `find_by_class`, `data_asset.list`)
// and `UObject::IsDataValid` for `data_asset.validate`.
//
// AssetRegistry vs. live UObjects: every graph query uses the registry's
// on-disk view. We deliberately do NOT load the asset to answer
// referencer/dependency questions — the registry already has the answer
// and loading would be an expensive (and frequently unwanted) side
// effect. `data_asset.validate` is the one exception: validation is
// defined on a live UObject, so we `TryLoad` the asset there.
//
// Path normalisation: the registry deals in package names (FName,
// `/Game/Foo/BP_Bar` form). For top-level assets package name == asset
// path for our purposes; we hand back the package name as the wire
// `assets[].path` field. When the caller passes a full object path
// (`/Game/Foo/BP_Bar.BP_Bar`), we strip back to the package name.
//
// Threading: all handlers are single-shot synchronous game-thread
// operations. The registry reads are cheap (in-memory hash lookups);
// the `find_by_class` path can grow with subclass enumeration, but the
// registry itself is the bottleneck and we hand straight off to it. No
// pending-handler variant needed for any of these.

#include "UeMcpAssetHandlers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataAsset.h"
#include "Misc/AssetRegistryInterface.h"
#include "Misc/DataValidation.h"
#include "Misc/PackageName.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpAssetHandlersPrivate
{
	/** Read-side; one game-thread registry call. */
	static constexpr double DefaultTimeoutSeconds = 10.0;

	/** Validate may load + run IsDataValid; give it a touch more headroom. */
	static constexpr double ValidateTimeoutSeconds = 30.0;

	/** Default class for `data_asset.list` when caller omits `class_filter`. */
	static const TCHAR* DefaultDataAssetClassPath = TEXT("/Script/Engine.DataAsset");

	/** Pull an `IAssetRegistry&` or return an inline error. */
	static IAssetRegistry* GetRegistryOrNull()
	{
		FAssetRegistryModule& Module = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
			AssetRegistryConstants::ModuleName);
		return Module.TryGet();
	}

	/**
	 * Normalise a wire-supplied asset path to a package name FName.
	 *
	 * Accepts:
	 *   - `/Game/Foo/BP_Bar`            (package path — passed through)
	 *   - `/Game/Foo/BP_Bar.BP_Bar`     (object path — strip after `.`)
	 *   - `/Game/Foo/BP_Bar.BP_Bar_C`   (class path — strip after `.`)
	 *
	 * Returns NAME_None and populates `OutErr` with a one-liner if the
	 * input is empty or doesn't look like a package path.
	 */
	static FName NormalisePackageName(const FString& InPath, FString& OutErr)
	{
		const FString Trimmed = InPath.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutErr = TEXT("`asset_path` is required and must be a non-empty string");
			return NAME_None;
		}
		// Split off any object-path suffix (`Foo.Foo` or `Foo.Foo_C`).
		FString PackagePart = Trimmed;
		int32 DotIdx = INDEX_NONE;
		if (PackagePart.FindChar(TEXT('.'), DotIdx))
		{
			PackagePart = PackagePart.Left(DotIdx);
		}
		// Engine paths must start with `/`.
		if (!PackagePart.StartsWith(TEXT("/")))
		{
			OutErr = FString::Printf(
				TEXT("`asset_path` must be an engine path starting with `/` (got '%s')"),
				*InPath);
			return NAME_None;
		}
		return FName(*PackagePart);
	}

	/**
	 * Parse `soft_only` / `hard_only` / `include_searchable_names` flags
	 * into the `(Category, Flags)` pair the registry expects. Mutually
	 * exclusive `soft_only` + `hard_only` → `SCHEMA_ERROR`.
	 *
	 * Returns false on conflict and writes the error JSON into `OutErr`.
	 */
	static bool BuildDependencyQuery(
		const TSharedRef<FJsonObject>& Args,
		UE::AssetRegistry::EDependencyCategory& OutCategory,
		UE::AssetRegistry::FDependencyQuery& OutQuery,
		TSharedRef<FJsonObject>& OutErr)
	{
		bool bSoftOnly = false;
		bool bHardOnly = false;
		bool bIncludeNames = false;
		(void)Args->TryGetBoolField(TEXT("soft_only"), bSoftOnly);
		(void)Args->TryGetBoolField(TEXT("hard_only"), bHardOnly);
		(void)Args->TryGetBoolField(TEXT("include_searchable_names"), bIncludeNames);

		if (bSoftOnly && bHardOnly)
		{
			OutErr = UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`soft_only` and `hard_only` are mutually exclusive."));
			return false;
		}

		// Default category is Package only (matches the engine default for
		// the FName-based GetReferencers/GetDependencies overloads). When the
		// caller asks for searchable-names we OR them in.
		OutCategory = UE::AssetRegistry::EDependencyCategory::Package;
		if (bIncludeNames)
		{
			OutCategory |= UE::AssetRegistry::EDependencyCategory::SearchableName;
		}

		// `FDependencyQuery` has an implicit ctor from the higher-level
		// `EDependencyQuery` flag enum; we use it for soft/hard so the
		// property-flag plumbing matches what the engine itself does.
		if (bHardOnly)
		{
			OutQuery = UE::AssetRegistry::FDependencyQuery(
				UE::AssetRegistry::EDependencyQuery::Hard);
		}
		else if (bSoftOnly)
		{
			OutQuery = UE::AssetRegistry::FDependencyQuery(
				UE::AssetRegistry::EDependencyQuery::Soft);
		}
		else
		{
			OutQuery = UE::AssetRegistry::FDependencyQuery();
		}
		return true;
	}

	/** Build a JSON array of FName package paths as strings. */
	static TArray<TSharedPtr<FJsonValue>> NamesToJsonArray(const TArray<FName>& Names)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Names.Num());
		for (const FName& N : Names)
		{
			if (!N.IsNone())
			{
				Out.Add(MakeShared<FJsonValueString>(N.ToString()));
			}
		}
		return Out;
	}

	/**
	 * Resolve a wire `class_path` argument to a `FTopLevelAssetPath`.
	 *
	 * Accepted forms:
	 *   - `/Script/Engine.DataAsset`             (engine class path)
	 *   - `/Game/Foo/BP_Bar.BP_Bar_C`            (BP class path)
	 *   - `/Game/Foo/BP_Bar`                     (BP asset — append `_C`)
	 *   - `BP_Foo_C`                             (short BP class — scan)
	 *   - `BP_Foo`                               (short BP asset — append `_C`)
	 *
	 * Returns false on resolution failure; populates `OutErr` with a
	 * NOT_FOUND inline error and echoes back the input.
	 */
	static bool ResolveClassPath(
		IAssetRegistry& Registry,
		const FString& InClassRef,
		FTopLevelAssetPath& OutPath,
		TSharedRef<FJsonObject>& OutErr)
	{
		const FString Trimmed = InClassRef.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutErr = UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`class_path` is required and must be a non-empty string"));
			return false;
		}

		// 1. Looks like a top-level path (`/Foo/Bar.Baz`)? Try direct.
		if (Trimmed.Contains(TEXT(".")) && Trimmed.StartsWith(TEXT("/")))
		{
			FTopLevelAssetPath Direct(Trimmed);
			if (Direct.IsValid())
			{
				OutPath = Direct;
				return true;
			}
		}

		// 2. Looks like a `/Game/...` BP asset path with no dot? Append _C.
		if (Trimmed.StartsWith(TEXT("/")) && !Trimmed.Contains(TEXT(".")))
		{
			FString AssetName;
			int32 LastSlash = INDEX_NONE;
			if (Trimmed.FindLastChar(TEXT('/'), LastSlash))
			{
				AssetName = Trimmed.Mid(LastSlash + 1);
			}
			if (!AssetName.IsEmpty())
			{
				const FString Composed = FString::Printf(
					TEXT("%s.%s_C"), *Trimmed, *AssetName);
				FTopLevelAssetPath Composed2(Composed);
				if (Composed2.IsValid())
				{
					OutPath = Composed2;
					return true;
				}
			}
		}

		// 3. Short name. Iterate loaded UClasses for a match. Try `Foo` and
		//    `Foo_C` so callers can pass either form.
		FString CandidateA = Trimmed;
		FString CandidateB = Trimmed.EndsWith(TEXT("_C"))
			? Trimmed.LeftChop(2)
			: Trimmed + TEXT("_C");
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			if (Cls == nullptr) continue;
			const FString Name = Cls->GetName();
			if (Name.Equals(CandidateA, ESearchCase::IgnoreCase)
				|| Name.Equals(CandidateB, ESearchCase::IgnoreCase))
			{
				OutPath = FTopLevelAssetPath(Cls);
				return true;
			}
		}

		// 4. Short BP-asset name (no `_C`): scan the asset registry by
		//    AssetName and append `_C` if a Blueprint matches.
		if (!Trimmed.EndsWith(TEXT("_C")))
		{
			FARFilter NameFilter;
			NameFilter.PackageNames; // unused
			TArray<FAssetData> Found;
			Registry.GetAllAssets(Found, /*bIncludeOnlyOnDiskAssets*/ true);
			for (const FAssetData& AD : Found)
			{
				if (AD.AssetName.ToString().Equals(Trimmed, ESearchCase::IgnoreCase))
				{
					const FString Composed = FString::Printf(
						TEXT("%s.%s_C"),
						*AD.PackageName.ToString(),
						*AD.AssetName.ToString());
					FTopLevelAssetPath P(Composed);
					if (P.IsValid())
					{
						OutPath = P;
						return true;
					}
				}
			}
		}

		// Couldn't resolve — emit NOT_FOUND with the input echoed back.
		TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
			TEXT("NOT_FOUND"),
			FString::Printf(
				TEXT("Could not resolve class_path '%s' to a known UClass or asset registry entry"),
				*InClassRef));
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("class_path"), InClassRef);
		Err->SetObjectField(TEXT("detail"), Detail);
		OutErr = Err;
		return false;
	}

	/**
	 * Shared body for `assets.referencers` + `assets.dependencies`. The
	 * `bReferencers` flag picks which registry call to make.
	 */
	static TSharedRef<FJsonObject> HandleAssetsGraphQuery(
		const TSharedRef<FJsonObject>& Args,
		bool bReferencers)
	{
		check(IsInGameThread());

		FString AssetPath;
		(void)Args->TryGetStringField(TEXT("asset_path"), AssetPath);
		FString ParseErr;
		const FName PackageName = NormalisePackageName(AssetPath, ParseErr);
		if (PackageName.IsNone())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
		}

		UE::AssetRegistry::EDependencyCategory Category;
		UE::AssetRegistry::FDependencyQuery Query;
		TSharedRef<FJsonObject> SchemaErr = MakeShared<FJsonObject>();
		if (!BuildDependencyQuery(Args, Category, Query, SchemaErr))
		{
			return SchemaErr;
		}

		IAssetRegistry* Registry = GetRegistryOrNull();
		if (Registry == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("AssetRegistry module is not available."));
		}

		// Ensure the asset itself is known to the registry. Otherwise the
		// registry will silently return zero referencers/dependencies for
		// any input — turning typos into "no results" rather than NOT_FOUND.
		// Cheap check: GetAssetPackageDataCopy returns empty optional if the
		// package is unknown.
		TOptional<FAssetPackageData> Pkg = Registry->GetAssetPackageDataCopy(PackageName);
		if (!Pkg.IsSet())
		{
			TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("Asset package '%s' is not registered with AssetRegistry"),
					*PackageName.ToString()));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("asset_path"), AssetPath);
			Detail->SetStringField(TEXT("normalised_package"), PackageName.ToString());
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}

		TArray<FName> Results;
		if (bReferencers)
		{
			Registry->GetReferencers(PackageName, Results, Category, Query);
		}
		else
		{
			Registry->GetDependencies(PackageName, Results, Category, Query);
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("asset_path"), PackageName.ToString());
		Out->SetNumberField(TEXT("count"), Results.Num());
		Out->SetArrayField(
			bReferencers ? TEXT("referencers") : TEXT("dependencies"),
			NamesToJsonArray(Results));
		return Out;
	}

	static TSharedRef<FJsonObject> HandleAssetsReferencers(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		return HandleAssetsGraphQuery(Args, /*bReferencers*/ true);
	}

	static TSharedRef<FJsonObject> HandleAssetsDependencies(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		return HandleAssetsGraphQuery(Args, /*bReferencers*/ false);
	}

	/**
	 * Shared body for `assets.find_by_class` and `data_asset.list`. The
	 * `DefaultClassFallback` arg supplies the implicit class when caller
	 * omits both `class_path` and `class_filter`.
	 *
	 * Resolution order on the wire arg:
	 *   1. `class_path` (preferred — `assets.find_by_class`)
	 *   2. `class_filter` (`data_asset.list` alias)
	 *   3. `DefaultClassFallback` if non-null
	 */
	static TSharedRef<FJsonObject> FindAssetsByClassImpl(
		const TSharedRef<FJsonObject>& Args,
		const TCHAR* DefaultClassFallback)
	{
		check(IsInGameThread());

		IAssetRegistry* Registry = GetRegistryOrNull();
		if (Registry == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("AssetRegistry module is not available."));
		}

		FString ClassRef;
		if (!Args->TryGetStringField(TEXT("class_path"), ClassRef)
			|| ClassRef.TrimStartAndEnd().IsEmpty())
		{
			(void)Args->TryGetStringField(TEXT("class_filter"), ClassRef);
		}
		if (ClassRef.TrimStartAndEnd().IsEmpty() && DefaultClassFallback != nullptr)
		{
			ClassRef = DefaultClassFallback;
		}
		if (ClassRef.TrimStartAndEnd().IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`class_path` is required and must be a non-empty string"));
		}

		FTopLevelAssetPath ClassTopPath;
		TSharedRef<FJsonObject> ResolveErr = MakeShared<FJsonObject>();
		if (!ResolveClassPath(*Registry, ClassRef, ClassTopPath, ResolveErr))
		{
			return ResolveErr;
		}

		// Build the filter. `bRecursivePaths` defaults to true — most
		// callers expect "everything under /Game" to recurse. `bRecursive-
		// Classes` is always true: DataAsset / Engine.Object base lookups
		// would return zero results without it.
		FARFilter Filter;
		Filter.ClassPaths.Add(ClassTopPath);
		Filter.bRecursiveClasses = true;
		Filter.bRecursivePaths = true;
		(void)Args->TryGetBoolField(TEXT("recursive"), Filter.bRecursivePaths);

		const TArray<TSharedPtr<FJsonValue>>* PackagePathsJson = nullptr;
		if (Args->TryGetArrayField(TEXT("package_paths"), PackagePathsJson)
			&& PackagePathsJson != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& V : *PackagePathsJson)
			{
				if (!V.IsValid()) continue;
				FString S;
				if (V->TryGetString(S))
				{
					const FString Stripped = S.TrimStartAndEnd();
					if (!Stripped.IsEmpty())
					{
						Filter.PackagePaths.Add(FName(*Stripped));
					}
				}
			}
		}

		TArray<FAssetData> AssetList;
		Registry->GetAssets(Filter, AssetList);

		TArray<TSharedPtr<FJsonValue>> AssetsJson;
		AssetsJson.Reserve(AssetList.Num());
		for (const FAssetData& AD : AssetList)
		{
			if (!AD.IsValid()) continue;
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			// Top-level asset path == package path for the cases we care about.
			Entry->SetStringField(TEXT("path"), AD.PackageName.ToString());
			Entry->SetStringField(TEXT("name"), AD.AssetName.ToString());
			Entry->SetStringField(TEXT("class"), AD.AssetClassPath.ToString());
			Entry->SetStringField(TEXT("parent_path"), AD.PackagePath.ToString());
			AssetsJson.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("class_path"), ClassTopPath.ToString());
		Out->SetNumberField(TEXT("count"), AssetsJson.Num());
		Out->SetArrayField(TEXT("assets"), AssetsJson);
		return Out;
	}

	static TSharedRef<FJsonObject> HandleAssetsFindByClass(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		return FindAssetsByClassImpl(Args, /*DefaultClassFallback*/ nullptr);
	}

	static TSharedRef<FJsonObject> HandleDataAssetList(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		return FindAssetsByClassImpl(Args, DefaultDataAssetClassPath);
	}

	/**
	 * `data_asset.validate` — load the asset, run `IsDataValid`, return
	 * the issues array. Loading is the only place in this file where we
	 * touch a live UObject; everything else is registry-only.
	 *
	 * Severity mapping: EMessageSeverity values are bit-positions in the
	 * engine, but we expose only the two callers care about: "error" and
	 * "warning". Info / PerformanceWarning collapse to "warning" for
	 * symmetry with the result enum. The numeric `total_errors` /
	 * `total_warnings` come from the context's own counters.
	 */
	static TSharedRef<FJsonObject> HandleDataAssetValidate(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString AssetPath;
		(void)Args->TryGetStringField(TEXT("asset_path"), AssetPath);
		AssetPath = AssetPath.TrimStartAndEnd();
		if (AssetPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`asset_path` is required and must be a non-empty string"));
		}

		// Try to resolve to a soft path and load. If the path is a package-
		// only form (`/Game/Foo/DA_Bar`) the soft-object loader needs the
		// `Foo.Foo` form — synthesise it.
		FString SoftPathString = AssetPath;
		if (!SoftPathString.Contains(TEXT(".")))
		{
			FString AssetName;
			int32 LastSlash = INDEX_NONE;
			if (SoftPathString.FindLastChar(TEXT('/'), LastSlash))
			{
				AssetName = SoftPathString.Mid(LastSlash + 1);
			}
			if (!AssetName.IsEmpty())
			{
				SoftPathString = FString::Printf(TEXT("%s.%s"), *SoftPathString, *AssetName);
			}
		}
		FSoftObjectPath SoftPath(SoftPathString);
		UObject* Asset = SoftPath.TryLoad();
		if (Asset == nullptr)
		{
			TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("Could not load asset '%s'"), *AssetPath));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("asset_path"), AssetPath);
			Detail->SetStringField(TEXT("tried_soft_path"), SoftPathString);
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}

		// Run validation under the modern API. UE 5.7's UObject::IsDataValid
		// takes an `FDataValidationContext&`; the legacy `TArray<FText>&`
		// overload is deprecated since 5.3. Cast to const so we hit the
		// non-deprecated `IsDataValid(...) const` overload, not the
		// deprecated non-const sibling.
		FDataValidationContext Context;
		const UObject* ConstAsset = Asset;
		const EDataValidationResult Result = ConstAsset->IsDataValid(Context);

		TArray<TSharedPtr<FJsonValue>> ErrorsJson;
		ErrorsJson.Reserve(Context.GetIssues().Num());
		for (const FDataValidationContext::FIssue& Issue : Context.GetIssues())
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			const TCHAR* SeverityStr = TEXT("warning");
			// Only EMessageSeverity::Error counts as "error" on the wire;
			// Warning / Info / PerformanceWarning collapse to "warning".
			if (Issue.Severity == EMessageSeverity::Error)
			{
				SeverityStr = TEXT("error");
			}
			Obj->SetStringField(TEXT("severity"), SeverityStr);
			Obj->SetStringField(TEXT("message"), Issue.Message.ToString());
			ErrorsJson.Add(MakeShared<FJsonValueObject>(Obj));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		// Map the tri-state result to a bool: only `Valid` is `true`. Both
		// `Invalid` and `NotValidated` are `false` — callers should branch
		// on `total_errors` if they need the finer signal.
		Out->SetBoolField(TEXT("valid"), Result == EDataValidationResult::Valid);
		Out->SetStringField(TEXT("asset"), AssetPath);
		Out->SetStringField(TEXT("class"), Asset->GetClass()->GetPathName());
		Out->SetArrayField(TEXT("errors"), ErrorsJson);
		Out->SetNumberField(TEXT("total_errors"),   (int32)Context.GetNumErrors());
		Out->SetNumberField(TEXT("total_warnings"), (int32)Context.GetNumWarnings());
		return Out;
	}
}

void UeMcp::RegisterAssetHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpAssetHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("assets.referencers"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleAssetsReferencers);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("assets.dependencies"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleAssetsDependencies);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("assets.find_by_class"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleAssetsFindByClass);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("data_asset.list"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleDataAssetList);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("data_asset.validate"));
		Reg.DefaultTimeoutSeconds = ValidateTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleDataAssetValidate);
		Dispatcher.RegisterTool(Reg);
	}
}
