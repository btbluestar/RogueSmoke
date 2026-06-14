// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// `subsystem.list` + `subsystem.invoke`.
//
// `subsystem.list` enumerates loaded subsystems across the four
// collections (Engine / GameInstance / World / Editor), returning each
// instance's UClass path + base + display name. Optionally filtered by
// base class so callers can scope to one collection.
//
// `subsystem.invoke` resolves a UClass from `subsystem_class`, looks up
// the matching loaded subsystem instance, finds a UFUNCTION on the
// class by name, marshals the caller's JSON arg map into the function's
// parameter buffer (using the engine's `FJsonObjectConverter`), runs
// `ProcessEvent` on the game thread, then marshals the return param +
// any out params back into JSON for the response.
//
// Why we lean on `FJsonObjectConverter` instead of the existing
// `FUeMcpPropertyAccessor`: the accessor is built around a path-walked
// _UObject_ root, not a stack-allocated parameter buffer with no UClass.
// The engine's converter already does JSON ↔ FProperty for the exact
// shape we need (one FProperty + raw memory) and is API-stable across
// 5.x, so reusing it keeps this handler small.

#include "UeMcpSubsystemHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EditorSubsystem.h"
#include "JsonObjectConverter.h"
#include "Logging/LogMacros.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "UObject/Class.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpWorldResolver.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeMcpSubsystem, Log, All);

namespace UeMcpSubsystemHandlersPrivate
{
	/** Game-thread executor cap. Subsystem reads + a single ProcessEvent
	 *  are cheap; 30s leaves headroom for a UFUNCTION that does heavy
	 *  asset-registry work synchronously. */
	static constexpr double ListTimeoutSeconds   = 15.0;
	static constexpr double InvokeTimeoutSeconds = 30.0;

	/** Wire tags for the four collections. Stable strings; callers
	 *  pattern-match on them. */
	static const TCHAR* BaseTagEngine       = TEXT("engine");
	static const TCHAR* BaseTagGameInstance = TEXT("game_instance");
	static const TCHAR* BaseTagWorld        = TEXT("world");
	static const TCHAR* BaseTagEditor       = TEXT("editor");

	/**
	 * Resolve a UClass from a caller-supplied string. Strategies, in
	 * order — same shape as `UeMcpActorHandlers::ResolveActorClass` so
	 * both surfaces accept the same argument forms:
	 *   1. `FindObject<UClass>` — exact full path (`/Script/Foo.UBar`).
	 *   2. `FSoftObjectPath::TryLoad` — load on demand for asset-form paths.
	 *   3. Short-name iteration — covers bare class names. Bounded by
	 *      the loaded UClass table; first match wins.
	 *
	 * Returns nullptr on failure; caller emits NOT_FOUND.
	 */
	static UClass* ResolveSubsystemClass(const FString& ClassNameOrPath)
	{
		const FString Trimmed = ClassNameOrPath.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return nullptr;
		}

		if (UClass* Found = FindObject<UClass>(nullptr, *Trimmed))
		{
			return Found;
		}

		if (Trimmed.Contains(TEXT(".")) || Trimmed.StartsWith(TEXT("/")))
		{
			FSoftObjectPath Soft(Trimmed);
			if (UObject* Loaded = Soft.TryLoad())
			{
				if (UClass* Cls = Cast<UClass>(Loaded))
				{
					return Cls;
				}
			}
		}

