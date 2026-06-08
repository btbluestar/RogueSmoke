// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Fan-out unit 7 — gameplay tag operations.
//
// Read-side: every actor that owns gameplay tags either implements
// `IGameplayTagAssetInterface` (engine-stock contract for static tag
// containers) or carries a `UAbilitySystemComponent` (the GAS plugin's
// live container, mutable via AddLooseGameplayTag/RemoveLooseGameplayTag).
// Most engine systems probe both, in that order, when they care about an
// actor's tags. We follow the same convention — interface first, then
// GAS — for consistency.
//
// Write-side: only GAS supports mutation. The asset interface is
// deliberately read-only (see GameplayTagAssetInterface.h's design
// comment). When neither interface nor GAS is found, both add/remove
// return `INVALID_TARGET` — the caller is asking us to do something we
// have no plumbing for.
//
// GAS detection: the test project the plugin ships with does NOT enable
// GAS. We locate `UAbilitySystemComponent` by class-name lookup at
// runtime (`FindObject<UClass>`) so this translation unit doesn't have
// to link `GameplayAbilities`. When GAS isn't loaded the lookup returns
// nullptr and the handler degrades cleanly: read operations still work
// via the interface path; write operations return `INVALID_TARGET` for
// non-interface targets (interfaces can't mutate either, so the message
// is the same).
//
// Tag parsing: `FGameplayTag::RequestGameplayTag(FName(TagName), false)`
// returns an invalid tag if the name isn't registered. We propagate that
// as `TAG_NOT_FOUND` rather than silently treating an unknown tag as "not
// owned by the actor", which would mask typos.
//
// Threading: every handler is a single game-thread read or write —
// FGameplayTagContainer access is not thread-safe and the tag manager
// holds locks. `check(IsInGameThread())` at entry catches misroutes.

