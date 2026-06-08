// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// Object resolver pattern inspired by Incurian/AgentBridge (MIT).
// Study at `THIRD_PARTY/notes/Incurian-AgentBridge.md §2.8`.

#include "UeMcpObjectResolver.h"

#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectGlobals.h"

namespace UeMcpObjectResolverPrivate
{
	/**
	 * Walk `World`'s actors and match `ObjectId` first against
	 * `GetActorLabel()` (case-insensitive), then against `GetName()`.
	 *
	 * Returns the first label match, else the first name match, else nullptr.
	 * Label wins on tie because that's the string a human sees in the
	 * editor — the mental model every test-author uses.
	 */
	static AActor* FindActorByLabelOrName(UWorld* World, const FString& ObjectId)
	{
		if (World == nullptr)
		{
			return nullptr;
		}

		const FString Lower = ObjectId.ToLower();
		AActor* NameMatch = nullptr;

		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr)
			{
				continue;
			}
			if (Actor->GetActorLabel().ToLower() == Lower)
			{
				return Actor;
			}
			if (NameMatch == nullptr && Actor->GetName().ToLower() == Lower)
			{
				NameMatch = Actor;
				// keep scanning — a label match on a later actor wins.
			}
		}
		return NameMatch;
	}

	/**
	 * True if `ObjectId` looks like a soft-object-path — either starts
	 * with `/` (e.g. `/Game/Foo/Bar`) or contains `.` (e.g. `Bar.Bar_C`).
	 * Used to skip the `FSoftObjectPath` strategies for bare identifiers
	 * like `BP_Door_3` that can only be actor labels.
	 */
	static bool LooksLikeObjectPath(const FString& ObjectId)
	{
		return ObjectId.StartsWith(TEXT("/")) || ObjectId.Contains(TEXT("."));
	}

	/**
	 * Append `_C` to an `/Game/Foo/Bar` path, producing
	 * `/Game/Foo/Bar.Bar_C`. If the string already contains `.`, we
	 * assume the caller meant a specific sub-object and don't touch it.
	 * Returns empty string when no transformation was possible.
	 */
	static FString DeriveClassPath(const FString& ObjectId)
	{
		if (ObjectId.IsEmpty() || ObjectId.Contains(TEXT(".")))
		{
			return FString();
		}
		int32 LastSlash = INDEX_NONE;
		if (!ObjectId.FindLastChar(TEXT('/'), LastSlash) || LastSlash == ObjectId.Len() - 1)
		{
			return FString();
		}
		const FString AssetName = ObjectId.RightChop(LastSlash + 1);
		return FString::Printf(TEXT("%s.%s_C"), *ObjectId, *AssetName);
	}

	/** Compose the inline-error JSON for a NOT_FOUND outcome. */
	static TSharedPtr<FJsonObject> MakeNotFoundError(
		const FString& ObjectId,
		const TArray<FString>& SearchedStrategies)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), TEXT("NOT_FOUND"));
		Out->SetStringField(TEXT("message"),
			FString::Printf(TEXT("Could not resolve object '%s'"), *ObjectId));

		TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("object"), ObjectId);
		TArray<TSharedPtr<FJsonValue>> StratValues;
		StratValues.Reserve(SearchedStrategies.Num());
		for (const FString& S : SearchedStrategies)
		{
			StratValues.Add(MakeShared<FJsonValueString>(S));
		}
		Detail->SetArrayField(TEXT("searched_strategies"), StratValues);
		Out->SetObjectField(TEXT("detail"), Detail);
		return Out;
	}
}

UeMcp::FUeMcpResolvedObject UeMcp::ResolveObject(
	const FString& ObjectId, UWorld* WorldHint)
{
	check(IsInGameThread());

	using namespace UeMcpObjectResolverPrivate;

	FUeMcpResolvedObject Out;

	const FString Trimmed = ObjectId.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		TSharedPtr<FJsonObject> Err = MakeShared<FJsonObject>();
		Err->SetStringField(TEXT("error"), TEXT("INVALID_PAYLOAD"));
		Err->SetStringField(TEXT("message"),
			TEXT("`object` is required and must be a non-empty string"));
		Out.ErrorInfo = Err;
		return Out;
	}

	TArray<FString> Tried;

	// Strategy 1: world-actor iteration (label then name). This is by far
	// the most common path for tests — humans type "BP_Door_3" when they
	// mean "the actor I placed labelled BP_Door_3."
	Tried.Add(TEXT("world_actor_label_or_name"));
	if (AActor* ActorMatch = FindActorByLabelOrName(WorldHint, Trimmed))
	{
		Out.Object = ActorMatch;
		Out.bFromWorld = true;
		return Out;
	}

	// Strategies 2-4 only make sense for strings that look like object
	// paths. A bare "BP_Door_3" isn't a soft-path candidate.
	const bool bLooksPath = LooksLikeObjectPath(Trimmed);

	// Strategy 2: FSoftObjectPath::ResolveObject (already-loaded).
	if (bLooksPath)
	{
		Tried.Add(TEXT("soft_object_path_resolve"));
		FSoftObjectPath Path(Trimmed);
		if (UObject* Resolved = Path.ResolveObject())
		{
			Out.Object = Resolved;
			Out.bFromAsset = true;
			return Out;
		}

		// Strategy 3: FSoftObjectPath::TryLoad (load on demand). This
		// triggers disk I/O and async-loading; the dispatcher's engine-
		// safety fence will have gated us against running inside GC or an
		// in-flight async-load, so calling this here is safe.
		Tried.Add(TEXT("soft_object_path_tryload"));
		if (UObject* Loaded = Path.TryLoad())
		{
			Out.Object = Loaded;
			Out.bFromAsset = true;
			Out.bLoaded = true;
			return Out;
		}
	}

	// Strategy 4: append `_C` and retry 2+3. Handles the common case
	// `/Game/Actors/BP_Door` -> `/Game/Actors/BP_Door.BP_Door_C`. Only
	// applies when the caller passed a bare asset path (no `.` already).
	const FString ClassPath = DeriveClassPath(Trimmed);
	if (!ClassPath.IsEmpty())
	{
		Tried.Add(TEXT("class_suffix_resolve"));
		FSoftObjectPath ClassSoftPath(ClassPath);
		if (UObject* Resolved = ClassSoftPath.ResolveObject())
		{
			Out.Object = Resolved;
			Out.bFromAsset = true;
			return Out;
		}

		Tried.Add(TEXT("class_suffix_tryload"));
		if (UObject* Loaded = ClassSoftPath.TryLoad())
		{
			Out.Object = Loaded;
			Out.bFromAsset = true;
			Out.bLoaded = true;
			return Out;
		}
	}

	// Strategy 5: give up. Structured error with the strategy list for
	// agent retry heuristics.
	Out.ErrorInfo = MakeNotFoundError(Trimmed, Tried);
	return Out;
}
