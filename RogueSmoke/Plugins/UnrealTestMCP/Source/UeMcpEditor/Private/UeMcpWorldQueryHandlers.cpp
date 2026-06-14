// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// World-spatial-query handlers (fan-out agent 6).
//
// Two synchronous queries against the resolved world's physics scene.
// Both run inline on the game thread under the dispatcher's executor — no
// pending-handler dance needed because a line trace / sphere overlap is
// a single physics-tick read with no waiting semantics.
//
// Channel parsing: callers pass a string like `"Visibility"` or
// `"WorldStatic"`; the table below maps to `ECollisionChannel`. Game-
// channel slots (`GameTraceChannel1..18`, `EngineTraceChannel1..6`) are
// also accepted by exact name so projects that #define their own
// COLLISION_X macros can still trace by name. We deliberately do NOT
// expose `Profile` arg in v0 — reasoning is in the line-trace handler
// header comment.

#include "UeMcpWorldQueryHandlers.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/HitResult.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicalMaterials/PhysicalMaterial.h"

#include "UeMcpDispatcher.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpWorldQueryHandlersPrivate
{
	/** Per-tool dispatcher timeout. A line trace + sphere overlap on an
	 *  arena-sized scene takes microseconds; 10s is generous. */
	static constexpr double DefaultTimeoutSeconds = 10.0;

	/** Hard cap on sphere radius. 100m is plenty for gameplay queries and
	 *  protects against accidental scene-wide overlaps that would dump a
	 *  multi-megabyte JSON response. */
	static constexpr float MaxSphereRadius = 10000.0f; // 100 m in cm

	/**
	 * Map a wire string (e.g. `"Visibility"`, `"ECC_Pawn"`,
	 * `"GameTraceChannel1"`) to an `ECollisionChannel`. Case-insensitive.
	 * Returns false on miss — caller surfaces a SCHEMA_ERROR with the
	 * accepted-names list.
	 */
	static bool ParseCollisionChannel(const FString& Name, ECollisionChannel& OutChannel)
	{
		const FString Lower = Name.ToLower();

		// Strip optional "ecc_" prefix so callers can pass either spelling.
		const FString Stripped = Lower.StartsWith(TEXT("ecc_"))
			? Lower.RightChop(4)
			: Lower;

		struct FNameToChannel
		{
			const TCHAR* Name;
			ECollisionChannel Channel;
		};
		static const FNameToChannel Map[] =
		{
			{ TEXT("worldstatic"),         ECC_WorldStatic },
			{ TEXT("worlddynamic"),        ECC_WorldDynamic },
			{ TEXT("pawn"),                ECC_Pawn },
			{ TEXT("visibility"),          ECC_Visibility },
			{ TEXT("camera"),              ECC_Camera },
			{ TEXT("physicsbody"),         ECC_PhysicsBody },
			{ TEXT("vehicle"),             ECC_Vehicle },
			{ TEXT("destructible"),        ECC_Destructible },
			{ TEXT("enginetracechannel1"), ECC_EngineTraceChannel1 },
			{ TEXT("enginetracechannel2"), ECC_EngineTraceChannel2 },
			{ TEXT("enginetracechannel3"), ECC_EngineTraceChannel3 },
			{ TEXT("enginetracechannel4"), ECC_EngineTraceChannel4 },
			{ TEXT("enginetracechannel5"), ECC_EngineTraceChannel5 },
			{ TEXT("enginetracechannel6"), ECC_EngineTraceChannel6 },
			{ TEXT("gametracechannel1"),   ECC_GameTraceChannel1 },
			{ TEXT("gametracechannel2"),   ECC_GameTraceChannel2 },
			{ TEXT("gametracechannel3"),   ECC_GameTraceChannel3 },
			{ TEXT("gametracechannel4"),   ECC_GameTraceChannel4 },
			{ TEXT("gametracechannel5"),   ECC_GameTraceChannel5 },
			{ TEXT("gametracechannel6"),   ECC_GameTraceChannel6 },
			{ TEXT("gametracechannel7"),   ECC_GameTraceChannel7 },
			{ TEXT("gametracechannel8"),   ECC_GameTraceChannel8 },
			{ TEXT("gametracechannel9"),   ECC_GameTraceChannel9 },
			{ TEXT("gametracechannel10"),  ECC_GameTraceChannel10 },
			{ TEXT("gametracechannel11"),  ECC_GameTraceChannel11 },
			{ TEXT("gametracechannel12"),  ECC_GameTraceChannel12 },
			{ TEXT("gametracechannel13"),  ECC_GameTraceChannel13 },
			{ TEXT("gametracechannel14"),  ECC_GameTraceChannel14 },
			{ TEXT("gametracechannel15"),  ECC_GameTraceChannel15 },
			{ TEXT("gametracechannel16"),  ECC_GameTraceChannel16 },
			{ TEXT("gametracechannel17"),  ECC_GameTraceChannel17 },
			{ TEXT("gametracechannel18"),  ECC_GameTraceChannel18 },
		};

		for (const FNameToChannel& Entry : Map)
		{
			if (Stripped.Equals(Entry.Name))
			{
				OutChannel = Entry.Channel;
				return true;
			}
		}
		return false;
	}

	/** Read a 3-element number array out of a JSON value into an FVector.
	 *  Accepts only `[x, y, z]` shape — refuses object form to keep the
	 *  schema unambiguous. */
	static bool ParseVector(
		const TSharedPtr<FJsonValue>& V, FVector& Out, FString& OutError)
	{
		if (!V.IsValid() || V->Type != EJson::Array)
		{
			OutError = TEXT("must be a 3-element [x, y, z] number array");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Arr = V->AsArray();
		if (Arr.Num() != 3)
		{
			OutError = FString::Printf(
				TEXT("must have 3 elements, got %d"), Arr.Num());
			return false;
		}
		double Vals[3] = { 0.0, 0.0, 0.0 };
		for (int32 i = 0; i < 3; ++i)
		{
			if (!Arr[i].IsValid() || !Arr[i]->TryGetNumber(Vals[i]))
			{
				OutError = FString::Printf(
					TEXT("element %d must be a number"), i);
				return false;
			}
		}
		Out = FVector(Vals[0], Vals[1], Vals[2]);
		return true;
	}

	/** Build a JSON `[x, y, z]` from an `FVector`. */
	static TSharedRef<FJsonValueArray> VectorToJson(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(3);
		Out.Add(MakeShared<FJsonValueNumber>(V.X));
		Out.Add(MakeShared<FJsonValueNumber>(V.Y));
		Out.Add(MakeShared<FJsonValueNumber>(V.Z));
		return MakeShared<FJsonValueArray>(Out);
	}

	/**
	 * Apply the optional `ignore_actors` arg to a `FCollisionQueryParams`.
	 * Each entry is resolved through `ResolveObject`; resolvable actors
	 * are added as ignored, unresolved entries become a warning string in
	 * `OutWarnings` (NOT a hard failure — tests routinely pass labels
	 * that haven't spawned yet).
	 */
	static void ApplyIgnoreActors(
		const TSharedRef<FJsonObject>& Args, UWorld* World,
		FCollisionQueryParams& Params, TArray<FString>& OutWarnings)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Args->TryGetArrayField(TEXT("ignore_actors"), Arr) || Arr == nullptr)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			if (!V.IsValid() || V->Type != EJson::String)
			{
				continue;
			}
			const FString Id = V->AsString();
			if (Id.IsEmpty())
			{
				continue;
			}
			UeMcp::FUeMcpResolvedObject Resolved = UeMcp::ResolveObject(Id, World);
			if (!Resolved.IsOk())
			{
				OutWarnings.Add(FString::Printf(
					TEXT("ignore_actors: '%s' not resolvable; skipped"), *Id));
				continue;
			}
			if (AActor* Actor = Cast<AActor>(Resolved.Object))
			{
				Params.AddIgnoredActor(Actor);
			}
			else
			{
				OutWarnings.Add(FString::Printf(
					TEXT("ignore_actors: '%s' resolved to a non-actor (%s); skipped"),
					*Id, *Resolved.Object->GetClass()->GetName()));
			}
		}
	}

	/** Append a warnings array to a payload when non-empty. */
	static void AttachWarnings(
		const TSharedRef<FJsonObject>& Out, const TArray<FString>& Warnings)
	{
		if (Warnings.Num() == 0) { return; }
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Warnings.Num());
		for (const FString& W : Warnings)
		{
			Arr.Add(MakeShared<FJsonValueString>(W));
		}
		Out->SetArrayField(TEXT("warnings"), Arr);
	}

	/** Read optional bool field. */
	static bool ReadOptionalBool(
		const TSharedRef<FJsonObject>& Args, const TCHAR* Field, bool Default)
	{
		bool Value = Default;
		Args->TryGetBoolField(Field, Value);
		return Value;
	}

	/**
	 * `world.line_trace` body. Single read on the game thread; no waiting.
	 *
	 * Wire args:
	 *   - `start`           required, `[x, y, z]`
	 *   - `end`             required, `[x, y, z]`
	 *   - `channel`         optional string (default `"Visibility"`)
	 *   - `trace_complex`   optional bool (default false)
	 *   - `ignore_actors`   optional array of object ids
	 *   - `world`           optional `"auto" | "pie" | "editor"`
	 */
	static TSharedRef<FJsonObject> HandleLineTrace(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		// --- Required vector args ---
		FVector Start, End;
		FString VecErr;
		if (!ParseVector(Args->TryGetField(TEXT("start")), Start, VecErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`start` %s"), *VecErr));
		}
		if (!ParseVector(Args->TryGetField(TEXT("end")), End, VecErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`end` %s"), *VecErr));
		}

		// --- Channel ---
		ECollisionChannel Channel = ECC_Visibility;
		FString ChannelName;
		if (Args->TryGetStringField(TEXT("channel"), ChannelName)
			&& !ChannelName.IsEmpty())
		{
			if (!ParseCollisionChannel(ChannelName, Channel))
			{
				return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("`channel` '%s' is not a recognised ECollisionChannel; "
							 "expected one of WorldStatic|WorldDynamic|Pawn|Visibility|"
							 "Camera|PhysicsBody|Vehicle|Destructible|"
							 "GameTraceChannel1..18|EngineTraceChannel1..6"),
						*ChannelName));
			}
		}

		const bool bTraceComplex = ReadOptionalBool(Args, TEXT("trace_complex"), false);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(UeMcpLineTrace), bTraceComplex);
		Params.bReturnPhysicalMaterial = true;

		TArray<FString> Warnings;
		ApplyIgnoreActors(Args, World.World, Params, Warnings);

		// --- Trace ---
		FHitResult Hit;
		const bool bHit = World.World->LineTraceSingleByChannel(
			Hit, Start, End, Channel, Params);

		// --- Build response ---
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("hit"), bHit);
		Out->SetField(TEXT("start"), VectorToJson(Start));
		Out->SetField(TEXT("end"), VectorToJson(End));
		Out->SetStringField(TEXT("channel"), ChannelName.IsEmpty() ? TEXT("Visibility") : ChannelName);
		Out->SetStringField(TEXT("world_scope"), UeMcp::WorldScopeToString(World.ResolvedScope));

		if (bHit)
		{
			Out->SetField(TEXT("impact_point"),  VectorToJson(Hit.ImpactPoint));
			Out->SetField(TEXT("impact_normal"), VectorToJson(Hit.ImpactNormal));
			Out->SetField(TEXT("location"),      VectorToJson(Hit.Location));
			Out->SetNumberField(TEXT("distance"), Hit.Distance);
			Out->SetNumberField(TEXT("time"),     Hit.Time);

			if (AActor* HitActor = Hit.GetActor())
			{
				Out->SetStringField(TEXT("actor_path"),  HitActor->GetPathName());
				Out->SetStringField(TEXT("actor_label"), HitActor->GetActorLabel());
				Out->SetStringField(TEXT("actor_class"), HitActor->GetClass()->GetPathName());
			}
			if (UPrimitiveComponent* HitComp = Hit.GetComponent())
			{
				Out->SetStringField(TEXT("component_path"),  HitComp->GetPathName());
				Out->SetStringField(TEXT("component_class"), HitComp->GetClass()->GetPathName());
			}
			if (!Hit.BoneName.IsNone())
			{
				Out->SetStringField(TEXT("bone_name"), Hit.BoneName.ToString());
			}
			if (UPhysicalMaterial* PhysMat = Hit.PhysMaterial.Get())
			{
				Out->SetStringField(TEXT("phys_material"), PhysMat->GetPathName());
			}
		}

		AttachWarnings(Out, Warnings);
		return Out;
	}

	/**
	 * `world.sphere_overlap` body. Single read; returns deduplicated
	 * actor records.
	 *
	 * Wire args:
	 *   - `center`        required, `[x, y, z]`
	 *   - `radius`        required, > 0, <= 10000 cm
	 *   - `channel`       optional string (default `"Pawn"` — matches the
	 *                     "what's nearby" intent better than Visibility)
	 *   - `ignore_actors` optional array of object ids
	 *   - `world`         optional `"auto" | "pie" | "editor"`
	 */
	static TSharedRef<FJsonObject> HandleSphereOverlap(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		FVector Center;
		FString VecErr;
		if (!ParseVector(Args->TryGetField(TEXT("center")), Center, VecErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`center` %s"), *VecErr));
		}

		double Radius = 0.0;
		if (!Args->TryGetNumberField(TEXT("radius"), Radius))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`radius` is required and must be a number"));
		}
		if (Radius <= 0.0)
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`radius` must be > 0 (got %g)"), Radius));
		}
		if (Radius > MaxSphereRadius)
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`radius` must be <= %g cm (got %g)"),
					MaxSphereRadius, Radius));
		}

		ECollisionChannel Channel = ECC_Pawn;
		FString ChannelName;
		if (Args->TryGetStringField(TEXT("channel"), ChannelName)
			&& !ChannelName.IsEmpty())
		{
			if (!ParseCollisionChannel(ChannelName, Channel))
			{
				return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("`channel` '%s' is not a recognised ECollisionChannel"),
						*ChannelName));
			}
		}

		FCollisionQueryParams Params(SCENE_QUERY_STAT(UeMcpSphereOverlap));
		TArray<FString> Warnings;
		ApplyIgnoreActors(Args, World.World, Params, Warnings);

		// --- Overlap ---
		TArray<FOverlapResult> Overlaps;
		const bool bAny = World.World->OverlapMultiByChannel(
			Overlaps, Center, FQuat::Identity, Channel,
			FCollisionShape::MakeSphere(static_cast<float>(Radius)), Params);

		// --- Dedupe overlaps to actor records, count component overlaps ---
		TMap<AActor*, int32> ActorComponentHits;
		ActorComponentHits.Reserve(Overlaps.Num());
		for (const FOverlapResult& Result : Overlaps)
		{
			if (AActor* Actor = Result.GetActor())
			{
				int32& Count = ActorComponentHits.FindOrAdd(Actor);
				++Count;
			}
		}

		TArray<TSharedPtr<FJsonValue>> ActorJson;
		ActorJson.Reserve(ActorComponentHits.Num());
		for (const TPair<AActor*, int32>& Pair : ActorComponentHits)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("actor_path"),  Pair.Key->GetPathName());
			Entry->SetStringField(TEXT("actor_label"), Pair.Key->GetActorLabel());
			Entry->SetStringField(TEXT("actor_class"), Pair.Key->GetClass()->GetPathName());
			Entry->SetNumberField(TEXT("component_hits"), Pair.Value);
			ActorJson.Add(MakeShared<FJsonValueObject>(Entry));
		}

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("any_hit"), bAny);
		Out->SetField(TEXT("center"), VectorToJson(Center));
		Out->SetNumberField(TEXT("radius"), Radius);
		Out->SetStringField(TEXT("channel"), ChannelName.IsEmpty() ? TEXT("Pawn") : ChannelName);
		Out->SetStringField(TEXT("world_scope"), UeMcp::WorldScopeToString(World.ResolvedScope));
		Out->SetNumberField(TEXT("overlap_count"), Overlaps.Num());
		Out->SetNumberField(TEXT("actor_count"), ActorJson.Num());
		Out->SetArrayField(TEXT("actors"), ActorJson);

		AttachWarnings(Out, Warnings);
		return Out;
	}
}

void UeMcp::RegisterWorldQueryHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpWorldQueryHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("world.line_trace"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleLineTrace);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("world.sphere_overlap"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSphereOverlap);
		Dispatcher.RegisterTool(Reg);
	}
}
