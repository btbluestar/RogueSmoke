// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpClassReflectionHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpClassReflectionHandlersPrivate
{
	/** Resolution touches the loader (`StaticLoadClass` may discover a
	 *  Blueprint asset); 15s matches the sibling reflection tools. */
	static constexpr double ReflectTimeoutSeconds = 15.0;

	/** Hard cap on classes per call — keeps the response bounded and the
	 *  game-thread hop short. Matches the "one round-trip, bounded" shape
	 *  of `cdo.defaults` / `actor.properties`. */
	static constexpr int32 MaxClassesPerCall = 64;

	/**
	 * Resolve a class identifier to a concrete `UClass*`. Strategy order
	 * is intentionally identical to `UeMcpActorHandlers.cpp::ResolveActorClass`
	 * (the canonical pattern in this plugin) so callers get the same
	 * resolution semantics regardless of which tool they reach for:
	 *   1. `FindObject<UClass>(nullptr, *Path)` — exact, already-loaded
	 *      `/Script/Module.ClassName` (the issue-#35 hot path) or a loaded
	 *      `/Game/.../BP_X.BP_X_C`.
	 *   2. `StaticLoadClass(UObject::StaticClass(), nullptr, *Path)` —
	 *      load-on-demand; forces Blueprint generated-class discovery.
	 *   3. `StaticLoadObject` + `UBlueprint::GeneratedClass` unwrap — for
	 *      a BP asset path supplied without the trailing `_C`.
	 *   4. Short-name scan via `TObjectIterator<UClass>` — bare names like
	 *      `Actor`. Bounded; first case-insensitive match wins.
	 *
	 * Returns nullptr if nothing matches. Does NOT log — the caller shapes
	 * the not-resolved entry into the response.
	 */
	static UClass* ResolveClassPath(const FString& Path)
	{
		if (Path.IsEmpty())
		{
			return nullptr;
		}

		// Strategy 1: exact already-loaded lookup. This is the issue-#35
		// hot path — `/Script/...` classes are compiled-in and resolve here
		// without ever touching the loader.
		if (UClass* Found = FindObject<UClass>(nullptr, *Path))
		{
			return Found;
		}

		// Strategy 2: load-on-demand. Covers BP generated-class paths
		// (`.../BP_Thing.BP_Thing_C`).
		if (UClass* Loaded = StaticLoadClass(UObject::StaticClass(), nullptr, *Path))
		{
			return Loaded;
		}

		// Strategy 3: load as a UObject; unwrap a UBlueprint to its
		// GeneratedClass. Handles BP asset paths with no trailing `_C`.
		if (UObject* LoadedObj = StaticLoadObject(UObject::StaticClass(), nullptr, *Path))
		{
			if (UBlueprint* BP = Cast<UBlueprint>(LoadedObj))
			{
				if (BP->GeneratedClass != nullptr)
				{
					return BP->GeneratedClass;
				}
			}
			if (UClass* AsClass = Cast<UClass>(LoadedObj))
			{
				return AsClass;
			}
		}

		// Strategy 4: bare short-name scan. Bounded — iterating every
		// UClass is a few thousand entries; first match wins.
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			if (Cls != nullptr && Cls->GetName().Equals(Path, ESearchCase::IgnoreCase))
			{
				return Cls;
			}
		}

		return nullptr;
	}

	/**
	 * Walk `UClass::GetSuperClass()` from `Cls`'s immediate parent up to
	 * and including `/Script/CoreUObject.Object`. The walked class itself
	 * is NOT included (it's reported separately as `class_path`). Each
	 * entry is the fully-qualified path name so the chain is directly
	 * comparable against a caller's `/Script/...` string.
	 *
	 * `UClass::GetSuperClass()` is the canonical inheritance accessor
	 * (`UObject/Class.h`); it returns the C++/BP parent and terminates at
	 * `UObject` (whose super is null), so the loop is naturally bounded.
	 */
	static TArray<TSharedPtr<FJsonValue>> BuildParentChain(const UClass* Cls)
	{
		TArray<TSharedPtr<FJsonValue>> Chain;
		if (Cls == nullptr)
		{
			return Chain;
		}
		for (UClass* Super = Cls->GetSuperClass();
			 Super != nullptr;
			 Super = Super->GetSuperClass())
		{
			Chain.Add(MakeShared<FJsonValueString>(Super->GetPathName()));
		}
		return Chain;
	}

	/**
	 * Build the per-class result object. `Path` is the caller's original
	 * (un-normalized) request string — echoed back as `requested` so a
	 * caller passing several paths can correlate entries positionally OR
	 * by value.
	 */
	static TSharedRef<FJsonObject> BuildClassEntry(
		const FString& Path,
		UClass* BaseClass,
		bool bHaveBase)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("requested"), Path);

		UClass* Cls = ResolveClassPath(Path);
		if (Cls == nullptr)
		{
			Entry->SetBoolField(TEXT("resolved"), false);
			// Stable shape: callers can branch on `resolved` alone without
			// null-guarding every other field.
			Entry->SetField(TEXT("class_path"), MakeShared<FJsonValueNull>());
			Entry->SetArrayField(TEXT("parent_chain"),
				TArray<TSharedPtr<FJsonValue>>());
			if (bHaveBase)
			{
				Entry->SetBoolField(TEXT("is_a"), false);
			}
			return Entry;
		}

		Entry->SetBoolField(TEXT("resolved"), true);
		Entry->SetStringField(TEXT("class_path"), Cls->GetPathName());
		Entry->SetStringField(TEXT("class_name"), Cls->GetName());
		Entry->SetArrayField(TEXT("parent_chain"), BuildParentChain(Cls));

		// Cheap reflection flags — these are exactly the
		// "did this UCLASS register and is it usable" checks the
		// fixture-verify smoke does inline today. We deliberately stick to
		// always-available `ClassFlags` (no `WITH_METADATA`-gated metadata
		// reads): `IsBlueprintBase` metadata is editor-build-only and
		// would make this handler's shape config-dependent.
		Entry->SetBoolField(TEXT("is_native"), Cls->IsNative());
		Entry->SetBoolField(TEXT("is_abstract"),
			Cls->HasAnyClassFlags(CLASS_Abstract));
		// CLASS_CompiledFromBlueprint => the class IS a Blueprint-generated
		// class. For native classes there's no config-free signal of
		// "blueprintable", so we report whether the class itself originated
		// from a Blueprint. This is the flag the fixture smokes actually
		// need (distinguishing a C++ fixture from a BP one).
		Entry->SetBoolField(TEXT("is_blueprint_generated"),
			Cls->HasAnyClassFlags(CLASS_CompiledFromBlueprint));

		if (bHaveBase)
		{
			// The isinstance-equivalent. `UClass::IsChildOf` walks the
			// super chain in C++ and returns true when `Cls == BaseClass`
			// too (a class is its own subclass) — matches Python's
			// `issubclass` / the smoke's `isinstance` intent.
			Entry->SetBoolField(TEXT("is_a"),
				BaseClass != nullptr && Cls->IsChildOf(BaseClass));
		}
		return Entry;
	}

	/**
	 * Collect the requested class paths. Accepts either:
	 *   - `class_path` : a single string, or
	 *   - `class_paths`: an array of strings
	 * (both names accepted; `class_path` mirrors `cdo.defaults`, the
	 * plural is the batch ergonomic form). Returns false + fills OutErr on
	 * a malformed/empty/over-cap request.
	 */
	static bool CollectClassPaths(
		const TSharedRef<FJsonObject>& Args,
		TArray<FString>& OutPaths,
		TSharedPtr<FJsonObject>& OutErr)
	{
		FString Single;
		if (Args->TryGetStringField(TEXT("class_path"), Single)
			&& !Single.IsEmpty())
		{
			OutPaths.Add(Single);
		}

		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Args->TryGetArrayField(TEXT("class_paths"), Arr) && Arr != nullptr)
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				FString P;
				if (V.IsValid() && V->TryGetString(P) && !P.IsEmpty())
				{
					OutPaths.Add(P);
				}
			}
		}

		if (OutPaths.Num() == 0)
		{
			OutErr = UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`class_path` (string) or `class_paths` (array of "
					 "strings) is required and must be non-empty"));
			return false;
		}
		if (OutPaths.Num() > MaxClassesPerCall)
		{
			OutErr = UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("too many class paths (%d); cap is %d per call"),
					OutPaths.Num(), MaxClassesPerCall));
			return false;
		}
		return true;
	}

	/** `class.reflect` body. */
	static TSharedRef<FJsonObject> HandleClassReflect(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		TArray<FString> Paths;
		TSharedPtr<FJsonObject> Err;
		if (!CollectClassPaths(Args, Paths, Err))
		{
			return Err.ToSharedRef();
		}

		// Optional `is_a` base. Resolve once up front — every entry
		// compares against the same base.
		FString BasePath;
		const bool bHaveBase =
			Args->TryGetStringField(TEXT("is_a"), BasePath)
			&& !BasePath.IsEmpty();
		UClass* BaseClass = bHaveBase ? ResolveClassPath(BasePath) : nullptr;
		if (bHaveBase && BaseClass == nullptr)
		{
			// A supplied-but-unresolvable base is a caller error, not a
			// per-class miss — fail the whole call so the mistake is loud.
			return UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("`is_a` base class '%s' did not resolve"),
					*BasePath));
		}

		TArray<TSharedPtr<FJsonValue>> Entries;
		Entries.Reserve(Paths.Num());
		for (const FString& P : Paths)
		{
			Entries.Add(MakeShared<FJsonValueObject>(
				BuildClassEntry(P, BaseClass, bHaveBase)));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("classes"), Entries);
		Data->SetNumberField(TEXT("count"), Entries.Num());
		if (bHaveBase)
		{
			Data->SetStringField(TEXT("is_a"), BaseClass->GetPathName());
		}
		return Data;
	}
}

void UeMcp::RegisterClassReflectionHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpClassReflectionHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("class.reflect"));
		Reg.DefaultTimeoutSeconds = ReflectTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleClassReflect);
		Dispatcher.RegisterTool(Reg);
	}
}
