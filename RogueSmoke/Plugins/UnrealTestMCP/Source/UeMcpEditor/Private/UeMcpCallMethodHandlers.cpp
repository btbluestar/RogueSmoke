// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// `actor.call_method` + `component.call_method` (issue #30).
//
// Invokes a UFUNCTION on a live object strictly through `UClass`
// reflection — `FindFunctionByName` + `ProcessEvent` on a freshly
// resolved instance. There is NO cached function pointer anywhere in
// this path, which is the whole point: Python's auto-generated
// snake_case UFUNCTION bindings cache a pointer from the first class
// load and dispatch to STALE bytecode after an AngelScript hot-reload.
// A reflection call always hits the current `UClass`, so a hot-reloaded
// AS class is dispatched correctly with no editor restart.
//
// Marshalling reuses the engine's `FJsonObjectConverter` exactly the
// way `subsystem.invoke` (UeMcpSubsystemHandlers.cpp) does — one
// FProperty + raw parameter-slot memory, JSON in / JSON out. The brief
// points at the reflection handlers' value path; the property accessor
// (`FUeMcpPropertyAccessor`) is built around a path-walked UObject root,
// not a stack-allocated parameter buffer with no UClass, so the
// converter is the right tool for params/return and is API-stable
// across 5.x. Object resolution reuses `UeMcp::ResolveObject` — the
// same five-strategy chain `get_property` / `set_property` use — so
// `actor.call_method` accepts identical `object` argument forms.

#include "UeMcpCallMethodHandlers.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "JsonObjectConverter.h"
#include "Logging/LogMacros.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeMcpCallMethod, Log, All);

namespace UeMcpCallMethodHandlersPrivate
{
	/** Game-thread executor cap. A reflected lookup plus a single
	 *  ProcessEvent is cheap; 30 s leaves headroom for a UFUNCTION that
	 *  does heavy synchronous work (asset registry walk, etc.). Mirrors
	 *  `subsystem.invoke`. */
	static constexpr double CallMethodTimeoutSeconds = 30.0;

	/**
	 * Resolve a component on `Actor` by name. Same matching rule the
	 * property accessor's first-segment component fallback uses
	 * (`UeMcpPropertyAccessor.cpp::FindComponentByName`): exact match,
	 * then case-insensitive, then a case-insensitive prefix match where
	 * the remainder is a numeric/underscore auto-suffix. Reimplemented
	 * here (a few lines) because that helper is a file-local static in
	 * the runtime module, not exported.
	 */
	static UActorComponent* FindComponentByName(AActor* Actor, const FString& ComponentName)
	{
		if (Actor == nullptr || ComponentName.IsEmpty())
		{
			return nullptr;
		}

		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);

