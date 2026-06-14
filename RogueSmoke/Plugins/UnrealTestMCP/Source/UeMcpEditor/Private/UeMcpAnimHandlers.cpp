// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// `anim.play_montage` / `anim.stop_montage` / `anim.get_active_section`.
//
// Each handler resolves a target actor via the shared `ResolveObject`
// chain, locates a `USkeletalMeshComponent` on it (by name when the
// caller supplied `component`, otherwise the first one), and calls
// through to the component's `UAnimInstance`. Anim instance is NOT
// guaranteed — meshes without an anim BP set return null from
// `GetAnimInstance()`, and we surface that as `NOT_FOUND` so tests can
// distinguish "wrong actor" from "actor exists but has no anim graph".
//
// Montage assets are loaded via `LoadObject<UAnimMontage>`. Repeated
// calls with the same path are cheap — the asset registry caches the
// load. We do not unload after stopping; the montage lives in memory
// for the editor session, same as anything else loaded by reflection.

#include "UeMcpAnimHandlers.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "UeMcpDispatcher.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpAnimHandlersPrivate
{
	/** Default dispatcher timeouts — single game-thread API call each. */
	static constexpr double DefaultTimeoutSeconds = 10.0;

	/**
	 * Locate a `USkeletalMeshComponent` on `Actor` by name. When `Name`
	 * is empty, returns the first skeletal mesh component on the actor.
	 * Returns nullptr when no match.
	 *
	 * Name match order:
	 *   1. exact `GetName()` (e.g. `Mesh`, `CharacterMesh0`)
	 *   2. exact `GetFName()` lookup
	 * Both case-insensitive — components in BP graphs and SCS use
	 * mixed-case names so we don't want callers second-guessing casing.
	 */
	static USkeletalMeshComponent* FindSkeletalMeshComponent(
		AActor* Actor, const FString& Name)
	{
		if (Actor == nullptr)
		{
			return nullptr;
		}

		TInlineComponentArray<USkeletalMeshComponent*> Components(Actor);
		if (Components.Num() == 0)
		{
			return nullptr;
		}

		if (Name.IsEmpty())
		{
			return Components[0];
		}

		for (USkeletalMeshComponent* Comp : Components)
		{
			if (Comp == nullptr)
			{
				continue;
			}
			if (Comp->GetName().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Comp;
			}
			if (Comp->GetFName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/**
	 * Resolve the (actor, skeletal-mesh-component, anim-instance) triple
	 * the three handlers all need. On failure, `OutError` carries the
	 * inline-error JSON and the function returns false. All three handlers
	 * require an anim instance — without one there is nothing to drive or
	 * report — so a missing anim instance is treated as NOT_FOUND.
	 */
	struct FResolvedTarget
	{
		AActor* Actor = nullptr;
		USkeletalMeshComponent* Component = nullptr;
		UAnimInstance* AnimInstance = nullptr;
		FString ResolvedComponentName;
	};

	static bool ResolveTarget(
		const TSharedRef<FJsonObject>& Args,
		FResolvedTarget& OutTarget,
		TSharedPtr<FJsonObject>& OutError)
	{
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			OutError = UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
			return false;
		}

		FString ActorRef;
		if (!Args->TryGetStringField(TEXT("actor"), ActorRef) || ActorRef.IsEmpty())
		{
			OutError = UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`actor` is required and must be a non-empty string"));
			return false;
		}

		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ActorRef, World.World);
		if (!Resolved.IsOk())
		{
			OutError = Resolved.ErrorInfo;
			return false;
		}

		AActor* Actor = Cast<AActor>(Resolved.Object);
		if (Actor == nullptr)
		{
			OutError = UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("`actor`='%s' resolved to a non-actor (%s)"),
					*ActorRef,
					Resolved.Object ? *Resolved.Object->GetClass()->GetName()
					                : TEXT("null")));
			return false;
		}

		FString ComponentName;
		Args->TryGetStringField(TEXT("component"), ComponentName);

		USkeletalMeshComponent* Comp = FindSkeletalMeshComponent(Actor, ComponentName);
		if (Comp == nullptr)
		{
			OutError = UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				ComponentName.IsEmpty()
					? FString::Printf(
						TEXT("Actor '%s' has no USkeletalMeshComponent"),
						*Actor->GetName())
					: FString::Printf(
						TEXT("USkeletalMeshComponent '%s' not found on actor '%s'"),
						*ComponentName, *Actor->GetName()));
			return false;
		}

		UAnimInstance* AnimInstance = Comp->GetAnimInstance();
		if (AnimInstance == nullptr)
		{
			OutError = UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("USkeletalMeshComponent '%s' on actor '%s' has no UAnimInstance "
						 "(no anim BP / AnimationMode != AnimationBlueprint?)"),
					*Comp->GetName(), *Actor->GetName()));
			return false;
		}

		OutTarget.Actor = Actor;
		OutTarget.Component = Comp;
		OutTarget.AnimInstance = AnimInstance;
		OutTarget.ResolvedComponentName = Comp->GetName();
		return true;
	}

	/** Load a montage asset by path. Returns nullptr on miss. */
	static UAnimMontage* LoadMontage(const FString& MontagePath)
	{
		if (MontagePath.IsEmpty())
		{
			return nullptr;
		}
		// LoadObject hits the asset registry; subsequent calls with the
		// same path are cached by the engine's UObject system.
		return LoadObject<UAnimMontage>(nullptr, *MontagePath);
	}

	/**
	 * Resolve an optional `montage` arg. Three outcomes:
	 *   - field absent   → returns true, OutMontage=nullptr.
	 *   - field present, load succeeds → returns true, OutMontage set.
	 *   - field present, load fails    → returns false, OutError set.
	 */
	static bool ResolveOptionalMontage(
		const TSharedRef<FJsonObject>& Args,
		UAnimMontage*& OutMontage,
		FString& OutMontagePath,
		TSharedPtr<FJsonObject>& OutError)
	{
		OutMontage = nullptr;
		OutMontagePath.Reset();
		if (!Args->TryGetStringField(TEXT("montage"), OutMontagePath)
			|| OutMontagePath.IsEmpty())
		{
			return true;
		}
		OutMontage = LoadMontage(OutMontagePath);
		if (OutMontage == nullptr)
		{
			OutError = UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("Failed to LoadObject<UAnimMontage>('%s')"),
					*OutMontagePath));
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------
	// anim.play_montage
	// ------------------------------------------------------------------

	static TSharedRef<FJsonObject> HandlePlayMontage(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FResolvedTarget Target;
		TSharedPtr<FJsonObject> Error;
		if (!ResolveTarget(Args, Target, Error))
		{
			return Error.ToSharedRef();
		}

		FString MontagePath;
		if (!Args->TryGetStringField(TEXT("montage"), MontagePath) || MontagePath.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`montage` is required and must be a non-empty asset path string"));
		}

		UAnimMontage* Montage = LoadMontage(MontagePath);
		if (Montage == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("Failed to LoadObject<UAnimMontage>('%s') — asset missing or wrong type"),
					*MontagePath));
		}

		double PlayRate = 1.0;
		(void)Args->TryGetNumberField(TEXT("play_rate"), PlayRate);
		if (PlayRate <= 0.0)
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`play_rate` must be > 0 (got %g)"), PlayRate));
		}

		FString StartSection;
		Args->TryGetStringField(TEXT("start_section"), StartSection);

		// Montage_Play returns the play length in seconds (0 on fail).
		const float Length = Target.AnimInstance->Montage_Play(
			Montage, static_cast<float>(PlayRate));
		const bool bStarted = Length > 0.f;

		if (bStarted && !StartSection.IsEmpty())
		{
			Target.AnimInstance->Montage_JumpToSection(FName(*StartSection), Montage);
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("playing"), bStarted);
		Data->SetNumberField(TEXT("length"), Length);
		Data->SetNumberField(TEXT("play_rate"), PlayRate);
		Data->SetStringField(TEXT("montage"), MontagePath);
		Data->SetStringField(TEXT("actor"), Target.Actor->GetName());
		Data->SetStringField(TEXT("component"), Target.ResolvedComponentName);
		if (!StartSection.IsEmpty())
		{
			Data->SetStringField(TEXT("start_section"), StartSection);
		}
		return Data;
	}

	// ------------------------------------------------------------------
	// anim.stop_montage
	// ------------------------------------------------------------------

	static TSharedRef<FJsonObject> HandleStopMontage(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FResolvedTarget Target;
		TSharedPtr<FJsonObject> Error;
		if (!ResolveTarget(Args, Target, Error))
		{
			return Error.ToSharedRef();
		}

		// Optional named montage — when omitted, stops every active montage
		// on the anim instance.
		UAnimMontage* SpecificMontage = nullptr;
		FString MontagePath;
		if (!ResolveOptionalMontage(Args, SpecificMontage, MontagePath, Error))
		{
			return Error.ToSharedRef();
		}

		double BlendOutTime = -1.0; // sentinel — use montage default
		(void)Args->TryGetNumberField(TEXT("blend_out_time"), BlendOutTime);

		// `Montage_Stop` semantics: when `InBlendOutTime < 0`, the engine
		// uses the montage's default BlendOut time. We keep that affordance.
		const float ResolvedBlend = BlendOutTime < 0.0
			? (SpecificMontage != nullptr
				? SpecificMontage->BlendOut.GetBlendTime()
				: 0.f)
			: static_cast<float>(BlendOutTime);

		Target.AnimInstance->Montage_Stop(ResolvedBlend, SpecificMontage);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("stopped"), true);
		Data->SetNumberField(TEXT("blend_out_time"), ResolvedBlend);
		Data->SetStringField(TEXT("actor"), Target.Actor->GetName());
		Data->SetStringField(TEXT("component"), Target.ResolvedComponentName);
		if (SpecificMontage != nullptr)
		{
			Data->SetStringField(TEXT("montage"), MontagePath);
		}
		return Data;
	}

	// ------------------------------------------------------------------
	// anim.get_active_section
	// ------------------------------------------------------------------

	static TSharedRef<FJsonObject> HandleGetActiveSection(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FResolvedTarget Target;
		TSharedPtr<FJsonObject> Error;
		if (!ResolveTarget(Args, Target, Error))
		{
			return Error.ToSharedRef();
		}

		// Optional named montage. When absent, the engine queries the first
		// active montage. Either way we report whatever the anim instance
		// considers the active state for that query.
		UAnimMontage* SpecificMontage = nullptr;
		FString MontagePath;
		if (!ResolveOptionalMontage(Args, SpecificMontage, MontagePath, Error))
		{
			return Error.ToSharedRef();
		}

		const FName Section =
			Target.AnimInstance->Montage_GetCurrentSection(SpecificMontage);
		const bool bIsPlaying = SpecificMontage != nullptr
			? Target.AnimInstance->Montage_IsPlaying(SpecificMontage)
			: Target.AnimInstance->IsAnyMontagePlaying();

		// `Montage_GetPosition` requires a non-null montage. When the
		// caller didn't name one, derive the active montage from the
		// anim instance and query against that — otherwise position is
		// reported as null.
		UAnimMontage* MontageForPosition = SpecificMontage;
		if (MontageForPosition == nullptr)
		{
			MontageForPosition = Target.AnimInstance->GetCurrentActiveMontage();
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("is_playing"), bIsPlaying);
		Data->SetStringField(TEXT("actor"), Target.Actor->GetName());
		Data->SetStringField(TEXT("component"), Target.ResolvedComponentName);

		if (Section.IsNone())
		{
			Data->SetField(TEXT("section"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Data->SetStringField(TEXT("section"), Section.ToString());
		}

		if (MontageForPosition != nullptr)
		{
			const float Position = Target.AnimInstance->Montage_GetPosition(MontageForPosition);
			Data->SetNumberField(TEXT("position"), Position);
			Data->SetStringField(TEXT("montage"), MontageForPosition->GetPathName());
			Data->SetNumberField(TEXT("length"), MontageForPosition->GetPlayLength());

			// Position-within-section: the section the position currently
			// falls inside might NOT be the engine's "current" section
			// (during blend-outs, etc.) — use the time-to-section APIs to
			// derive a stable answer.
			float SectionStart = 0.f;
			float SectionEnd = 0.f;
			const int32 SectionIndex = MontageForPosition->GetSectionIndex(Section);
			if (SectionIndex != INDEX_NONE)
			{
				MontageForPosition->GetSectionStartAndEndTime(
					SectionIndex, SectionStart, SectionEnd);
				Data->SetNumberField(TEXT("section_start"), SectionStart);
				Data->SetNumberField(TEXT("section_end"), SectionEnd);
				Data->SetNumberField(TEXT("position_in_section"),
					FMath::Max(0.f, Position - SectionStart));
			}
			else
			{
				Data->SetField(TEXT("section_start"), MakeShared<FJsonValueNull>());
				Data->SetField(TEXT("section_end"), MakeShared<FJsonValueNull>());
				Data->SetField(TEXT("position_in_section"), MakeShared<FJsonValueNull>());
			}
		}
		else
		{
			Data->SetField(TEXT("position"), MakeShared<FJsonValueNull>());
			Data->SetField(TEXT("montage"), MakeShared<FJsonValueNull>());
			Data->SetField(TEXT("length"), MakeShared<FJsonValueNull>());
			Data->SetField(TEXT("section_start"), MakeShared<FJsonValueNull>());
			Data->SetField(TEXT("section_end"), MakeShared<FJsonValueNull>());
			Data->SetField(TEXT("position_in_section"), MakeShared<FJsonValueNull>());
		}

		return Data;
	}

	// ------------------------------------------------------------------
	// anim.create_montage  (Issue #19)
	//
	// Build + save a `UAnimMontage` asset that wraps a single source
	// `UAnimSequence`. This is the authoring counterpart the play/stop
	// runtime tools were missing: without it every montage test had to
	// reach for `python_exec` + `AnimMontageFactory`.
	//
	// The construction recipe is lifted verbatim from the engine's own
	// `UAnimMontageFactory::FactoryCreateNew`
	// (Editor/UnrealEd/Private/Factories/AnimMontageFactory.cpp L72-104):
	//   NewObject<UAnimMontage> → FAnimSegment::SetAnimReference →
	//   SlotAnimTracks[0].AnimTrack.AnimSegments.Add →
	//   SetCompositeLength(seq->GetPlayLength()) →
	//   UpdateCommonTargetFrameRate() → SetSkeleton(seq->GetSkeleton()) →
	//   EnsureStartingSection ("Default" composite section at t=0).
	// ------------------------------------------------------------------

	/** Best-effort save of a loaded asset via the editor asset subsystem. */
	static bool SaveLoadedAssetBestEffort(UObject* Asset)
	{
		if (Asset == nullptr || GEditor == nullptr)
		{
			return false;
		}
		if (UEditorAssetSubsystem* AssetSubsystem =
			GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
		{
			return AssetSubsystem->SaveLoadedAsset(Asset, /*bOnlyIfIsDirty=*/false);
		}
		return false;
	}

	static TSharedRef<FJsonObject> HandleCreateMontage(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("asset_path"), AssetPath)
			|| AssetPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`asset_path` is required and must be a non-empty content path"));
		}
		FString SequencePath;
		if (!Args->TryGetStringField(TEXT("source_sequence_path"), SequencePath)
			|| SequencePath.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`source_sequence_path` is required and must be a non-empty asset path"));
		}

		FString SlotName;
		Args->TryGetStringField(TEXT("slot_name"), SlotName);
		FString SectionName = TEXT("Default");
		Args->TryGetStringField(TEXT("section_name"), SectionName);
		if (SectionName.IsEmpty())
		{
			SectionName = TEXT("Default");
		}

		bool bSave = true;
		(void)Args->TryGetBoolField(TEXT("save"), bSave);

		// --- resolve the source sequence ---------------------------------
		UAnimSequence* Sequence =
			LoadObject<UAnimSequence>(nullptr, *SequencePath);
		if (Sequence == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("Failed to LoadObject<UAnimSequence>('%s') — asset missing or wrong type"),
					*SequencePath));
		}

		USkeleton* Skeleton = Sequence->GetSkeleton();
		if (Skeleton == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("UAnimSequence '%s' has no USkeleton — cannot build a montage"),
					*SequencePath));
		}
		if (!Sequence->CanBeUsedInComposition())
		{
			return UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("UAnimSequence '%s' cannot be used in a montage composition"),
					*SequencePath));
		}

		// --- normalise the destination path ------------------------------
		FString PackagePath = AssetPath;
		FString ObjectName;
		int32 DotIdx = INDEX_NONE;
		if (PackagePath.FindChar(TEXT('.'), DotIdx))
		{
			ObjectName = PackagePath.Mid(DotIdx + 1);
			PackagePath.LeftInline(DotIdx, EAllowShrinking::No);
		}
		while (PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath.LeftChopInline(1, EAllowShrinking::No);
		}
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`asset_path` must be a content path like '/Game/Tests/AM_Foo' (got '%s')"),
					*AssetPath));
		}
		if (ObjectName.IsEmpty())
		{
			FString Left, Right;
			if (PackagePath.Split(TEXT("/"), &Left, &Right, ESearchCase::IgnoreCase,
					ESearchDir::FromEnd))
			{
				ObjectName = Right;
			}
		}
		if (ObjectName.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("Could not derive an asset name from '%s'"),
					*AssetPath));
		}
		const FString FullObjectPath = PackagePath + TEXT(".") + ObjectName;

		// Refuse to clobber an existing asset — authoring tools are
		// create-only; deleting/overwriting is a separate explicit op.
		if (UObject* Existing = LoadObject<UObject>(nullptr, *FullObjectPath))
		{
			return UeMcp::MakeInlineError(
				TEXT("ALREADY_EXISTS"),
				FString::Printf(
					TEXT("An asset already exists at '%s' (class '%s'); "
						 "delete it first or pick a new path"),
					*FullObjectPath, *Existing->GetClass()->GetName()));
		}

		UPackage* Pkg = CreatePackage(*PackagePath);
		if (Pkg == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(TEXT("CreatePackage('%s') returned null"), *PackagePath));
		}
		Pkg->FullyLoad();

		// --- build the montage (engine factory recipe) -------------------
		UAnimMontage* Montage = NewObject<UAnimMontage>(
			Pkg, UAnimMontage::StaticClass(), FName(*ObjectName),
			RF_Public | RF_Standalone | RF_Transactional);
		if (Montage == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(
					TEXT("NewObject<UAnimMontage>('%s') returned null"),
					*FullObjectPath));
		}

		// The UAnimMontage ctor pre-adds one slot named
		// FAnimSlotGroup::DefaultSlotName. Rename it if the caller asked
		// for a specific slot.
		FName ResolvedSlot = FAnimSlotGroup::DefaultSlotName;
		if (!SlotName.IsEmpty())
		{
			ResolvedSlot = FName(*SlotName);
			if (Montage->SlotAnimTracks.Num() > 0)
			{
				Montage->SlotAnimTracks[0].SlotName = ResolvedSlot;
			}
		}

		FAnimSegment NewSegment;
		NewSegment.SetAnimReference(Sequence, /*bInitialize=*/true);

		FSlotAnimationTrack& Track = Montage->SlotAnimTracks[0];
		Track.AnimTrack.AnimSegments.Add(NewSegment);

		Montage->SetCompositeLength(Sequence->GetPlayLength());
		// NOTE: the engine factory also calls
		// `UAnimMontage::UpdateCommonTargetFrameRate()` here, but that
		// method is `private` + WITH_EDITOR (AnimMontage.h L994) — the
		// factory has friend access, plugins do not. It is not required
		// for a playable asset: `PostEditChangeProperty` / `PreSave`
		// recompute the common frame rate on save (AnimMontage.cpp
		// L802/L1239), and `SetCompositeLength` already drove the data
		// model's frame count. So we deliberately omit the direct call.
		Montage->SetSkeleton(Skeleton);

		// Starting section at t=0 (engine factory's EnsureStartingSection).
		const int32 SectionIdx =
			Montage->AddAnimCompositeSection(FName(*SectionName), 0.0f);
		// Fallback for the unlikely AddAnimCompositeSection failure (e.g.
		// a duplicate name): guarantee at least one section so the montage
		// is playable.
		if (SectionIdx == INDEX_NONE && Montage->CompositeSections.Num() == 0)
		{
			FCompositeSection NewSection;
			NewSection.SectionName = FName(*SectionName);
			NewSection.SetTime(0.0f);
			Montage->CompositeSections.Add(NewSection);
		}

		FAssetRegistryModule::AssetCreated(Montage);
		Pkg->MarkPackageDirty();

		const bool bSaved = bSave && SaveLoadedAssetBestEffort(Montage);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("ok"), true);
		Data->SetStringField(TEXT("asset_path"), Montage->GetPathName());
		Data->SetStringField(TEXT("source_sequence_path"), Sequence->GetPathName());
		Data->SetStringField(TEXT("slot_name"), ResolvedSlot.ToString());
		Data->SetStringField(TEXT("section_name"), SectionName);
		Data->SetNumberField(TEXT("length"), Montage->GetPlayLength());
		Data->SetBoolField(TEXT("saved"), bSaved);
		if (bSave && !bSaved)
		{
			Data->SetStringField(TEXT("save_error"),
				TEXT("SaveLoadedAsset returned false or editor subsystem "
					 "unavailable; asset exists in-memory, caller may retry"));
		}

		// Rollback hint — the inverse is deleting the freshly-created asset.
		// We don't ship an anim.delete_montage; surface the path so the
		// caller (or a python_exec cleanup) can remove it deterministically.
		{
			TSharedRef<FJsonObject> Rb = MakeShared<FJsonObject>();
			Rb->SetStringField(TEXT("hint"),
				TEXT("delete the created asset to undo"));
			Rb->SetStringField(TEXT("asset_path"), Montage->GetPathName());
			Data->SetObjectField(TEXT("rollback"), Rb);
		}

		return Data;
	}
}

void UeMcp::RegisterAnimHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpAnimHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("anim.play_montage"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandlePlayMontage);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("anim.stop_montage"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleStopMontage);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("anim.get_active_section"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleGetActiveSection);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("anim.create_montage"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleCreateMontage);
		Dispatcher.RegisterTool(Reg);
	}
}