#include "UeMcpTagHandlers.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "GameplayTagAssetInterface.h"
#include "GameplayTagContainer.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpTagHandlersPrivate
{
	/** A single game-thread comparison or mutation; cheap. */
	static constexpr double DefaultTimeoutSeconds = 5.0;

	/**
	 * Lazily resolve `UAbilitySystemComponent`'s UClass by name. Returns
	 * nullptr if GAS is not loaded in this build/project. Cached after the
	 * first lookup; re-resolves on next call if the lookup fails so a
	 * project that loads GAS late (e.g. via plugin reload) becomes mutable
	 * without an editor restart.
	 */
	static UClass* GetAbilitySystemComponentClass()
	{
		static TWeakObjectPtr<UClass> CachedClass;
		if (UClass* Cached = CachedClass.Get())
		{
			return Cached;
		}
		// FindObject so this translation unit doesn't need a link-time
		// dependency on GameplayAbilities. Must be called on the game
		// thread (UObject globals).
		UClass* Found = FindObject<UClass>(
			nullptr, TEXT("/Script/GameplayAbilities.AbilitySystemComponent"));
		if (Found != nullptr)
		{
			CachedClass = Found;
		}
		return Found;
	}

	/**
	 * Find a `UAbilitySystemComponent` on `Actor`. Returns nullptr if GAS
	 * is not loaded or the actor has no ASC. The component pointer's
	 * lifetime tracks the actor's, so callers don't need to retain it
	 * across ticks.
	 */
	static UActorComponent* FindAbilitySystemComponent(AActor* Actor)
	{
		if (Actor == nullptr) return nullptr;
		UClass* AscClass = GetAbilitySystemComponentClass();
		if (AscClass == nullptr) return nullptr;
		return Actor->FindComponentByClass(AscClass);
	}

	/**
	 * Resolve a UFunction on `UAbilitySystemBlueprintLibrary` by name.
	 * Returns nullptr if GAS isn't loaded. Cached per name across calls.
	 *
	 * Why the BP-library wrapper instead of the ASC's own AddLooseGameplay-
	 * Tag: those are `inline void` (no UFUNCTION decoration) in UE 5.7, so
	 * `UAbilitySystemComponent::FindFunction("AddLooseGameplayTag")`
	 * returns nullptr. The BP library's static wrappers ARE UFUNCTION-
	 * exposed, take an `AActor*` rather than the component, and degrade
	 * cleanly to a no-op when the actor has no ASC. Reflective dispatch
	 * keeps this translation unit free of a `GameplayAbilities` link.
	 */
	static UFunction* GetBlueprintLibraryFunction(const TCHAR* FuncName)
	{
		UClass* LibClass = FindObject<UClass>(
			nullptr, TEXT("/Script/GameplayAbilities.AbilitySystemBlueprintLibrary"));
		if (LibClass == nullptr) return nullptr;
		return LibClass->FindFunctionByName(FName(FuncName));
	}

	/** Resolve `Object` to an `AActor*`. Components walk up to their owner. */
	static AActor* ResolveActor(UObject* Object)
	{
		if (Object == nullptr) return nullptr;
		if (AActor* Actor = Cast<AActor>(Object))
		{
			return Actor;
		}
		// Components: hop to owner so callers can name a sub-component
		// path and still hit the owning actor's tag surface.
		if (UActorComponent* Comp = Cast<UActorComponent>(Object))
		{
			return Comp->GetOwner();
		}
		return nullptr;
	}

	/**
	 * Read every gameplay tag the actor exposes — interface tags first,
	 * then live GAS tags appended. The two are merged so callers get a
	 * uniform view; we return both sources in `OutHasInterface` /
	 * `OutHasAsc` so the response can document which path matched.
	 */
	static void GatherActorTags(
		AActor* Actor,
		FGameplayTagContainer& OutTags,
		bool& OutHasInterface,
		bool& OutHasAsc)
	{
		OutTags.Reset();
		OutHasInterface = false;
		OutHasAsc = false;
		if (Actor == nullptr) return;

		// IGameplayTagAssetInterface — engine-stock read API.
		if (IGameplayTagAssetInterface* AssetIface =
				Cast<IGameplayTagAssetInterface>(Actor))
		{
			FGameplayTagContainer Owned;
			AssetIface->GetOwnedGameplayTags(Owned);
			OutTags.AppendTags(Owned);
			OutHasInterface = true;
		}

		// GAS — live tags via reflection-walk. The ASC's runtime tag
		// container is exposed through the interface too (UAbilitySystem-
		// Component implements IGameplayTagAssetInterface), so the
		// interface branch above already covers it when GAS is present.
		// We still flag `bHasAsc` for the response so callers can target
		// add/remove operations.
		if (UActorComponent* Asc = FindAbilitySystemComponent(Actor))
		{
			OutHasAsc = true;
			if (!OutHasInterface)
			{
				// Belt-and-braces: if the ASC didn't surface via the
				// interface cast (older GAS forks, custom subclass), pull
				// its tags via the interface on the component itself.
				if (IGameplayTagAssetInterface* CompIface =
						Cast<IGameplayTagAssetInterface>(Asc))
				{
					FGameplayTagContainer Owned;
					CompIface->GetOwnedGameplayTags(Owned);
					OutTags.AppendTags(Owned);
				}
			}
		}
	}

	/**
	 * Parse the wire-supplied tag string into an `FGameplayTag`. Returns
	 * an invalid tag (caller checks `IsValid()`) when the name isn't
	 * registered with the tag manager — `bErrorIfNotFound=false` keeps
	 * the call silent so the handler can return our own structured error.
	 */
	static FGameplayTag RequestTag(const FString& TagName)
	{
		if (TagName.IsEmpty())
		{
			return FGameplayTag();
		}
		return FGameplayTag::RequestGameplayTag(FName(*TagName), /*bErrorIfNotFound*/ false);
	}

	/** Pull `tag` (required, non-empty string) from the args. */
	static bool GetRequiredTagString(
		const TSharedRef<FJsonObject>& Args, FString& OutTag, FString& OutError)
	{
		if (!Args->TryGetStringField(TEXT("tag"), OutTag) || OutTag.IsEmpty())
		{
			OutError = TEXT("`tag` is required and must be a non-empty string");
			return false;
		}
		return true;
	}

	/** Pull `actor` (required, non-empty string). */
	static bool GetRequiredActorString(
		const TSharedRef<FJsonObject>& Args, FString& OutActor, FString& OutError)
	{
		if (!Args->TryGetStringField(TEXT("actor"), OutActor) || OutActor.IsEmpty())
		{
			OutError = TEXT("`actor` is required and must be a non-empty string");
			return false;
		}
		return true;
	}

	/**
	 * Resolve the world+actor pair, returning either the live actor or an
	 * inline-error JSON object ready to be returned from a handler. On
	 * success, `OutActor` is non-null and the function returns true.
	 */
	static bool ResolveTargetActor(
		const TSharedRef<FJsonObject>& Args,
		const FString& ActorRef,
		AActor*& OutActor,
		TSharedRef<FJsonObject>& OutError)
	{
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			OutError = UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
			return false;
		}
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ActorRef, World.World);
		if (!Resolved.IsOk())
		{
			OutError = Resolved.ErrorInfo.ToSharedRef();
			return false;
		}
		AActor* Actor = ResolveActor(Resolved.Object);
		if (Actor == nullptr)
		{
			OutError = UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("Resolved object '%s' is not an AActor (or component thereof)"),
					*ActorRef));
			return false;
		}
		OutActor = Actor;
		return true;
	}

	/** Append the standard `actor` + `tag` echo fields used by every response. */
	static void EchoIdentity(
		const TSharedRef<FJsonObject>& Out,
		const FString& Actor,
		const FString& Tag)
	{
		Out->SetStringField(TEXT("actor"), Actor);
		Out->SetStringField(TEXT("tag"), Tag);
	}

	/**
	 * Add a flat array of every owned tag's text form to `Out` under
	 * `owned_tags`. Useful for tags.query so the caller can see what
	 * matched (or what was close).
	 */
	static void SetOwnedTagsField(
		const TSharedRef<FJsonObject>& Out, const FGameplayTagContainer& Tags)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(Tags.Num());
		for (const FGameplayTag& T : Tags)
		{
			Arr.Add(MakeShared<FJsonValueString>(T.ToString()));
		}
		Out->SetArrayField(TEXT("owned_tags"), Arr);
	}

	// -----------------------------------------------------------------------
	// tags.has — exact membership check.
	// -----------------------------------------------------------------------
	static TSharedRef<FJsonObject> HandleTagsHas(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString ActorRef, TagName, ParseErr;
		if (!GetRequiredActorString(Args, ActorRef, ParseErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
		}
		if (!GetRequiredTagString(Args, TagName, ParseErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
		}

		AActor* Actor = nullptr;
		TSharedRef<FJsonObject> ResolveErr = MakeShared<FJsonObject>();
		if (!ResolveTargetActor(Args, ActorRef, Actor, ResolveErr))
		{
			return ResolveErr;
		}

		const FGameplayTag QueryTag = RequestTag(TagName);
		if (!QueryTag.IsValid())
		{
			return UeMcp::MakeInlineError(
				TEXT("TAG_NOT_FOUND"),
				FString::Printf(
					TEXT("Tag '%s' is not registered with the gameplay-tag manager"),
					*TagName));
		}

		FGameplayTagContainer Tags;
		bool bHasInterface = false, bHasAsc = false;
		GatherActorTags(Actor, Tags, bHasInterface, bHasAsc);

		if (!bHasInterface && !bHasAsc)
		{
			return UeMcp::MakeInlineError(
				TEXT("INVALID_TARGET"),
				FString::Printf(
					TEXT("Actor '%s' does not implement IGameplayTagAssetInterface "
					     "and has no UAbilitySystemComponent"),
					*ActorRef));
		}

		// Exact membership — no parent walk.
		const bool bHas = Tags.HasTagExact(QueryTag);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		EchoIdentity(Out, ActorRef, TagName);
		Out->SetBoolField(TEXT("has"), bHas);
		Out->SetBoolField(TEXT("via_interface"), bHasInterface);
		Out->SetBoolField(TEXT("via_ability_system"), bHasAsc);
		return Out;
	}

	// -----------------------------------------------------------------------
	// tags.query — exact OR parent match.
	// -----------------------------------------------------------------------
	static TSharedRef<FJsonObject> HandleTagsQuery(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString ActorRef, TagName, ParseErr;
		if (!GetRequiredActorString(Args, ActorRef, ParseErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
		}
		if (!GetRequiredTagString(Args, TagName, ParseErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
		}

		// `exact` is optional and defaults to false (parent-match is the
		// useful semantic for queries — that's why this tool exists separately
		// from tags.has).
		bool bExact = false;
		(void)Args->TryGetBoolField(TEXT("exact"), bExact);

		AActor* Actor = nullptr;
		TSharedRef<FJsonObject> ResolveErr = MakeShared<FJsonObject>();
		if (!ResolveTargetActor(Args, ActorRef, Actor, ResolveErr))
		{
			return ResolveErr;
		}

		const FGameplayTag QueryTag = RequestTag(TagName);
		if (!QueryTag.IsValid())
		{
			return UeMcp::MakeInlineError(
				TEXT("TAG_NOT_FOUND"),
				FString::Printf(
					TEXT("Tag '%s' is not registered with the gameplay-tag manager"),
					*TagName));
		}

		FGameplayTagContainer Tags;
		bool bHasInterface = false, bHasAsc = false;
		GatherActorTags(Actor, Tags, bHasInterface, bHasAsc);

		if (!bHasInterface && !bHasAsc)
		{
			return UeMcp::MakeInlineError(
				TEXT("INVALID_TARGET"),
				FString::Printf(
					TEXT("Actor '%s' does not implement IGameplayTagAssetInterface "
					     "and has no UAbilitySystemComponent"),
					*ActorRef));
		}

		// HasTag walks parents ({"A.1"}.HasTag("A") → true);
		// HasTagExact does not.
		const bool bMatched = bExact ? Tags.HasTagExact(QueryTag) : Tags.HasTag(QueryTag);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		EchoIdentity(Out, ActorRef, TagName);
		Out->SetBoolField(TEXT("matched"), bMatched);
		Out->SetBoolField(TEXT("exact"), bExact);
		Out->SetBoolField(TEXT("via_interface"), bHasInterface);
		Out->SetBoolField(TEXT("via_ability_system"), bHasAsc);
		SetOwnedTagsField(Out, Tags);
		return Out;
	}

	/**
	 * Shared body for `tags.add` and `tags.remove`. Both call the same
	 * shape of UFUNCTION on `UAbilitySystemBlueprintLibrary` —
	 * `(AActor*, FGameplayTagContainer, bool bShouldReplicate)` — they
	 * differ only in the function name and the success-bool field name.
	 */
	// Layout matches `UAbilitySystemBlueprintLibrary::AddLooseGameplayTags`'s
	// UFUNCTION signature: `(AActor*, FGameplayTagContainer, bool) -> bool`.
	// ProcessEvent appends the return value at the end of the param block.
	struct FMutateLooseTagsParams
	{
		AActor* Actor = nullptr;
		FGameplayTagContainer Tags;
		bool bShouldReplicate = false;
		bool ReturnValue = false;
	};

	static TSharedRef<FJsonObject> MutateLooseTag(
		const TSharedRef<FJsonObject>& Args,
		const TCHAR* BpFuncName,
		const TCHAR* OutcomeFieldName,
		const TCHAR* InverseToolName)
	{
		FString ActorRef, TagName, ParseErr;
		if (!GetRequiredActorString(Args, ActorRef, ParseErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
		}
		if (!GetRequiredTagString(Args, TagName, ParseErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), ParseErr);
		}

		AActor* Actor = nullptr;
		TSharedRef<FJsonObject> ResolveErr = MakeShared<FJsonObject>();
		if (!ResolveTargetActor(Args, ActorRef, Actor, ResolveErr))
		{
			return ResolveErr;
		}

		const FGameplayTag QueryTag = RequestTag(TagName);
		if (!QueryTag.IsValid())
		{
			return UeMcp::MakeInlineError(
				TEXT("TAG_NOT_FOUND"),
				FString::Printf(
					TEXT("Tag '%s' is not registered with the gameplay-tag manager"),
					*TagName));
		}

		UActorComponent* Asc = FindAbilitySystemComponent(Actor);
		if (Asc == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("INVALID_TARGET"),
				FString::Printf(
					TEXT("Actor '%s' has no UAbilitySystemComponent — "
					     "tag mutation requires GAS (loose-tag API)"),
					*ActorRef));
		}

		bool bExisted = false;
		if (IGameplayTagAssetInterface* Iface = Cast<IGameplayTagAssetInterface>(Asc))
		{
			FGameplayTagContainer Owned;
			Iface->GetOwnedGameplayTags(Owned);
			bExisted = Owned.HasTagExact(QueryTag);
		}

		UFunction* BpFunc = GetBlueprintLibraryFunction(BpFuncName);
		if (BpFunc == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(
					TEXT("UAbilitySystemBlueprintLibrary::%s UFunction not found "
					     "(GAS not loaded or API drift)."),
					BpFuncName));
		}

		// CDO is the canonical receiver for static UFUNCTIONs.
		UObject* Receiver = BpFunc->GetOuterUClass()->GetDefaultObject();
		FMutateLooseTagsParams Params;
		Params.Actor = Actor;
		Params.Tags.AddTag(QueryTag);
		Receiver->ProcessEvent(BpFunc, &Params);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		EchoIdentity(Out, ActorRef, TagName);
		Out->SetBoolField(OutcomeFieldName, Params.ReturnValue);
		Out->SetBoolField(TEXT("existed"), bExisted);

		// Rollback hint — only when the call actually changed world state,
		// so the replay is a true inverse and not a redundant no-op. We are
		// the add op when our inverse is `tags.remove`; state then changed
		// iff the tag was NOT already present (!bExisted). We are the remove
		// op when our inverse is `tags.add`; state changed iff it WAS
		// present (bExisted). The inverse tool takes the same natural keys
		// (`actor` + `tag`), both stable across the rollback lifetime. See
		// docs/handler-conventions.md §4 and the canonical pattern in
		// UeMcpActorHandlers.cpp (actor.spawn → actor.destroy).
		const bool bIsAddOp = InverseToolName != nullptr &&
			FCString::Strcmp(InverseToolName, TEXT("tags.remove")) == 0;
		const bool bStateChanged = bIsAddOp ? !bExisted : bExisted;
		if (InverseToolName != nullptr && bStateChanged)
		{
			TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
			Rollback->SetStringField(TEXT("tool"), InverseToolName);
			TSharedRef<FJsonObject> RollbackArgs = MakeShared<FJsonObject>();
			RollbackArgs->SetStringField(TEXT("actor"), ActorRef);
			RollbackArgs->SetStringField(TEXT("tag"), TagName);
			Rollback->SetObjectField(TEXT("args"), RollbackArgs);
			Out->SetObjectField(TEXT("rollback"), Rollback);
		}
		return Out;
	}

	static TSharedRef<FJsonObject> HandleTagsAdd(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		return MutateLooseTag(
			Args, TEXT("AddLooseGameplayTags"), TEXT("added"), TEXT("tags.remove"));
	}

	static TSharedRef<FJsonObject> HandleTagsRemove(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		return MutateLooseTag(
			Args, TEXT("RemoveLooseGameplayTags"), TEXT("removed"), TEXT("tags.add"));
	}
}

void UeMcp::RegisterTagHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpTagHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tags.has"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTagsHas);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tags.query"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTagsQuery);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tags.add"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTagsAdd);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("tags.remove"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTagsRemove);
		Dispatcher.RegisterTool(Reg);
	}
}