		for (UActorComponent* Comp : Components)
		{
			if (Comp != nullptr && Comp->GetName() == ComponentName)
			{
				return Comp;
			}
		}
		for (UActorComponent* Comp : Components)
		{
			if (Comp != nullptr
				&& Comp->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Comp;
			}
		}
		for (UActorComponent* Comp : Components)
		{
			if (Comp == nullptr)
			{
				continue;
			}
			const FString CompName = Comp->GetName();
			if (!CompName.StartsWith(ComponentName, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString Suffix = CompName.Mid(ComponentName.Len());
			bool bAllAutoSuffix = !Suffix.IsEmpty();
			for (const TCHAR Ch : Suffix)
			{
				if (!FChar::IsDigit(Ch) && Ch != TEXT('_'))
				{
					bAllAutoSuffix = false;
					break;
				}
			}
			if (bAllAutoSuffix)
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/** List a few component names for a `detail.available_components`
	 *  hint when a component name doesn't resolve. */
	static TArray<TSharedPtr<FJsonValue>> ListComponentNames(AActor* Actor)
	{
		TArray<TSharedPtr<FJsonValue>> Names;
		if (Actor == nullptr)
		{
			return Names;
		}
		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			if (Comp != nullptr)
			{
				Names.Add(MakeShared<FJsonValueString>(Comp->GetName()));
			}
		}
		return Names;
	}

	/**
	 * Marshal one JSON arg into a parameter slot. Returns false on
	 * failure with `OutFail` populated. Identical to the
	 * `subsystem.invoke` helper — same converter, same flags.
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
	 * Marshal an output param (return or out-by-ref) back to JSON. Null
	 * JSON on failure rather than failing the call — by the time we read
	 * outputs the side-effect already ran; stripping the field is the
	 * least-bad outcome. Identical to the `subsystem.invoke` helper.
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
	 * RAII wrapper for a stack-allocated parameter buffer. Zero the
	 * buffer, then `InitializeValue_InContainer` each parm, tear down
	 * with `DestroyValue_InContainer` so container out-params (TArray,
	 * FString) free their memory. Mirrors what `UObject::ProcessEvent`
	 * itself does for its blueprint-driven path. Identical to the
	 * `subsystem.invoke` helper.
	 */
	struct FParameterBuffer
	{
		uint8* Buffer = nullptr;
		UFunction* Function = nullptr;

		explicit FParameterBuffer(UFunction* Func)
			: Function(Func)
		{
			if (Function != nullptr && Function->ParmsSize > 0)
			{
				Buffer = static_cast<uint8*>(
					FMemory::Malloc(Function->ParmsSize, Function->GetMinAlignment()));
				FMemory::Memzero(Buffer, Function->ParmsSize);
				for (TFieldIterator<FProperty> It(Function);
					It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
				{
					It->InitializeValue_InContainer(Buffer);
				}
			}
		}

		~FParameterBuffer()
		{
			if (Buffer != nullptr && Function != nullptr)
			{
				for (TFieldIterator<FProperty> It(Function);
					It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
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
	 * Shared body for both tools. `bComponentTarget` selects whether the
	 * resolved object is treated as the call target directly (actor) or
	 * whether a `component` arg drills into a named component first.
	 */
	static TSharedRef<FJsonObject> HandleCallMethod(
		const TSharedRef<FJsonObject>& Args,
		bool bComponentTarget)
	{
		check(IsInGameThread());

		// --- World resolution (so we search the right actor set) ---
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		// --- Required args ---
		FString ObjectId;
		if (!Args->TryGetStringField(TEXT("object"), ObjectId)
			|| ObjectId.TrimStartAndEnd().IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`object` is required and must be a non-empty string"));
		}

		FString FunctionName;
		if (!Args->TryGetStringField(TEXT("function_name"), FunctionName)
			|| FunctionName.TrimStartAndEnd().IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`function_name` is required and must be a non-empty string"));
		}
		FunctionName = FunctionName.TrimStartAndEnd();

		FString ComponentName;
		if (bComponentTarget)
		{
			if (!Args->TryGetStringField(TEXT("component"), ComponentName)
				|| ComponentName.TrimStartAndEnd().IsEmpty())
			{
				return UeMcp::MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					TEXT("`component` is required for component.call_method "
						"and must be a non-empty string"));
			}
			ComponentName = ComponentName.TrimStartAndEnd();
		}

		// `args` is an optional JSON object keyed by parameter name.
		// Empty/absent is fine for no-arg functions.
		const TSharedPtr<FJsonObject>* CallArgsPtr = nullptr;
		Args->TryGetObjectField(TEXT("args"), CallArgsPtr);
		TSharedPtr<FJsonObject> CallArgs =
			(CallArgsPtr != nullptr && CallArgsPtr->IsValid())
				? *CallArgsPtr
				: TSharedPtr<FJsonObject>();

		// --- Object resolution (shared five-strategy chain) ---
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ObjectId, World.World);
		if (!Resolved.IsOk())
		{
			return Resolved.ErrorInfo.ToSharedRef();
		}

		// The instance the UFUNCTION is invoked on. For actor.call_method
		// it's the resolved object; for component.call_method we drill in.
		UObject* CallTarget = Resolved.Object;

		if (bComponentTarget)
		{
			AActor* Actor = Cast<AActor>(Resolved.Object);
			if (Actor == nullptr)
			{
				TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
					TEXT("TYPE_MISMATCH"),
					FString::Printf(
						TEXT("Resolved object '%s' is not an AActor — "
							"component.call_method needs an actor to host the "
							"component"),
						*ObjectId));
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("object"), ObjectId);
				Detail->SetStringField(TEXT("resolved_class"),
					Resolved.Object->GetClass()
						? Resolved.Object->GetClass()->GetPathName()
						: FString());
				Err->SetObjectField(TEXT("detail"), Detail);
				return Err;
			}

			UActorComponent* Comp = FindComponentByName(Actor, ComponentName);
			if (Comp == nullptr)
			{
				TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
					TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("Component '%s' not found on actor '%s'"),
						*ComponentName, *ObjectId));
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("component"), ComponentName);
				Detail->SetStringField(TEXT("object"), ObjectId);
				Detail->SetArrayField(TEXT("available_components"),
					ListComponentNames(Actor));
				Err->SetObjectField(TEXT("detail"), Detail);
				return Err;
			}
			CallTarget = Comp;
		}

		UClass* Cls = CallTarget->GetClass();

		// --- Function lookup (LIVE — no cached binding) ---
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
			// A handful of callable UFUNCTION names to ease the typo case.
			TArray<TSharedPtr<FJsonValue>> Funcs;
			for (TFieldIterator<UFunction> It(Cls); It && Funcs.Num() < 40; ++It)
			{
				if (UFunction* F = *It)
				{
					Funcs.Add(MakeShared<FJsonValueString>(F->GetName()));
				}
			}
			Detail->SetArrayField(TEXT("available_functions"), Funcs);
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
				// Required-input parameter the caller didn't supply.
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

		// --- Invoke (LIVE reflection — ProcessEvent on the current
		//     UClass's UFunction; no cached function pointer) ---
		UE_LOG(LogUeMcpCallMethod, Verbose,
			TEXT("%s.call_method %s::%s"),
			bComponentTarget ? TEXT("component") : TEXT("actor"),
			*Cls->GetName(), *FunctionName);
		CallTarget->ProcessEvent(Function, Params.Buffer);

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
		Data->SetStringField(TEXT("object"), ObjectId);
		if (bComponentTarget)
		{
			Data->SetStringField(TEXT("component"), ComponentName);
		}
		Data->SetStringField(TEXT("class_path"), Cls->GetPathName());
		Data->SetStringField(TEXT("class_name"), Cls->GetName());
		Data->SetStringField(TEXT("function_name"), FunctionName);
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
		if (Resolved.bLoaded)
		{
			Data->SetBoolField(TEXT("asset_loaded_on_demand"), true);
		}
		Data->SetStringField(TEXT("world"),
			UeMcp::WorldScopeToString(World.ResolvedScope));
		return Data;
	}

	/** `actor.call_method` body. */
	static TSharedRef<FJsonObject> HandleActorCallMethod(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		return HandleCallMethod(Args, /*bComponentTarget*/ false);
	}

	/** `component.call_method` body. */
	static TSharedRef<FJsonObject> HandleComponentCallMethod(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		return HandleCallMethod(Args, /*bComponentTarget*/ true);
	}
}

void UeMcp::RegisterCallMethodHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpCallMethodHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.call_method"));
		Reg.DefaultTimeoutSeconds = CallMethodTimeoutSeconds;
		// Conservatively mutating — the invoked UFUNCTION may mutate
		// gameplay/editor state.
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorCallMethod);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("component.call_method"));
		Reg.DefaultTimeoutSeconds = CallMethodTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleComponentCallMethod);
		Dispatcher.RegisterTool(Reg);
	}
}