		// Short-name scan: matches both `EditorActorSubsystem` and
		// `UEditorActorSubsystem` — case-insensitive, with-or-without
		// the leading `U`.
		const FString Stripped = Trimmed.StartsWith(TEXT("U"))
			? Trimmed.RightChop(1) : Trimmed;
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Cls = *It;
			if (Cls == nullptr)
			{
				continue;
			}
			const FString Name = Cls->GetName();
			if (Name.Equals(Trimmed, ESearchCase::IgnoreCase)
				|| Name.Equals(Stripped, ESearchCase::IgnoreCase))
			{
				return Cls;
			}
		}

		return nullptr;
	}

	/** Build a `{class_path, base_class, instance_name}` JSON record. */
	static TSharedRef<FJsonObject> SubsystemRecord(USubsystem* Sub, const TCHAR* BaseTag)
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		UClass* Cls = (Sub != nullptr) ? Sub->GetClass() : nullptr;
		R->SetStringField(TEXT("class_path"),
			Cls != nullptr ? Cls->GetPathName() : FString());
		R->SetStringField(TEXT("class_name"),
			Cls != nullptr ? Cls->GetName() : FString());
		R->SetStringField(TEXT("base_class"), BaseTag);
		R->SetStringField(TEXT("instance_name"),
			Sub != nullptr ? Sub->GetName() : FString());
		return R;
	}

	/**
	 * Append every subsystem in `Subs` to `Out`. Skipped entirely when
	 * `BaseFilter` is non-empty and doesn't match this collection's tag.
	 */
	template <typename TBase>
	static void AppendCollection(
		const TArray<TBase*>& Subs,
		const FString& BaseFilter,
		const TCHAR* WireBaseTag,
		TArray<TSharedPtr<FJsonValue>>& Out)
	{
		if (!BaseFilter.IsEmpty() && !BaseFilter.Equals(WireBaseTag, ESearchCase::IgnoreCase))
		{
			return;
		}
		for (TBase* Sub : Subs)
		{
			if (Sub != nullptr)
			{
				Out.Add(MakeShared<FJsonValueObject>(SubsystemRecord(Sub, WireBaseTag)));
			}
		}
	}

	/**
	 * Resolve the right collection for a UClass and return the live
	 * subsystem instance, or nullptr when the class isn't loaded as a
	 * subsystem of any collection that's currently active. `OutBaseTag`
	 * is set to the wire base-tag on success.
	 */
	static USubsystem* FindLoadedSubsystem(UClass* Cls, UWorld* World, const TCHAR*& OutBaseTag)
	{
		OutBaseTag = nullptr;
		if (Cls == nullptr) return nullptr;

		if (Cls->IsChildOf(UEngineSubsystem::StaticClass()))
		{
			OutBaseTag = BaseTagEngine;
			if (GEngine != nullptr)
			{
				return GEngine->GetEngineSubsystemBase(Cls);
			}
			return nullptr;
		}
		if (Cls->IsChildOf(UEditorSubsystem::StaticClass()))
		{
			OutBaseTag = BaseTagEditor;
			if (GEditor != nullptr)
			{
				return GEditor->GetEditorSubsystemBase(Cls);
			}
			return nullptr;
		}
		if (Cls->IsChildOf(UGameInstanceSubsystem::StaticClass()))
		{
			OutBaseTag = BaseTagGameInstance;
			if (World != nullptr && World->GetGameInstance() != nullptr)
			{
				return World->GetGameInstance()->GetSubsystemBase(Cls);
			}
			return nullptr;
		}
		if (Cls->IsChildOf(UWorldSubsystem::StaticClass()))
		{
			OutBaseTag = BaseTagWorld;
			if (World != nullptr)
			{
				return World->GetSubsystemBase(Cls);
			}
			return nullptr;
		}
		return nullptr;
	}

	/**
	 * Marshal one JSON arg into a parameter slot. Returns false on
	 * failure with `OutFail` populated (caller surfaces TYPE_MISMATCH).
	 */
	static bool WriteJsonIntoParam(
		FProperty* Prop,
		void* SlotAddr,
		const TSharedPtr<FJsonValue>& Value,
		FString& OutFail)
	{
		if (Prop == nullptr || SlotAddr == nullptr)
		{
			OutFail = TEXT("internal: null property or slot");
			return false;
		}
		if (!Value.IsValid())
		{
			OutFail = TEXT("missing JSON value for parameter");
			return false;
		}
		FText Reason;
		if (!FJsonObjectConverter::JsonValueToUProperty(
				Value, Prop, SlotAddr,
				/*CheckFlags*/ 0, /*SkipFlags*/ 0,
				/*bStrictMode*/ false, &Reason))
		{
			OutFail = Reason.IsEmpty()
				? FString::Printf(TEXT("could not coerce JSON to %s"), *Prop->GetCPPType())
				: Reason.ToString();
			return false;
		}
		return true;
	}

	/**
	 * Marshal an output param (return or out-by-ref) back to JSON.
	 * Returns null JSON on failure rather than failing the call —
	 * by the time we're reading outputs the side-effect has already
	 * happened, and stripping the field is the least-bad outcome.
	 */
	static TSharedPtr<FJsonValue> ReadParamAsJson(
		FProperty* Prop, const void* SlotAddr)
	{
		if (Prop == nullptr || SlotAddr == nullptr)
		{
			return MakeShared<FJsonValueNull>();
		}
		TSharedPtr<FJsonValue> Out = FJsonObjectConverter::UPropertyToJsonValue(
			Prop, SlotAddr,
			/*CheckFlags*/ 0, /*SkipFlags*/ 0);
		if (!Out.IsValid())
		{
			return MakeShared<FJsonValueNull>();
		}
		return Out;
	}

	/**
	 * RAII wrapper for a stack-allocated parameter buffer. Constructs
	 * each FProperty with `InitializeValue_InContainer` and tears them
	 * down with `DestroyValue_InContainer` so out-params holding
	 * containers (TArray, FString) free their memory.
	 *
	 * Construction matches what `UObject::ProcessEvent` itself does for
	 * its blueprint-driven path: zero the buffer first, then init each
	 * property. `Memzero` alone is insufficient for properties that
	 * carry a non-trivial constructor (FString allocates, TArray sets
	 * its allocator, etc.).
	 */
	struct FParameterBuffer
	{
		uint8* Buffer = nullptr;
		UFunction* Function = nullptr;

		FParameterBuffer(UFunction* Func)
			: Function(Func)
		{
			if (Function != nullptr && Function->ParmsSize > 0)
			{
				Buffer = static_cast<uint8*>(
					FMemory::Malloc(Function->ParmsSize, Function->GetMinAlignment()));
				FMemory::Memzero(Buffer, Function->ParmsSize);
				for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					It->InitializeValue_InContainer(Buffer);
				}
			}
		}

		~FParameterBuffer()
		{
			if (Buffer != nullptr && Function != nullptr)
			{
				for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					It->DestroyValue_InContainer(Buffer);
				}
				FMemory::Free(Buffer);
				Buffer = nullptr;
			}
		}

		FParameterBuffer(const FParameterBuffer&) = delete;
		FParameterBuffer& operator=(const FParameterBuffer&) = delete;
	};

	/**
	 * `subsystem.list` body. Filters by `base_class` if provided.
	 */
	static TSharedRef<FJsonObject> HandleSubsystemList(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BaseFilter;
		Args->TryGetStringField(TEXT("base_class"), BaseFilter);
		BaseFilter = BaseFilter.TrimStartAndEnd();

		// Resolve world for the GameInstance/World collections. `auto`
		// is the right default — same as every other tool here.
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		// World resolution failure is non-fatal for `subsystem.list`:
		// engine + editor collections are still meaningful. Only the
		// game-instance / world entries silently empty.
		UWorld* WorldPtr = World.IsOk() ? World.World : nullptr;

		TArray<TSharedPtr<FJsonValue>> Items;

		// Engine subsystems.
		if (GEngine != nullptr)
		{
			TArray<UEngineSubsystem*> Engines =
				GEngine->GetEngineSubsystemArrayCopy<UEngineSubsystem>();
			AppendCollection<UEngineSubsystem>(Engines, BaseFilter, BaseTagEngine, Items);
		}

		// Editor subsystems.
		if (GEditor != nullptr)
		{
			TArray<UEditorSubsystem*> EditorSubs =
				GEditor->GetEditorSubsystemArrayCopy<UEditorSubsystem>();
			AppendCollection<UEditorSubsystem>(EditorSubs, BaseFilter, BaseTagEditor, Items);
		}

		// Game-instance subsystems.
		if (WorldPtr != nullptr && WorldPtr->GetGameInstance() != nullptr)
		{
			TArray<UGameInstanceSubsystem*> GISubs =
				WorldPtr->GetGameInstance()->GetSubsystemArrayCopy<UGameInstanceSubsystem>();
			AppendCollection<UGameInstanceSubsystem>(GISubs, BaseFilter, BaseTagGameInstance, Items);
		}

		// World subsystems.
		if (WorldPtr != nullptr)
		{
			TArray<UWorldSubsystem*> WorldSubs =
				WorldPtr->GetSubsystemArrayCopy<UWorldSubsystem>();
			AppendCollection<UWorldSubsystem>(WorldSubs, BaseFilter, BaseTagWorld, Items);
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("subsystems"), Items);
		Data->SetNumberField(TEXT("count"), Items.Num());
		if (!BaseFilter.IsEmpty())
		{
			Data->SetStringField(TEXT("base_filter"), BaseFilter);
		}
		Data->SetStringField(TEXT("world"),
			UeMcp::WorldScopeToString(World.ResolvedScope));
		return Data;
	}

	/**
	 * `subsystem.invoke` body.
	 */
	static TSharedRef<FJsonObject> HandleSubsystemInvoke(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		// --- Required args ---
		FString ClassRef;
		if (!Args->TryGetStringField(TEXT("subsystem_class"), ClassRef)
			|| ClassRef.TrimStartAndEnd().IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`subsystem_class` is required and must be a non-empty string"));
		}
		FString FunctionName;
		if (!Args->TryGetStringField(TEXT("function_name"), FunctionName)
			|| FunctionName.TrimStartAndEnd().IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`function_name` is required and must be a non-empty string"));
		}

		// `args` is an optional JSON object keyed by parameter name.
		// Empty/absent is fine for no-arg functions.
		const TSharedPtr<FJsonObject>* CallArgsPtr = nullptr;
		Args->TryGetObjectField(TEXT("args"), CallArgsPtr);
		TSharedPtr<FJsonObject> CallArgs =
			(CallArgsPtr != nullptr && CallArgsPtr->IsValid())
				? *CallArgsPtr
				: TSharedPtr<FJsonObject>();

		// --- World resolution (for game-instance/world collections) ---
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		UWorld* WorldPtr = World.IsOk() ? World.World : nullptr;

		// --- Class resolution ---
		UClass* Cls = ResolveSubsystemClass(ClassRef);
		if (Cls == nullptr)
		{
			TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Could not resolve class '%s'"), *ClassRef));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("subsystem_class"), ClassRef);
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}
		if (!Cls->IsChildOf(USubsystem::StaticClass()))
		{
			TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("Class '%s' is not a USubsystem"),
					*Cls->GetPathName()));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("class_path"), Cls->GetPathName());
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}

		// --- Locate the live subsystem instance ---
		const TCHAR* BaseTag = nullptr;
		USubsystem* Subsystem = FindLoadedSubsystem(Cls, WorldPtr, BaseTag);
		if (Subsystem == nullptr)
		{
			TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("Subsystem '%s' is not loaded in any active collection"),
					*Cls->GetName()));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("class_path"), Cls->GetPathName());
			if (BaseTag != nullptr)
			{
				Detail->SetStringField(TEXT("base_class"), BaseTag);
			}
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}

		// --- Function lookup ---
		UFunction* Function = Cls->FindFunctionByName(FName(*FunctionName));
		if (Function == nullptr)
		{
			TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
				TEXT("PROP_NOT_FOUND"),
				FString::Printf(
					TEXT("UFUNCTION '%s' not found on '%s'"),
					*FunctionName, *Cls->GetName()));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("function_name"), FunctionName);
			Detail->SetStringField(TEXT("class_path"), Cls->GetPathName());
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}

		// --- Marshal args into the parameter buffer ---
		FParameterBuffer Params(Function);

		TArray<FString> MissingArgs;
		TArray<TPair<FString, FString>> ArgFailures; // {param_name, reason}

		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Parm)) continue;
			if (Prop->HasAnyPropertyFlags(CPF_ReturnParm)) continue;

			const bool bIsOutParm = Prop->HasAnyPropertyFlags(CPF_OutParm)
				&& !Prop->HasAnyPropertyFlags(CPF_ConstParm);
			// Pure-out params (no const, no input) skip JSON marshal —
			// they're written to by the callee. UE encodes "in/out" as
			// CPF_OutParm without CPF_ReferenceParm-only-out semantics
			// being carved out separately, so we treat any non-const
			// CPF_OutParm without explicit input as "output only" by
			// trying to find the arg key first; if absent, leave zero-
			// initialised.
			const FString ParamName = Prop->GetName();
			if (CallArgs.IsValid() && CallArgs->Values.Contains(ParamName))
			{
				const TSharedPtr<FJsonValue>& V =
					CallArgs->Values.FindChecked(ParamName);
				FString Fail;
				void* SlotAddr = Prop->ContainerPtrToValuePtr<void>(Params.Buffer);
				if (!WriteJsonIntoParam(Prop, SlotAddr, V, Fail))
				{
					ArgFailures.Emplace(ParamName, Fail);
				}
			}
			else if (!bIsOutParm)
			{
				// Required-input parameter that the caller didn't supply.
				MissingArgs.Add(ParamName);
			}
		}

		if (MissingArgs.Num() > 0 || ArgFailures.Num() > 0)
		{
			TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("Argument marshalling failed for '%s' (%d missing, %d invalid)"),
					*FunctionName, MissingArgs.Num(), ArgFailures.Num()));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Missing;
			for (const FString& M : MissingArgs)
			{
				Missing.Add(MakeShared<FJsonValueString>(M));
			}
			Detail->SetArrayField(TEXT("missing_args"), Missing);
			TArray<TSharedPtr<FJsonValue>> Failures;
			for (const TPair<FString, FString>& F : ArgFailures)
			{
				TSharedRef<FJsonObject> FObj = MakeShared<FJsonObject>();
				FObj->SetStringField(TEXT("param"), F.Key);
				FObj->SetStringField(TEXT("reason"), F.Value);
				Failures.Add(MakeShared<FJsonValueObject>(FObj));
			}
			Detail->SetArrayField(TEXT("invalid_args"), Failures);
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}

		// --- Invoke ---
		UE_LOG(LogUeMcpSubsystem, Verbose,
			TEXT("subsystem.invoke %s::%s"),
			*Cls->GetName(), *FunctionName);
		Subsystem->ProcessEvent(Function, Params.Buffer);

		// --- Marshal return + out params back to JSON ---
		TSharedPtr<FJsonValue> ReturnJson;
		TSharedRef<FJsonObject> OutParamsJson = MakeShared<FJsonObject>();
		bool bHasReturn = false;

		for (TFieldIterator<FProperty> It(Function); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop->HasAnyPropertyFlags(CPF_Parm)) continue;

			void* SlotAddr = Prop->ContainerPtrToValuePtr<void>(Params.Buffer);
			if (Prop->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				ReturnJson = ReadParamAsJson(Prop, SlotAddr);
				bHasReturn = true;
				continue;
			}
			if (Prop->HasAnyPropertyFlags(CPF_OutParm)
				&& !Prop->HasAnyPropertyFlags(CPF_ConstParm))
			{
				TSharedPtr<FJsonValue> V = ReadParamAsJson(Prop, SlotAddr);
				if (V.IsValid())
				{
					OutParamsJson->SetField(Prop->GetName(), V);
				}
			}
		}

		// --- Build response ---
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("class_path"), Cls->GetPathName());
		Data->SetStringField(TEXT("class_name"), Cls->GetName());
		Data->SetStringField(TEXT("function_name"), FunctionName);
		if (BaseTag != nullptr)
		{
			Data->SetStringField(TEXT("base_class"), BaseTag);
		}
		Data->SetBoolField(TEXT("has_return"), bHasReturn);
		if (bHasReturn && ReturnJson.IsValid())
		{
			Data->SetField(TEXT("return"), ReturnJson);
		}
		else
		{
			Data->SetField(TEXT("return"), MakeShared<FJsonValueNull>());
		}
		Data->SetObjectField(TEXT("out_params"), OutParamsJson);
		Data->SetStringField(TEXT("world"),
			UeMcp::WorldScopeToString(World.ResolvedScope));
		return Data;
	}
}

void UeMcp::RegisterSubsystemHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpSubsystemHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("subsystem.list"));
		Reg.DefaultTimeoutSeconds = ListTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSubsystemList);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("subsystem.invoke"));
		Reg.DefaultTimeoutSeconds = InvokeTimeoutSeconds;
		// Conservatively `bMutating=true` — `subsystem.invoke` calls an
		// arbitrary UFUNCTION, which may mutate engine state.
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSubsystemInvoke);
		Dispatcher.RegisterTool(Reg);
	}
}
