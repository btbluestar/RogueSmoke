// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpActorHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpPropertyAccessor.h"
#include "UeMcpPropertyValue.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpActorHandlersPrivate
{
	/** Default dispatcher timeouts. */
	static constexpr double ListDefaultTimeoutSeconds       = 10.0;
	static constexpr double SpawnDefaultTimeoutSeconds      = 15.0;
	static constexpr double DestroyDefaultTimeoutSeconds    = 10.0;
	/** Composites can do more work: spawn-and-set touches N properties,
	 *  set-properties similarly. Bumped to 30s to give room. */
	static constexpr double CompositeDefaultTimeoutSeconds  = 30.0;
	/** spawn_batch can spawn many actors (each with property writes) in
	 *  one dispatch. Per docs/handler-conventions.md §5 the write
	 *  envelope is 60s; a 50-actor batch at ~5-20ms/spawn is well under
	 *  that, but property-heavy entries widen the per-actor cost — 60s
	 *  keeps the all-or-nothing batch inside one safe envelope. */
	static constexpr double SpawnBatchDefaultTimeoutSeconds = 60.0;

	/** Default cap on actors returned before truncation. */
	static constexpr int32 DefaultActorLimit = 500;

	/** Build a 3-element JSON array from an `FVector`. */
	static TSharedRef<FJsonValueArray> BuildVectorArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Elems;
		Elems.Reserve(3);
		Elems.Add(MakeShared<FJsonValueNumber>(V.X));
		Elems.Add(MakeShared<FJsonValueNumber>(V.Y));
		Elems.Add(MakeShared<FJsonValueNumber>(V.Z));
		return MakeShared<FJsonValueArray>(Elems);
	}

	/** Build a 3-element JSON array from an `FRotator` (pitch, yaw, roll). */
	static TSharedRef<FJsonValueArray> BuildRotatorArray(const FRotator& R)
	{
		TArray<TSharedPtr<FJsonValue>> Elems;
		Elems.Reserve(3);
		Elems.Add(MakeShared<FJsonValueNumber>(R.Pitch));
		Elems.Add(MakeShared<FJsonValueNumber>(R.Yaw));
		Elems.Add(MakeShared<FJsonValueNumber>(R.Roll));
		return MakeShared<FJsonValueArray>(Elems);
	}

	/** Read a 3-element number array out of a JSON array; leave defaults on mismatch. */
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
		A = Ad;
		B = Bd;
		C = Cd;
		return true;
	}

	/**
	 * Parse an optional `transform: {location?, rotation_euler_deg?, scale?}`
	 * arg into an `FTransform`. Missing fields default to identity. Returns
	 * true on success; populates `OutTransform` with the parsed pieces.
	 *
	 * Returns false with `OutError` populated on a malformed sub-array. An
	 * entirely missing `transform` is treated as success with identity.
	 */
	static bool ParseOptionalTransform(
		const TSharedRef<FJsonObject>& Args,
		FTransform& OutTransform,
		FString& OutError)
	{
		OutTransform = FTransform::Identity;

		const TSharedPtr<FJsonObject>* TransformObjPtr = nullptr;
		if (!Args->TryGetObjectField(TEXT("transform"), TransformObjPtr)
			|| !TransformObjPtr || !TransformObjPtr->IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonObject>& TransformObj = *TransformObjPtr;

		// Location.
		const TArray<TSharedPtr<FJsonValue>>* LocArr = nullptr;
		if (TransformObj->TryGetArrayField(TEXT("location"), LocArr) && LocArr)
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!ReadTripleFromArray(*LocArr, X, Y, Z))
			{
				OutError = TEXT("`transform.location` must be a 3-element number array");
				return false;
			}
			OutTransform.SetLocation(FVector(X, Y, Z));
		}

		// Rotation (pitch, yaw, roll).
		const TArray<TSharedPtr<FJsonValue>>* RotArr = nullptr;
		if (TransformObj->TryGetArrayField(TEXT("rotation_euler_deg"), RotArr) && RotArr)
		{
			double P = 0.0, Y = 0.0, R = 0.0;
			if (!ReadTripleFromArray(*RotArr, P, Y, R))
			{
				OutError = TEXT("`transform.rotation_euler_deg` must be a 3-element number array");
				return false;
			}
			OutTransform.SetRotation(FRotator(P, Y, R).Quaternion());
		}

		// Scale.
		const TArray<TSharedPtr<FJsonValue>>* ScaleArr = nullptr;
		if (TransformObj->TryGetArrayField(TEXT("scale"), ScaleArr) && ScaleArr)
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			if (!ReadTripleFromArray(*ScaleArr, X, Y, Z))
			{
				OutError = TEXT("`transform.scale` must be a 3-element number array");
				return false;
			}
			OutTransform.SetScale3D(FVector(X, Y, Z));
		}

		return true;
	}

	/**
	 * Resolve a user-supplied class string to a concrete `UClass*`.
	 *
	 * Resolution strategies, in order:
	 *   1. `FindObject<UClass>(nullptr, *ClassName)` — exact full path such
	 *      as `/Script/Engine.StaticMeshActor`.
	 *   2. `StaticLoadClass(UObject::StaticClass(), nullptr, *ClassName)`
	 *      — accepts asset paths like `/Game/BPs/BP_Thing.BP_Thing_C`.
	 *   3. Short-name scan via `TObjectIterator<UClass>` — for bare class
	 *      names like `StaticMeshActor`. Bounded, first match wins.
	 *
	 * Blueprint assets (`UBlueprint`) are unwrapped to their
	 * `GeneratedClass`. If nothing matches, returns nullptr and populates
	 * `OutTriedStrategies` for error detail.
	 */
	static UClass* ResolveActorClass(
		const FString& ClassName,
		TArray<FString>& OutTriedStrategies)
	{
		OutTriedStrategies.Reset();

		// Strategy 1: exact `FindObject<UClass>`.
		OutTriedStrategies.Add(TEXT("FindObject"));
		if (UClass* Found = FindObject<UClass>(nullptr, *ClassName))
		{
			return Found;
		}

		// Strategy 2: load-on-demand via StaticLoadClass. Covers BP
		// generated-class paths and forces asset discovery.
		OutTriedStrategies.Add(TEXT("StaticLoadClass"));
		if (UClass* Loaded = StaticLoadClass(UObject::StaticClass(), nullptr, *ClassName))
		{
			return Loaded;
		}

		// Strategy 2b: load as a UObject; if it's a UBlueprint, use its
		// GeneratedClass. This handles callers who pass a BP asset path
		// without the trailing `_C`.
		OutTriedStrategies.Add(TEXT("LoadObject_as_Blueprint"));
		if (UObject* Loaded = StaticLoadObject(UObject::StaticClass(), nullptr, *ClassName))
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Loaded))
			{
				if (BP->GeneratedClass != nullptr)
				{
					return BP->GeneratedClass;
				}
			}
			if (UClass* AsClass = Cast<UClass>(Loaded))
			{
				return AsClass;
			}
		}

		// Strategy 3: short-name scan. Covers the `StaticMeshActor`-style
		// bare class name. Bounded — first match wins. Iterating every
		// UClass is cheap (~a few thousand classes) and bounded.
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

	/** Build a compact per-actor JSON object for `actor.list`. */
	static TSharedRef<FJsonObject> BuildActorJson(AActor* Actor)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Actor == nullptr)
		{
			return Out;
		}

		UClass* Cls = Actor->GetClass();
		const FString ClassName = Cls ? Cls->GetName() : FString();
		const FString ClassPath = Cls ? Cls->GetPathName() : FString();

		Out->SetStringField(TEXT("name"), Actor->GetName());
		Out->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Out->SetStringField(TEXT("class"), ClassName);
		Out->SetStringField(TEXT("class_path"), ClassPath);
		Out->SetStringField(TEXT("path"), Actor->GetPathName());

		const FTransform Xf = Actor->GetActorTransform();
		Out->SetField(TEXT("location"), BuildVectorArray(Xf.GetLocation()));
		Out->SetField(TEXT("rotation_euler_deg"), BuildRotatorArray(Xf.GetRotation().Rotator()));
		Out->SetField(TEXT("scale"), BuildVectorArray(Xf.GetScale3D()));

		// Tags as string array.
		TArray<TSharedPtr<FJsonValue>> TagValues;
		TagValues.Reserve(Actor->Tags.Num());
		for (const FName& Tag : Actor->Tags)
		{
			TagValues.Add(MakeShared<FJsonValueString>(Tag.ToString()));
		}
		Out->SetArrayField(TEXT("tags"), TagValues);

		return Out;
	}

	/** Extract the short tail of a path (after the last `.` or `:` or `/`). */
	static FString GetPathTail(const FString& Path)
	{
		int32 Idx = INDEX_NONE;
		// Work from the end, any of the three separators.
		for (int32 i = Path.Len() - 1; i >= 0; --i)
		{
			const TCHAR C = Path[i];
			if (C == TEXT('.') || C == TEXT(':') || C == TEXT('/'))
			{
				Idx = i;
				break;
			}
		}
		return Idx == INDEX_NONE ? Path : Path.RightChop(Idx + 1);
	}

	/** `actor.list` handler body. */
	static TSharedRef<FJsonObject> HandleActorList(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution Resolution = UeMcp::ResolveWorldFromArgs(Args);
		if (!Resolution.IsOk())
		{
			return UeMcp::MakeInlineError(Resolution.ErrorCode, Resolution.ErrorMessage);
		}

		FString ClassFilter;
		Args->TryGetStringField(TEXT("class_filter"), ClassFilter);
		const FString LowerClassFilter = ClassFilter.ToLower();

		FString NameFilter;
		Args->TryGetStringField(TEXT("name_filter"), NameFilter);
		const FString LowerNameFilter = NameFilter.ToLower();

		int32 Limit = DefaultActorLimit;
		{
			int32 LimitRaw = 0;
			if (Args->TryGetNumberField(TEXT("limit"), LimitRaw) && LimitRaw > 0)
			{
				Limit = LimitRaw;
			}
		}

		int32 TotalMatched = 0;
		TArray<TSharedPtr<FJsonValue>> Emitted;
		Emitted.Reserve(Limit);

		for (TActorIterator<AActor> It(Resolution.World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr)
			{
				continue;
			}

			if (!LowerClassFilter.IsEmpty())
			{
				UClass* Cls = Actor->GetClass();
				if (Cls == nullptr)
				{
					continue;
				}
				const bool bShortMatch = Cls->GetName().ToLower() == LowerClassFilter;
				const bool bPathMatch  = Cls->GetPathName().ToLower() == LowerClassFilter;
				if (!bShortMatch && !bPathMatch)
				{
					continue;
				}
			}

			if (!LowerNameFilter.IsEmpty())
			{
				if (!Actor->GetActorLabel().ToLower().Contains(LowerNameFilter))
				{
					continue;
				}
			}

			TotalMatched++;
			if (Emitted.Num() < Limit)
			{
				Emitted.Add(MakeShared<FJsonValueObject>(BuildActorJson(Actor)));
			}
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("actors"), Emitted);
		Data->SetNumberField(TEXT("total_matched"), TotalMatched);
		Data->SetBoolField(TEXT("truncated"), TotalMatched > Emitted.Num());
		Data->SetStringField(TEXT("resolved_scope"),
			UeMcp::WorldScopeToString(Resolution.ResolvedScope));
		return Data;
	}

	/** `actor.spawn` handler body. */
	static TSharedRef<FJsonObject> HandleActorSpawn(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution Resolution = UeMcp::ResolveWorldFromArgs(Args);
		if (!Resolution.IsOk())
		{
			return UeMcp::MakeInlineError(Resolution.ErrorCode, Resolution.ErrorMessage);
		}

		FString ClassName;
		if (!Args->TryGetStringField(TEXT("class"), ClassName) || ClassName.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`class` is required and must be a non-empty string"));
		}

		TArray<FString> TriedStrategies;
		UClass* ResolvedClass = ResolveActorClass(ClassName, TriedStrategies);

		// UBlueprint-as-class fall-through: if someone managed to get a
		// UBlueprint back through FindObject (unusual but possible), prefer
		// its GeneratedClass.
		if (ResolvedClass != nullptr && ResolvedClass->IsChildOf(UBlueprint::StaticClass()))
		{
			// Extremely unlikely path; defensive.
			ResolvedClass = nullptr;
		}

		if (ResolvedClass == nullptr)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> TriedValues;
			TriedValues.Reserve(TriedStrategies.Num());
			for (const FString& Strat : TriedStrategies)
			{
				TriedValues.Add(MakeShared<FJsonValueString>(Strat));
			}
			Detail->SetArrayField(TEXT("tried"), TriedValues);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), TEXT("NOT_FOUND"));
			Out->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Class '%s' not found"), *ClassName));
			Out->SetObjectField(TEXT("detail"), Detail);
			return Out;
		}

		// The class must derive from AActor — SpawnActor on a non-actor
		// class is a misuse we want to reject loudly.
		if (!ResolvedClass->IsChildOf(AActor::StaticClass()))
		{
			return UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("Class '%s' is not an AActor subclass"),
					*ResolvedClass->GetPathName()));
		}

		FTransform SpawnTransform;
		FString TransformError;
		if (!ParseOptionalTransform(Args, SpawnTransform, TransformError))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), TransformError);
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AActor* Spawned = Resolution.World->SpawnActor<AActor>(
			ResolvedClass, SpawnTransform, SpawnParams);

		if (Spawned == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("SPAWN_FAILED"),
				FString::Printf(TEXT("SpawnActor returned null for class '%s'"),
					*ResolvedClass->GetPathName()));
		}

		// Optional label.
		FString Label;
		if (Args->TryGetStringField(TEXT("label"), Label) && !Label.IsEmpty())
		{
			Spawned->SetActorLabel(Label);
		}

		// Optional tags.
		const TArray<TSharedPtr<FJsonValue>>* TagsArr = nullptr;
		if (Args->TryGetArrayField(TEXT("tags"), TagsArr) && TagsArr)
		{
			TArray<FName> NewTags;
			NewTags.Reserve(TagsArr->Num());
			for (const TSharedPtr<FJsonValue>& Tag : *TagsArr)
			{
				FString TagStr;
				if (Tag.IsValid() && Tag->TryGetString(TagStr) && !TagStr.IsEmpty())
				{
					NewTags.Add(FName(*TagStr));
				}
			}
			Spawned->Tags = NewTags;
		}

		const FString SpawnedPath = Spawned->GetPathName();

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("spawned"), true);
		Data->SetStringField(TEXT("name"), Spawned->GetName());
		Data->SetStringField(TEXT("label"), Spawned->GetActorLabel());
		Data->SetStringField(TEXT("class"), ResolvedClass->GetName());
		Data->SetStringField(TEXT("class_path"), ResolvedClass->GetPathName());
		Data->SetStringField(TEXT("path"), SpawnedPath);

		const FTransform ActualXf = Spawned->GetActorTransform();
		Data->SetField(TEXT("location"), BuildVectorArray(ActualXf.GetLocation()));
		Data->SetField(TEXT("rotation_euler_deg"),
			BuildRotatorArray(ActualXf.GetRotation().Rotator()));

		// Rollback hint: actor.destroy by the spawned path. The dispatcher
		// does not currently hoist `rollback` to the top-level wire shape,
		// so it lands as `data.rollback`; that's fine — callers and the
		// future flow-runner will read it there. If the dispatcher is
		// later extended to promote this to the top level, the handler-
		// side shape (single top-level key on the returned JSON) needs no
		// change. See docs/handler-conventions.md §4.
		TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
		Rollback->SetStringField(TEXT("tool"), TEXT("actor.destroy"));
		TSharedRef<FJsonObject> RollbackArgs = MakeShared<FJsonObject>();
		RollbackArgs->SetStringField(TEXT("actor_path"), SpawnedPath);
		Rollback->SetObjectField(TEXT("args"), RollbackArgs);
		Data->SetObjectField(TEXT("rollback"), Rollback);

		return Data;
	}

	/** `actor.destroy` handler body. */
	static TSharedRef<FJsonObject> HandleActorDestroy(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution Resolution = UeMcp::ResolveWorldFromArgs(Args);
		if (!Resolution.IsOk())
		{
			return UeMcp::MakeInlineError(Resolution.ErrorCode, Resolution.ErrorMessage);
		}

		FString ActorPath;
		const bool bHasPath = Args->TryGetStringField(TEXT("actor_path"), ActorPath)
			&& !ActorPath.IsEmpty();

		FString ActorName;
		const bool bHasName = Args->TryGetStringField(TEXT("actor_name"), ActorName)
			&& !ActorName.IsEmpty();

		if (bHasPath == bHasName)
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("Exactly one of `actor_path` or `actor_name` is required"));
		}

		AActor* Match = nullptr;
		const FString LowerName = ActorName.ToLower();

		for (TActorIterator<AActor> It(Resolution.World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr)
			{
				continue;
			}
			if (bHasPath)
			{
				if (Actor->GetPathName() == ActorPath)
				{
					Match = Actor;
					break;
				}
			}
			else
			{
				// Match order: label, then GetName, then path tail.
				if (Actor->GetActorLabel().ToLower() == LowerName)
				{
					Match = Actor;
					break;
				}
				if (Match == nullptr && Actor->GetName().ToLower() == LowerName)
				{
					Match = Actor;
					// keep scanning — a label match on a later actor wins.
				}
				if (Match == nullptr
					&& GetPathTail(Actor->GetPathName()).ToLower() == LowerName)
				{
					Match = Actor;
				}
			}
		}

		if (Match == nullptr)
		{
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("searched_world"),
				UeMcp::WorldScopeToString(Resolution.ResolvedScope));
			if (bHasPath)
			{
				Detail->SetStringField(TEXT("actor_path"), ActorPath);
			}
			else
			{
				Detail->SetStringField(TEXT("actor_name"), ActorName);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), TEXT("NOT_FOUND"));
			Out->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Actor '%s' not found in the %s world"),
					bHasPath ? *ActorPath : *ActorName,
					UeMcp::WorldScopeToString(Resolution.ResolvedScope)));
			Out->SetObjectField(TEXT("detail"), Detail);
			return Out;
		}

		// Capture identifiers BEFORE destroy — post-destroy the actor is
		// pending-kill and some accessors become unreliable.
		const FString MatchedName  = Match->GetName();
		const FString MatchedLabel = Match->GetActorLabel();
		const FString MatchedPath  = Match->GetPathName();

		// Idempotency-style guard: if the match is already in a pending-
		// kill state, report success with `already_destroyed: true`. In
		// practice, the iterator filters pending-kill actors out; this is
		// belt-and-braces.
		bool bAlreadyDestroyed = false;
		if (!IsValid(Match))
		{
			bAlreadyDestroyed = true;
		}
		else
		{
			if (!Resolution.World->DestroyActor(Match))
			{
				return UeMcp::MakeInlineError(
					TEXT("SPAWN_FAILED"),
					FString::Printf(TEXT("DestroyActor refused to destroy '%s'"),
						*MatchedPath));
			}
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("destroyed"), true);
		Data->SetStringField(TEXT("name"), MatchedName);
		Data->SetStringField(TEXT("label"), MatchedLabel);
		Data->SetStringField(TEXT("path"), MatchedPath);
		if (bAlreadyDestroyed)
		{
			Data->SetBoolField(TEXT("already_destroyed"), true);
		}
		return Data;
	}

	// ------------------------------------------------------------------
	// actor.spawn_and_set — COMPOSITE handler.
	//
	// Informed by the tool-call ledger: `actor.spawn` is almost always
	// followed by a burst of `get_property` / `set_property` pairs to
	// configure the freshly-spawned actor. This tool collapses
	// "spawn + N property writes" into one round-trip with
	// fail-atomic semantics (default): if any write fails, the actor
	// is destroyed and the error is surfaced. Set `fail_atomic=false`
	// to keep the actor alive and collect per-property failures in
	// `failed_properties`.
	// ------------------------------------------------------------------

	/** True if the inline-error shape {error, message} is present. */
	static bool IsInlineError(const TSharedRef<FJsonObject>& Obj)
	{
		FString Code;
		return Obj->TryGetStringField(TEXT("error"), Code) && !Code.IsEmpty();
	}

	/**
	 * Map an accessor error info into a handler inline-error JSON.
	 * Accessor-error -> wire-code goes through the shared
	 * `UeMcp::AccessorErrorToCode` (issue #62); no local switch here.
	 */
	static TSharedRef<FJsonObject> AccessorErrorToInline(
		const FUeMcpAccessorErrorInfo& Err, const FString& Path)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), UeMcp::AccessorErrorToCode(Err.Code));
		Out->SetStringField(TEXT("message"),
			FString::Printf(TEXT("set_property on '%s' failed: %s"),
				*Path, *Err.Message));
		if (Err.Detail.IsValid())
		{
			Out->SetObjectField(TEXT("detail"), Err.Detail.ToSharedRef());
		}
		return Out;
	}

	/** `actor.spawn_and_set` handler body. */
	static TSharedRef<FJsonObject> HandleActorSpawnAndSet(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		// --- Pull out `properties` before delegating — the underlying
		// spawn handler doesn't know about it, and the wire-contract
		// validators on that side would flag it as an unknown field if
		// we weren't careful to forward a clean args dict.
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		Args->TryGetObjectField(TEXT("properties"), PropsObj);

		bool bFailAtomic = true;
		Args->TryGetBoolField(TEXT("fail_atomic"), bFailAtomic);

		// --- Step 1: spawn. Forward every field the spawn primitive
		// accepts (class, transform, tags, label, world).
		TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : Args->Values)
		{
			if (Kv.Key == TEXT("properties") || Kv.Key == TEXT("fail_atomic"))
			{
				continue;
			}
			SpawnArgs->SetField(Kv.Key, Kv.Value);
		}
		TSharedRef<FJsonObject> SpawnResp = HandleActorSpawn(SpawnArgs, Cancel);
		if (IsInlineError(SpawnResp))
		{
			return SpawnResp;
		}
		FString ActorPath;
		SpawnResp->TryGetStringField(TEXT("path"), ActorPath);
		if (ActorPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SPAWN_FAILED"),
				TEXT("actor.spawn succeeded but returned no path"));
		}

		// Resolve the spawned actor UObject for the accessor. We use
		// `ResolveObject` (asset-or-world) with the editor world hint —
		// labels and names both resolve; a freshly-spawned actor is
		// trivially findable by path.
		UWorld* WorldHint = nullptr;
		{
			const FWorldContext& EditorWC = GEditor->GetEditorWorldContext(false);
			WorldHint = EditorWC.World();
		}
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ActorPath, WorldHint);
		if (!Resolved.IsOk() || Resolved.Object == nullptr)
		{
			// Shouldn't happen right after a successful spawn. Surface
			// loudly rather than silently destroy.
			return UeMcp::MakeInlineError(TEXT("SPAWN_FAILED"),
				FString::Printf(
					TEXT("spawn_and_set could not resolve freshly-spawned actor '%s'"),
					*ActorPath));
		}
		UObject* SpawnedActor = Resolved.Object;

		// Helper: destroy-by-path rollback. Swallows errors — we're
		// already returning an error; don't shadow it.
		auto RollbackDestroy = [&]()
		{
			TSharedRef<FJsonObject> DestroyArgs = MakeShared<FJsonObject>();
			DestroyArgs->SetStringField(TEXT("actor_path"), ActorPath);
			FString WorldArg;
			if (Args->TryGetStringField(TEXT("world"), WorldArg) && !WorldArg.IsEmpty())
			{
				DestroyArgs->SetStringField(TEXT("world"), WorldArg);
			}
			HandleActorDestroy(DestroyArgs, Cancel);
		};

		// --- Step 2: apply each property through the reflection core.
		TArray<TSharedPtr<FJsonValue>> AppliedList;
		TArray<TSharedPtr<FJsonValue>> FailedList;
		if (PropsObj && *PropsObj)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*PropsObj)->Values)
			{
				const FString& PropPath = Entry.Key;
				const TSharedPtr<FJsonValue>& Value = Entry.Value;

				FUeMcpAccessorErrorInfo AccErr;
				const bool bOk = FUeMcpPropertyAccessor::SetValue(
					SpawnedActor, PropPath, Value, AccErr);
				if (!bOk)
				{
					if (bFailAtomic)
					{
						RollbackDestroy();
						TSharedRef<FJsonObject> InlineErr =
							AccessorErrorToInline(AccErr, PropPath);
						InlineErr->SetStringField(TEXT("failed_step"),
							TEXT("set_property"));
						InlineErr->SetStringField(TEXT("failed_path"), PropPath);
						return InlineErr;
					}
					// Non-atomic: collect into failed_properties.
					TSharedRef<FJsonObject> FailEntry = MakeShared<FJsonObject>();
					FailEntry->SetStringField(TEXT("path"), PropPath);
					FailEntry->SetStringField(TEXT("error"),
						AccessorErrorToInline(AccErr, PropPath)
						->GetStringField(TEXT("error")));
					FailEntry->SetStringField(TEXT("message"), AccErr.Message);
					FailedList.Add(MakeShared<FJsonValueObject>(FailEntry));
				}
				else
				{
					AppliedList.Add(MakeShared<FJsonValueString>(PropPath));
				}
			}
		}

		// --- Step 3: build the composite response. Start from the
		// spawn response so the caller gets the full spawned-actor
		// shape (name, label, class, class_path, path, location, ...,
		// rollback). Layer applied/failed lists on top.
		TSharedRef<FJsonObject> Out = SpawnResp;
		Out->SetArrayField(TEXT("applied_properties"), AppliedList);
		if (FailedList.Num() > 0)
		{
			Out->SetArrayField(TEXT("failed_properties"), FailedList);
		}
		return Out;
	}

	// ------------------------------------------------------------------
	// actor.set_properties — COMPOSITE handler.
	//
	// Batch write of N properties onto an existing actor (or asset).
	// Same semantics as spawn_and_set but without spawning: useful for
	// the observed `get -> set -> get -> set` cleanup pattern at test
	// teardown.
	//
	// Fail-atomic mode (default) captures pre-state for each property
	// BEFORE writing and, on any failure, reverts prior writes so the
	// actor ends up in its pre-call state. `fail_atomic=false` applies
	// whatever it can and reports per-property failures.
	// ------------------------------------------------------------------

	/** `actor.set_properties` handler body. */
	static TSharedRef<FJsonObject> HandleActorSetProperties(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString ObjectId;
		if (!Args->TryGetStringField(TEXT("object"), ObjectId) || ObjectId.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`object` is required and must be a non-empty string"));
		}

		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		Args->TryGetObjectField(TEXT("properties"), PropsObj);
		if (!PropsObj || !*PropsObj || (*PropsObj)->Values.Num() == 0)
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`properties` is required and must be a non-empty object"));
		}

		bool bFailAtomic = true;
		Args->TryGetBoolField(TEXT("fail_atomic"), bFailAtomic);

		// World-scope resolution — reuse the world resolver. If `world`
		// arg is absent, auto-preference mirrors the rest of the
		// actor.* surface (PIE if active, else editor).
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ObjectId, World.World);
		if (!Resolved.IsOk() || Resolved.Object == nullptr)
		{
			return Resolved.ErrorInfo.IsValid()
				? TSharedRef<FJsonObject>(Resolved.ErrorInfo.ToSharedRef())
				: UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
					FString::Printf(TEXT("Could not resolve '%s'"), *ObjectId));
		}
		UObject* Target = Resolved.Object;

		// Pre-read originals for rollback (atomic mode only). We snapshot
		// each property BEFORE any write so a mid-sequence failure can
		// restore. Pre-read failures are tolerated: a property that
		// can't be read probably can't be written either, and the first
		// write attempt will surface the real error.
		struct FSnapshot { FString Path; TSharedPtr<FJsonValue> Original; };
		TArray<FSnapshot> PreState;

		if (bFailAtomic)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*PropsObj)->Values)
			{
				FUeMcpPropertyValue ReadValue;
				FUeMcpAccessorErrorInfo ReadErr;
				if (FUeMcpPropertyAccessor::GetValue(
						Target, Entry.Key, ReadValue, ReadErr))
				{
					FSnapshot Snap;
					Snap.Path = Entry.Key;
					Snap.Original = ReadValue.Json;
					PreState.Add(MoveTemp(Snap));
				}
			}
		}

		// Apply.
		TArray<TSharedPtr<FJsonValue>> AppliedList;
		TArray<TSharedPtr<FJsonValue>> FailedList;
		TArray<FString> AppliedInOrder; // for rollback

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*PropsObj)->Values)
		{
			FUeMcpAccessorErrorInfo AccErr;
			const bool bOk = FUeMcpPropertyAccessor::SetValue(
				Target, Entry.Key, Entry.Value, AccErr);

			if (!bOk)
			{
				if (bFailAtomic)
				{
					// Roll back in reverse order using captured originals.
					for (int32 i = AppliedInOrder.Num() - 1; i >= 0; --i)
					{
						const FString& RevertPath = AppliedInOrder[i];
						// Find the snapshot.
						for (const FSnapshot& Snap : PreState)
						{
							if (Snap.Path == RevertPath && Snap.Original.IsValid())
							{
								FUeMcpAccessorErrorInfo RevErr;
								FUeMcpPropertyAccessor::SetValue(
									Target, RevertPath, Snap.Original, RevErr);
								break;
							}
						}
					}
					TSharedRef<FJsonObject> InlineErr =
						AccessorErrorToInline(AccErr, Entry.Key);
					InlineErr->SetStringField(TEXT("failed_step"),
						TEXT("set_property"));
					InlineErr->SetStringField(TEXT("failed_path"), Entry.Key);
					return InlineErr;
				}
				// Non-atomic: record and continue.
				TSharedRef<FJsonObject> FailEntry = MakeShared<FJsonObject>();
				FailEntry->SetStringField(TEXT("path"), Entry.Key);
				FailEntry->SetStringField(TEXT("error"),
					AccessorErrorToInline(AccErr, Entry.Key)
					->GetStringField(TEXT("error")));
				FailEntry->SetStringField(TEXT("message"), AccErr.Message);
				FailedList.Add(MakeShared<FJsonValueObject>(FailEntry));
			}
			else
			{
				AppliedList.Add(MakeShared<FJsonValueString>(Entry.Key));
				AppliedInOrder.Add(Entry.Key);
			}
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("object"), ObjectId);
		Out->SetArrayField(TEXT("applied"), AppliedList);
		if (FailedList.Num() > 0)
		{
			Out->SetArrayField(TEXT("failed"), FailedList);
		}
		Out->SetNumberField(TEXT("num_applied"), AppliedList.Num());
		return Out;
	}

	// ------------------------------------------------------------------
	// actor.spawn_batch — COMPOSITE handler.
	//
	// Signal-B fusion (issue #49): arena-setup prologues spawn N actors
	// back-to-back. Each `actor.spawn` is one dispatcher hop + one
	// transport round-trip through the one-request-at-a-time plugin
	// socket. This collapses "spawn N actors" into a single game-thread
	// dispatch — the win is N-1 fewer dispatcher hops and N-1 fewer
	// transport round-trips, mattering most on a cold editor where the
	// first-call game-thread budget is contended (cf. ue-api-gotchas §12).
	//
	// Each entry carries its own `class` / `transform` / `label` / `tags`
	// / optional `properties`; `world` (and `properties` cannot be moved
	// to per-entry without ambiguity, so each entry gets the batch-level
	// world unless it overrides via its own `world`). The handler reuses
	// `HandleActorSpawn` per entry (so class-resolution / transform-parse
	// semantics are identical to the singular tool) and the
	// reflection-core property apply used by `actor.spawn_and_set`.
	//
	// `stop_on_error=true` (default) bails on the first failed entry,
	// rolling back already-spawned actors so the batch is all-or-nothing.
	// `stop_on_error=false` collects partial successes; failed entries
	// land in `errors` and successful ones in `spawned`. This mirrors the
	// partial-success reporting of `blueprint.graph.connect_pins_batch`
	// (per-item results, batch `ok` reflects overall outcome).
	//
	// The cancel token is polled between entries — a long batch under
	// load can be cancelled mid-flight, same nice-to-have as the other
	// loop-driven composites.
	// ------------------------------------------------------------------

	/** Apply `properties` to a freshly-spawned actor by path. Returns
	 *  true and fills `OutApplied` on success; on failure (atomic) fills
	 *  `OutError` with an inline-error JSON. Mirrors the property-apply
	 *  half of `HandleActorSpawnAndSet`, factored so the batch can reuse
	 *  it per entry. `bFailAtomic` here means "fail this entry"; the
	 *  caller decides whether an entry failure stops the batch. */
	static bool ApplyEntryProperties(
		const TSharedPtr<FJsonObject>* PropsObj,
		const FString& ActorPath,
		UObject* SpawnedActor,
		bool bFailAtomic,
		TArray<TSharedPtr<FJsonValue>>& OutApplied,
		TArray<TSharedPtr<FJsonValue>>& OutFailed,
		TSharedPtr<FJsonObject>& OutAtomicError)
	{
		OutAtomicError = nullptr;
		if (!PropsObj || !*PropsObj)
		{
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*PropsObj)->Values)
		{
			const FString& PropPath = Entry.Key;
			FUeMcpAccessorErrorInfo AccErr;
			const bool bOk = FUeMcpPropertyAccessor::SetValue(
				SpawnedActor, PropPath, Entry.Value, AccErr);
			if (!bOk)
			{
				if (bFailAtomic)
				{
					TSharedRef<FJsonObject> InlineErr =
						AccessorErrorToInline(AccErr, PropPath);
					InlineErr->SetStringField(TEXT("failed_step"),
						TEXT("set_property"));
					InlineErr->SetStringField(TEXT("failed_path"), PropPath);
					OutAtomicError = InlineErr;
					return false;
				}
				TSharedRef<FJsonObject> FailEntry = MakeShared<FJsonObject>();
				FailEntry->SetStringField(TEXT("path"), PropPath);
				FailEntry->SetStringField(TEXT("error"),
					AccessorErrorToInline(AccErr, PropPath)
					->GetStringField(TEXT("error")));
				FailEntry->SetStringField(TEXT("message"), AccErr.Message);
				OutFailed.Add(MakeShared<FJsonValueObject>(FailEntry));
			}
			else
			{
				OutApplied.Add(MakeShared<FJsonValueString>(PropPath));
			}
		}
		return true;
	}

	/** `actor.spawn_batch` handler body. */
	static TSharedRef<FJsonObject> HandleActorSpawnBatch(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		// `actors` is the per-entry spec list. Required, non-empty —
		// the Python wrapper validates too, but the handler must be
		// safe when called directly (other handlers / tests).
		const TArray<TSharedPtr<FJsonValue>>* ActorsArr = nullptr;
		if (!Args->TryGetArrayField(TEXT("actors"), ActorsArr)
			|| ActorsArr == nullptr || ActorsArr->Num() == 0)
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`actors` is required and must be a non-empty array"));
		}

		bool bStopOnError = true;
		Args->TryGetBoolField(TEXT("stop_on_error"), bStopOnError);

		// Batch-level `world` is the default for every entry; an entry
		// may carry its own `world` to override. We don't resolve the
		// world here — `HandleActorSpawn` resolves per call so a
		// per-entry override Just Works.
		FString BatchWorld;
		const bool bHasBatchWorld =
			Args->TryGetStringField(TEXT("world"), BatchWorld)
			&& !BatchWorld.IsEmpty();

		// Track spawned paths (+ their world arg) for atomic rollback.
		struct FSpawnedRef { FString Path; FString WorldArg; };
		TArray<FSpawnedRef> SpawnedRefs;

		auto RollbackAll = [&]()
		{
			// Destroy in reverse order. Best-effort; we're already
			// returning an error and must not shadow it.
			for (int32 i = SpawnedRefs.Num() - 1; i >= 0; --i)
			{
				TSharedRef<FJsonObject> DestroyArgs = MakeShared<FJsonObject>();
				DestroyArgs->SetStringField(TEXT("actor_path"), SpawnedRefs[i].Path);
				if (!SpawnedRefs[i].WorldArg.IsEmpty())
				{
					DestroyArgs->SetStringField(TEXT("world"), SpawnedRefs[i].WorldArg);
				}
				HandleActorDestroy(DestroyArgs, Cancel);
			}
		};

		TArray<TSharedPtr<FJsonValue>> SpawnedList;
		TArray<TSharedPtr<FJsonValue>> ErrorList;

		for (int32 Idx = 0; Idx < ActorsArr->Num(); ++Idx)
		{
			// Cancellation: bail between entries. Already-spawned actors
			// roll back so a cancelled batch leaves no partial state.
			if (Cancel.IsCancellationRequested())
			{
				RollbackAll();
				return UeMcp::MakeInlineError(TEXT("CANCELLED"),
					FString::Printf(
						TEXT("actor.spawn_batch cancelled after %d/%d spawns"),
						SpawnedList.Num(), ActorsArr->Num()));
			}

			const TSharedPtr<FJsonValue>& EntryVal = (*ActorsArr)[Idx];
			const TSharedPtr<FJsonObject>* EntryObjPtr = nullptr;
			if (!EntryVal.IsValid() || !EntryVal->TryGetObject(EntryObjPtr)
				|| EntryObjPtr == nullptr || !EntryObjPtr->IsValid())
			{
				TSharedRef<FJsonObject> ErrEntry = MakeShared<FJsonObject>();
				ErrEntry->SetNumberField(TEXT("index"), Idx);
				ErrEntry->SetStringField(TEXT("error"), TEXT("SCHEMA_ERROR"));
				ErrEntry->SetStringField(TEXT("message"),
					TEXT("actors[] entry must be an object"));
				if (bStopOnError)
				{
					RollbackAll();
					TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
					Out->SetStringField(TEXT("error"), TEXT("SCHEMA_ERROR"));
					Out->SetStringField(TEXT("message"),
						FString::Printf(
							TEXT("actors[%d] is not an object"), Idx));
					return Out;
				}
				ErrorList.Add(MakeShared<FJsonValueObject>(ErrEntry));
				continue;
			}
			const TSharedPtr<FJsonObject>& EntryObj = *EntryObjPtr;

			// Build a clean spawn-args dict from the entry: class,
			// transform, label, tags. Per-entry `world` wins; else the
			// batch-level world is injected.
			TSharedRef<FJsonObject> SpawnArgs = MakeShared<FJsonObject>();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : EntryObj->Values)
			{
				if (Kv.Key == TEXT("properties"))
				{
					continue;
				}
				SpawnArgs->SetField(Kv.Key, Kv.Value);
			}
			FString EntryWorld;
			const bool bEntryHasWorld =
				EntryObj->TryGetStringField(TEXT("world"), EntryWorld)
				&& !EntryWorld.IsEmpty();
			if (!bEntryHasWorld && bHasBatchWorld)
			{
				SpawnArgs->SetStringField(TEXT("world"), BatchWorld);
			}
			const FString EffectiveWorld =
				bEntryHasWorld ? EntryWorld
				: (bHasBatchWorld ? BatchWorld : FString());

			TSharedRef<FJsonObject> SpawnResp =
				HandleActorSpawn(SpawnArgs, Cancel);
			if (IsInlineError(SpawnResp))
			{
				if (bStopOnError)
				{
					RollbackAll();
					SpawnResp->SetNumberField(TEXT("failed_index"), Idx);
					return SpawnResp;
				}
				TSharedRef<FJsonObject> ErrEntry = MakeShared<FJsonObject>();
				ErrEntry->SetNumberField(TEXT("index"), Idx);
				FString Code, Msg;
				SpawnResp->TryGetStringField(TEXT("error"), Code);
				SpawnResp->TryGetStringField(TEXT("message"), Msg);
				ErrEntry->SetStringField(TEXT("error"), Code);
				ErrEntry->SetStringField(TEXT("message"), Msg);
				ErrorList.Add(MakeShared<FJsonValueObject>(ErrEntry));
				continue;
			}

			FString ActorPath;
			SpawnResp->TryGetStringField(TEXT("path"), ActorPath);
			if (ActorPath.IsEmpty())
			{
				if (bStopOnError)
				{
					RollbackAll();
					return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
						FString::Printf(
							TEXT("actors[%d] spawned but returned no path"), Idx));
				}
				TSharedRef<FJsonObject> ErrEntry = MakeShared<FJsonObject>();
				ErrEntry->SetNumberField(TEXT("index"), Idx);
				ErrEntry->SetStringField(TEXT("error"),
					TEXT("PLUGIN_INTERNAL_ERROR"));
				ErrEntry->SetStringField(TEXT("message"),
					TEXT("spawn returned no path"));
				ErrorList.Add(MakeShared<FJsonValueObject>(ErrEntry));
				continue;
			}

			SpawnedRefs.Add({ActorPath, EffectiveWorld});

			// Optional per-entry property apply (reflection core, same
			// as actor.spawn_and_set). On atomic mode, a property
			// failure also rolls back THIS actor (consistent with the
			// singular tool) plus the rest of the batch.
			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			EntryObj->TryGetObjectField(TEXT("properties"), PropsObj);
			TArray<TSharedPtr<FJsonValue>> AppliedList;
			TArray<TSharedPtr<FJsonValue>> FailedPropsList;
			if (PropsObj && *PropsObj && (*PropsObj)->Values.Num() > 0)
			{
				UWorld* WorldHint = nullptr;
				{
					const FWorldContext& EditorWC =
						GEditor->GetEditorWorldContext(false);
					WorldHint = EditorWC.World();
				}
				UeMcp::FUeMcpResolvedObject Resolved =
					UeMcp::ResolveObject(ActorPath, WorldHint);
				if (!Resolved.IsOk() || Resolved.Object == nullptr)
				{
					if (bStopOnError)
					{
						RollbackAll();
						return UeMcp::MakeInlineError(
							TEXT("PLUGIN_INTERNAL_ERROR"),
							FString::Printf(TEXT("actors[%d]: could not "
								"resolve freshly-spawned actor '%s' for "
								"property apply"), Idx, *ActorPath));
					}
					TSharedRef<FJsonObject> ErrEntry = MakeShared<FJsonObject>();
					ErrEntry->SetNumberField(TEXT("index"), Idx);
					ErrEntry->SetStringField(TEXT("error"),
						TEXT("PLUGIN_INTERNAL_ERROR"));
					ErrEntry->SetStringField(TEXT("message"),
						TEXT("could not resolve spawned actor for property apply"));
					ErrorList.Add(MakeShared<FJsonValueObject>(ErrEntry));
					// The actor exists; keep it in SpawnedList below.
				}
				else
				{
					TSharedPtr<FJsonObject> AtomicErr;
					const bool bPropsOk = ApplyEntryProperties(
						PropsObj, ActorPath, Resolved.Object,
						bStopOnError, AppliedList, FailedPropsList,
						AtomicErr);
					if (!bPropsOk && bStopOnError && AtomicErr.IsValid())
					{
						RollbackAll();
						AtomicErr->SetNumberField(TEXT("failed_index"), Idx);
						return AtomicErr.ToSharedRef();
					}
				}
			}

			// Compact per-item success record. Mirrors the issue's
			// proposed `spawned` entry shape: label / actor_path /
			// transform (+ name/class for parity with actor.spawn).
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("index"), Idx);
			FString SName, SLabel, SClass;
			SpawnResp->TryGetStringField(TEXT("name"), SName);
			SpawnResp->TryGetStringField(TEXT("label"), SLabel);
			SpawnResp->TryGetStringField(TEXT("class"), SClass);
			Item->SetStringField(TEXT("name"), SName);
			Item->SetStringField(TEXT("label"), SLabel);
			Item->SetStringField(TEXT("class"), SClass);
			Item->SetStringField(TEXT("actor_path"), ActorPath);
			if (SpawnResp->HasField(TEXT("location")))
			{
				Item->SetField(TEXT("location"),
					SpawnResp->TryGetField(TEXT("location")));
			}
			if (SpawnResp->HasField(TEXT("rotation_euler_deg")))
			{
				Item->SetField(TEXT("rotation_euler_deg"),
					SpawnResp->TryGetField(TEXT("rotation_euler_deg")));
			}
			if (AppliedList.Num() > 0)
			{
				Item->SetArrayField(TEXT("applied_properties"), AppliedList);
			}
			if (FailedPropsList.Num() > 0)
			{
				Item->SetArrayField(TEXT("failed_properties"), FailedPropsList);
			}
			SpawnedList.Add(MakeShared<FJsonValueObject>(Item));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("spawned"), SpawnedList);
		Data->SetArrayField(TEXT("errors"), ErrorList);
		Data->SetNumberField(TEXT("num_spawned"), SpawnedList.Num());
		Data->SetNumberField(TEXT("num_requested"), ActorsArr->Num());
		Data->SetBoolField(TEXT("ok"), ErrorList.Num() == 0);
		return Data;
	}
}

void UeMcp::RegisterActorHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpActorHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.list"));
		Reg.DefaultTimeoutSeconds = ListDefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorList);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.spawn"));
		Reg.DefaultTimeoutSeconds = SpawnDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorSpawn);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.destroy"));
		Reg.DefaultTimeoutSeconds = DestroyDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorDestroy);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.spawn_and_set"));
		Reg.DefaultTimeoutSeconds = CompositeDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorSpawnAndSet);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.set_properties"));
		Reg.DefaultTimeoutSeconds = CompositeDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorSetProperties);
		Dispatcher.RegisterTool(Reg);
	}

	{
		// actor.spawn_batch: N spawns under one game-thread dispatch.
		// A 50-actor batch at ~5-20ms/spawn fits well inside the
		// 60s write envelope (docs/handler-conventions.md §5); the
		// batch-scoped timeout below gives generous headroom while
		// still bounding a pathological run.
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.spawn_batch"));
		Reg.DefaultTimeoutSeconds = SpawnBatchDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorSpawnBatch);
		Dispatcher.RegisterTool(Reg);
	}
}
