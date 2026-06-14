// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave F / Agent-11 — `ai.set_blackboard`, `ai.get_blackboard`,
// `ai.start_bt`. Game-thread synchronous handlers; no tick state.
//
// Why these and not python_exec: tests authored by Claude need to drive
// blackboard state to seed deterministic AI runs ("zombie sees player at
// LKP X") and to start a BT off-fixture ("attach BT_Aggro to this
// controller"). Each is a one-line wrapper over engine API that without
// this surface forces every test into a python_exec block (slower, no
// type validation, escapes the trust boundary).
//
// Target resolution: callers pass a `target` string the ObjectResolver
// chain knows how to handle (actor label, name, asset path). We accept
// either an `AAIController` directly OR an `APawn` and walk to its
// controller via `GetController<AAIController>()`.
//
// Type dispatch (`type` arg): one of the eight blackboard primitives —
// `bool`, `int`, `float`, `string`, `name`, `vector`, `rotator`,
// `object`. Each maps to a `SetValueAs<T>` / `GetValueAs<T>` pair and a
// JSON marshalling shape:
//   - bool         <-> JSON bool
//   - int          <-> JSON number (clamped to int32)
//   - float        <-> JSON number
//   - string       <-> JSON string
//   - name         <-> JSON string
//   - vector       <-> JSON [x, y, z] number array
//   - rotator      <-> JSON [pitch, yaw, roll] number array
//   - object       <-> JSON string (object path on get; ObjectResolver
//                                   string on set, or null to clear)
//
// `ai.start_bt` loads the BT asset by `/Game/...` path and calls
// `RunBehaviorTree` — engine semantics: returns true on success, replaces
// any current tree on the controller.

#include "UeMcpAIHandlers.h"

#include "AIController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#include "UeMcpDispatcher.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpAIHandlersPrivate
{
	/** Default dispatcher timeout. All three handlers are sync + cheap. */
	static constexpr double DefaultTimeoutSeconds = 5.0;

	/** Wire strings for the 8 supported blackboard primitive types. */
	enum class EBBType : uint8
	{
		Invalid,
		Bool,
		Int,
		Float,
		String,
		Name,
		Vector,
		Rotator,
		Object,
	};

	static const TCHAR* TypeToWire(EBBType Type)
	{
		switch (Type)
		{
			case EBBType::Bool:    return TEXT("bool");
			case EBBType::Int:     return TEXT("int");
			case EBBType::Float:   return TEXT("float");
			case EBBType::String:  return TEXT("string");
			case EBBType::Name:    return TEXT("name");
			case EBBType::Vector:  return TEXT("vector");
			case EBBType::Rotator: return TEXT("rotator");
			case EBBType::Object:  return TEXT("object");
			default:               return TEXT("invalid");
		}
	}

	static EBBType ParseType(const FString& Token)
	{
		if (Token.Equals(TEXT("bool"),    ESearchCase::IgnoreCase)) return EBBType::Bool;
		if (Token.Equals(TEXT("int"),     ESearchCase::IgnoreCase)) return EBBType::Int;
		if (Token.Equals(TEXT("float"),   ESearchCase::IgnoreCase)) return EBBType::Float;
		if (Token.Equals(TEXT("string"),  ESearchCase::IgnoreCase)) return EBBType::String;
		if (Token.Equals(TEXT("name"),    ESearchCase::IgnoreCase)) return EBBType::Name;
		if (Token.Equals(TEXT("vector"),  ESearchCase::IgnoreCase)) return EBBType::Vector;
		if (Token.Equals(TEXT("rotator"), ESearchCase::IgnoreCase)) return EBBType::Rotator;
		if (Token.Equals(TEXT("object"),  ESearchCase::IgnoreCase)) return EBBType::Object;
		return EBBType::Invalid;
	}

	static const TCHAR* SupportedTypesList()
	{
		return TEXT("bool|int|float|string|name|vector|rotator|object");
	}

	/**
	 * Resolve `target` to an `AAIController`. Accepts either the
	 * controller directly or a pawn (then walks to its controller).
	 * Returns nullptr on failure with `OutErr` populated.
	 */
	static AAIController* ResolveAIController(
		const FString& TargetId,
		UWorld* World,
		TSharedPtr<FJsonObject>& OutErr)
	{
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(TargetId, World);
		if (!Resolved.IsOk())
		{
			OutErr = Resolved.ErrorInfo;
			return nullptr;
		}

		if (AAIController* AsController = Cast<AAIController>(Resolved.Object))
		{
			return AsController;
		}

		if (APawn* Pawn = Cast<APawn>(Resolved.Object))
		{
			AAIController* AIController = Cast<AAIController>(Pawn->GetController());
			if (AIController == nullptr)
			{
				OutErr = UeMcp::MakeInlineError(
					TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("Pawn '%s' has no AAIController possessing it; ")
						TEXT("spawn the pawn under an AIController class or pass the controller directly"),
						*TargetId));
				return nullptr;
			}
			return AIController;
		}

		OutErr = UeMcp::MakeInlineError(
			TEXT("TYPE_MISMATCH"),
			FString::Printf(
				TEXT("`target` resolved to '%s' (class '%s') — expected an AAIController or APawn"),
				*TargetId,
				Resolved.Object ? *Resolved.Object->GetClass()->GetName() : TEXT("?")));
		return nullptr;
	}

	/**
	 * Get the blackboard component on a resolved controller. Returns
	 * nullptr + error on miss — `RunBehaviorTree` populates the
	 * blackboard, so a null component generally means no BT has been
	 * started yet.
	 */
	static UBlackboardComponent* GetBlackboard(
		AAIController* Controller,
		const FString& TargetId,
		TSharedPtr<FJsonObject>& OutErr)
	{
		UBlackboardComponent* BB = Controller->GetBlackboardComponent();
		if (BB == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("AAIController '%s' has no UBlackboardComponent; ")
					TEXT("call ai.start_bt with a UBehaviorTree first to initialise it"),
					*TargetId));
			return nullptr;
		}
		return BB;
	}

	/** Helper: extract a 3-element number array as a vector / rotator triple. */
	static bool TryReadNumberTriple(
		const TSharedPtr<FJsonValue>& V,
		const TCHAR* FieldName,
		double& X, double& Y, double& Z,
		FString& OutErr)
	{
		if (!V.IsValid() || V->Type != EJson::Array)
		{
			OutErr = FString::Printf(
				TEXT("`%s` must be a 3-element number array"), FieldName);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& Arr = V->AsArray();
		if (Arr.Num() != 3)
		{
			OutErr = FString::Printf(
				TEXT("`%s` must be exactly 3 numbers; got %d"),
				FieldName, Arr.Num());
			return false;
		}
		for (int32 i = 0; i < 3; ++i)
		{
			if (!Arr[i].IsValid() || Arr[i]->Type != EJson::Number)
			{
				OutErr = FString::Printf(
					TEXT("`%s[%d]` must be a number"), FieldName, i);
				return false;
			}
		}
		X = Arr[0]->AsNumber();
		Y = Arr[1]->AsNumber();
		Z = Arr[2]->AsNumber();
		return true;
	}

	/** Helper: 3-element number array out, for vector / rotator results. */
	static TSharedRef<FJsonValueArray> MakeNumberTriple(double X, double Y, double Z)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(3);
		Arr.Add(MakeShared<FJsonValueNumber>(X));
		Arr.Add(MakeShared<FJsonValueNumber>(Y));
		Arr.Add(MakeShared<FJsonValueNumber>(Z));
		return MakeShared<FJsonValueArray>(Arr);
	}

	/**
	 * Read the current value of a blackboard key as a JSON value in the
	 * exact wire shape `ai.set_blackboard` accepts for `value` (scalar for
	 * bool/int/float/string/name, 3-array for vector/rotator, path string
	 * or null for object). Returns an invalid ptr only for the unreachable
	 * default — callers omit the rollback hint in that case.
	 */
	static TSharedPtr<FJsonValue> CaptureBlackboardValue(
		UBlackboardComponent* BB, const FName& KeyName, EBBType Type)
	{
		switch (Type)
		{
		case EBBType::Bool:
			return MakeShared<FJsonValueBoolean>(BB->GetValueAsBool(KeyName));
		case EBBType::Int:
			return MakeShared<FJsonValueNumber>(BB->GetValueAsInt(KeyName));
		case EBBType::Float:
			return MakeShared<FJsonValueNumber>(BB->GetValueAsFloat(KeyName));
		case EBBType::String:
			return MakeShared<FJsonValueString>(BB->GetValueAsString(KeyName));
		case EBBType::Name:
			return MakeShared<FJsonValueString>(
				BB->GetValueAsName(KeyName).ToString());
		case EBBType::Vector:
		{
			if (!BB->IsVectorValueSet(KeyName))
			{
				return MakeShared<FJsonValueNull>();
			}
			const FVector V = BB->GetValueAsVector(KeyName);
			return MakeNumberTriple(V.X, V.Y, V.Z);
		}
		case EBBType::Rotator:
		{
			const FRotator R = BB->GetValueAsRotator(KeyName);
			return MakeNumberTriple(R.Pitch, R.Yaw, R.Roll);
		}
		case EBBType::Object:
		{
			UObject* Obj = BB->GetValueAsObject(KeyName);
			return Obj != nullptr
				? StaticCastSharedRef<FJsonValue>(
					MakeShared<FJsonValueString>(Obj->GetPathName()))
				: StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>());
		}
		default:
			return nullptr;
		}
	}

	/**
	 * `ai.set_blackboard` body. Writes a typed value into a key by name.
	 *
	 * Args: `target` (string), `key` (string), `type` (string),
	 *       `value` (typed; null clears object keys), `world` (optional).
	 *
	 * Returns: `{ok: true, target, key, type, set_value}`.
	 */
	static TSharedRef<FJsonObject> HandleSetBlackboard(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString TargetId;
		if (!Args->TryGetStringField(TEXT("target"), TargetId) || TargetId.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`target` is required and must be a non-empty string"));
		}
		FString KeyStr;
		if (!Args->TryGetStringField(TEXT("key"), KeyStr) || KeyStr.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`key` is required and must be a non-empty string"));
		}
		FString TypeStr;
		if (!Args->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`type` is required (one of %s)"),
					SupportedTypesList()));
		}
		const EBBType Type = ParseType(TypeStr);
		if (Type == EBBType::Invalid)
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`type` must be one of %s; got '%s'"),
					SupportedTypesList(), *TypeStr));
		}

		const TSharedPtr<FJsonValue> Value = Args->TryGetField(TEXT("value"));
		// Object is the only type for which a missing/null value is meaningful
		// (clears the entry). Everything else requires a value.
		if (Type != EBBType::Object
			&& (!Value.IsValid() || Value->Type == EJson::Null))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`value` is required for type='%s'"),
					TypeToWire(Type)));
		}

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		TSharedPtr<FJsonObject> Err;
		AAIController* Controller = ResolveAIController(TargetId, World.World, Err);
		if (Controller == nullptr)
		{
			return Err.ToSharedRef();
		}
		UBlackboardComponent* BB = GetBlackboard(Controller, TargetId, Err);
		if (BB == nullptr)
		{
			return Err.ToSharedRef();
		}

		// Validate key exists on the asset (better error than silent no-op).
		const FName KeyName(*KeyStr);
		if (BB->GetKeyID(KeyName) == FBlackboard::InvalidKey)
		{
			return UeMcp::MakeInlineError(TEXT("KEY_NOT_FOUND"),
				FString::Printf(
					TEXT("Blackboard on '%s' has no key named '%s'"),
					*TargetId, *KeyStr));
		}

		// Pre-write capture for the rollback hint. We snapshot the current
		// value (in the same wire shape `value` accepts) BEFORE any write
		// so a failed flow can restore the prior state. Same `target`/`key`/
		// `type` natural keys are stable across the rollback lifetime. See
		// docs/handler-conventions.md §4 and the canonical pre-state pattern
		// in UeMcpReflectionHandlers.cpp (set_property → set_property prev).
		const TSharedPtr<FJsonValue> PriorValue =
			CaptureBlackboardValue(BB, KeyName, Type);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("ok"), true);
		Data->SetStringField(TEXT("target"), TargetId);
		Data->SetStringField(TEXT("key"), KeyStr);
		Data->SetStringField(TEXT("type"), TypeToWire(Type));

		// Type-dispatched write + echo of the set_value back as JSON.
		switch (Type)
		{
		case EBBType::Bool:
		{
			if (Value->Type != EJson::Boolean)
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					TEXT("`value` for type='bool' must be a JSON boolean"));
			}
			const bool BV = Value->AsBool();
			BB->SetValueAsBool(KeyName, BV);
			Data->SetBoolField(TEXT("set_value"), BV);
			break;
		}
		case EBBType::Int:
		{
			if (Value->Type != EJson::Number)
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					TEXT("`value` for type='int' must be a JSON number"));
			}
			const int32 IV = static_cast<int32>(Value->AsNumber());
			BB->SetValueAsInt(KeyName, IV);
			Data->SetNumberField(TEXT("set_value"), IV);
			break;
		}
		case EBBType::Float:
		{
			if (Value->Type != EJson::Number)
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					TEXT("`value` for type='float' must be a JSON number"));
			}
			const double Raw = Value->AsNumber();
			BB->SetValueAsFloat(KeyName, static_cast<float>(Raw));
			Data->SetNumberField(TEXT("set_value"), Raw);
			break;
		}
		case EBBType::String:
		{
			if (Value->Type != EJson::String)
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					TEXT("`value` for type='string' must be a JSON string"));
			}
			const FString SV = Value->AsString();
			BB->SetValueAsString(KeyName, SV);
			Data->SetStringField(TEXT("set_value"), SV);
			break;
		}
		case EBBType::Name:
		{
			if (Value->Type != EJson::String)
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					TEXT("`value` for type='name' must be a JSON string"));
			}
			const FString SV = Value->AsString();
			BB->SetValueAsName(KeyName, FName(*SV));
			Data->SetStringField(TEXT("set_value"), SV);
			break;
		}
		case EBBType::Vector:
		{
			double X = 0.0, Y = 0.0, Z = 0.0;
			FString TypeErr;
			if (!TryReadNumberTriple(Value, TEXT("value"), X, Y, Z, TypeErr))
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"), TypeErr);
			}
			BB->SetValueAsVector(KeyName, FVector(X, Y, Z));
			Data->SetField(TEXT("set_value"), MakeNumberTriple(X, Y, Z));
			break;
		}
		case EBBType::Rotator:
		{
			double P = 0.0, Yaw = 0.0, R = 0.0;
			FString TypeErr;
			if (!TryReadNumberTriple(Value, TEXT("value"), P, Yaw, R, TypeErr))
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"), TypeErr);
			}
			BB->SetValueAsRotator(KeyName, FRotator(P, Yaw, R));
			Data->SetField(TEXT("set_value"), MakeNumberTriple(P, Yaw, R));
			break;
		}
		case EBBType::Object:
		{
			// `null` clears; otherwise the string runs through the same
			// ObjectResolver path used for the controller target.
			if (!Value.IsValid() || Value->Type == EJson::Null)
			{
				BB->SetValueAsObject(KeyName, nullptr);
				Data->SetField(TEXT("set_value"), MakeShared<FJsonValueNull>());
				break;
			}
			if (Value->Type != EJson::String)
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					TEXT("`value` for type='object' must be a JSON string (object id/path) or null"));
			}
			const FString ObjStr = Value->AsString();
			UeMcp::FUeMcpResolvedObject ObjResolved =
				UeMcp::ResolveObject(ObjStr, World.World);
			if (!ObjResolved.IsOk())
			{
				return ObjResolved.ErrorInfo.ToSharedRef();
			}
			BB->SetValueAsObject(KeyName, ObjResolved.Object);
			Data->SetStringField(TEXT("set_value"),
				ObjResolved.Object->GetPathName());
			break;
		}
		default:
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("Unhandled blackboard type — registration drift"));
		}

		// Rollback hint — replay set_blackboard with the captured prior
		// value. Only emit when the snapshot succeeded; the unreachable
		// default returns above so PriorValue is valid for every real type.
		if (PriorValue.IsValid())
		{
			TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
			Rollback->SetStringField(TEXT("tool"), TEXT("ai.set_blackboard"));
			TSharedRef<FJsonObject> RollbackArgs = MakeShared<FJsonObject>();
			RollbackArgs->SetStringField(TEXT("target"), TargetId);
			RollbackArgs->SetStringField(TEXT("key"), KeyStr);
			RollbackArgs->SetStringField(TEXT("type"), TypeToWire(Type));
			RollbackArgs->SetField(TEXT("value"), PriorValue);
			Rollback->SetObjectField(TEXT("args"), RollbackArgs);
			Data->SetObjectField(TEXT("rollback"), Rollback);
		}

		return Data;
	}

	/**
	 * `ai.get_blackboard` body. Reads a typed value from a key by name.
	 *
	 * Args: `target` (string), `key` (string), `type` (string),
	 *       `world` (optional).
	 *
	 * Returns: `{value, type, target, key, is_set}`. `is_set` is best-effort:
	 * for vector keys the engine exposes IsVectorValueSet; for everything
	 * else we report `true` whenever the call succeeded.
	 */
	static TSharedRef<FJsonObject> HandleGetBlackboard(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString TargetId;
		if (!Args->TryGetStringField(TEXT("target"), TargetId) || TargetId.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`target` is required and must be a non-empty string"));
		}
		FString KeyStr;
		if (!Args->TryGetStringField(TEXT("key"), KeyStr) || KeyStr.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`key` is required and must be a non-empty string"));
		}
		FString TypeStr;
		if (!Args->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`type` is required (one of %s)"),
					SupportedTypesList()));
		}
		const EBBType Type = ParseType(TypeStr);
		if (Type == EBBType::Invalid)
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`type` must be one of %s; got '%s'"),
					SupportedTypesList(), *TypeStr));
		}

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		TSharedPtr<FJsonObject> Err;
		AAIController* Controller = ResolveAIController(TargetId, World.World, Err);
		if (Controller == nullptr)
		{
			return Err.ToSharedRef();
		}
		UBlackboardComponent* BB = GetBlackboard(Controller, TargetId, Err);
		if (BB == nullptr)
		{
			return Err.ToSharedRef();
		}

		const FName KeyName(*KeyStr);
		if (BB->GetKeyID(KeyName) == FBlackboard::InvalidKey)
		{
			return UeMcp::MakeInlineError(TEXT("KEY_NOT_FOUND"),
				FString::Printf(
					TEXT("Blackboard on '%s' has no key named '%s'"),
					*TargetId, *KeyStr));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("target"), TargetId);
		Data->SetStringField(TEXT("key"), KeyStr);
		Data->SetStringField(TEXT("type"), TypeToWire(Type));
		// Default `is_set` to true; vector path overrides explicitly.
		bool bIsSet = true;

		switch (Type)
		{
		case EBBType::Bool:
			Data->SetBoolField(TEXT("value"), BB->GetValueAsBool(KeyName));
			break;
		case EBBType::Int:
			Data->SetNumberField(TEXT("value"), BB->GetValueAsInt(KeyName));
			break;
		case EBBType::Float:
			Data->SetNumberField(TEXT("value"), BB->GetValueAsFloat(KeyName));
			break;
		case EBBType::String:
			Data->SetStringField(TEXT("value"), BB->GetValueAsString(KeyName));
			break;
		case EBBType::Name:
			Data->SetStringField(TEXT("value"),
				BB->GetValueAsName(KeyName).ToString());
			break;
		case EBBType::Vector:
		{
			bIsSet = BB->IsVectorValueSet(KeyName);
			const FVector V = BB->GetValueAsVector(KeyName);
			if (bIsSet)
			{
				Data->SetField(TEXT("value"), MakeNumberTriple(V.X, V.Y, V.Z));
			}
			else
			{
				Data->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
			}
			break;
		}
		case EBBType::Rotator:
		{
			const FRotator R = BB->GetValueAsRotator(KeyName);
			Data->SetField(TEXT("value"),
				MakeNumberTriple(R.Pitch, R.Yaw, R.Roll));
			break;
		}
		case EBBType::Object:
		{
			UObject* Obj = BB->GetValueAsObject(KeyName);
			if (Obj != nullptr)
			{
				Data->SetStringField(TEXT("value"), Obj->GetPathName());
			}
			else
			{
				Data->SetField(TEXT("value"), MakeShared<FJsonValueNull>());
				bIsSet = false;
			}
			break;
		}
		default:
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("Unhandled blackboard type — registration drift"));
		}

		Data->SetBoolField(TEXT("is_set"), bIsSet);
		return Data;
	}

	/**
	 * `ai.start_bt` body. Loads a UBehaviorTree by `/Game/...` path and
	 * runs it on the resolved controller. Replaces any current tree.
	 *
	 * Args: `target` (string), `bt_path` (string asset path),
	 *       `world` (optional).
	 *
	 * Returns: `{started, target, bt_path, blackboard_attached}`.
	 */
	static TSharedRef<FJsonObject> HandleStartBT(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString TargetId;
		if (!Args->TryGetStringField(TEXT("target"), TargetId) || TargetId.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`target` is required and must be a non-empty string"));
		}
		FString BTPath;
		if (!Args->TryGetStringField(TEXT("bt_path"), BTPath) || BTPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`bt_path` is required and must be a non-empty string"));
		}

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		TSharedPtr<FJsonObject> Err;
		AAIController* Controller = ResolveAIController(TargetId, World.World, Err);
		if (Controller == nullptr)
		{
			return Err.ToSharedRef();
		}

		// Resolve the BT asset. Reuse the ObjectResolver so callers can pass
		// an actor-or-asset string consistently; restrict the result to
		// UBehaviorTree.
		UeMcp::FUeMcpResolvedObject BTResolved =
			UeMcp::ResolveObject(BTPath, World.World);
		if (!BTResolved.IsOk())
		{
			return BTResolved.ErrorInfo.ToSharedRef();
		}
		UBehaviorTree* BT = Cast<UBehaviorTree>(BTResolved.Object);
		if (BT == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("`bt_path` resolved to '%s' (class '%s') — expected a UBehaviorTree asset"),
					*BTPath,
					*BTResolved.Object->GetClass()->GetName()));
		}

		const bool bStarted = Controller->RunBehaviorTree(BT);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("started"), bStarted);
		Data->SetStringField(TEXT("target"), TargetId);
		Data->SetStringField(TEXT("bt_path"), BT->GetPathName());
		Data->SetBoolField(TEXT("blackboard_attached"),
			Controller->GetBlackboardComponent() != nullptr);
		return Data;
	}

	// ==================================================================
	// Issue #15 — blackboard authoring helpers + handlers.
	//
	// Two tools that replace the ~60 lines of inline python_exec in
	// `scripts/ai_smoke.py`:
	//   - `ai.add_blackboard_key`  — append a typed key to a
	//                                `UBlackboardData` asset (creating it).
	//   - `ai.init_blackboard`     — call `UseBlackboard` on a controller
	//                                so a real `UBlackboardComponent` is
	//                                created without a behavior tree.
	// ==================================================================

	/**
	 * Map a wire key-type token to its `UBlackboardKeyType_*` UClass.
	 * Returns nullptr for an unknown token. The supported set is a
	 * superset of the runtime `ai.set_blackboard` types — the same eight
	 * plus the deliberate distinction `class` (UBlackboardKeyType_Class)
	 * vs `object` (UBlackboardKeyType_Object).
	 */
	static UClass* KeyTypeClassFromWire(const FString& Token)
	{
		if (Token.Equals(TEXT("bool"),    ESearchCase::IgnoreCase)) return UBlackboardKeyType_Bool::StaticClass();
		if (Token.Equals(TEXT("int"),     ESearchCase::IgnoreCase)) return UBlackboardKeyType_Int::StaticClass();
		if (Token.Equals(TEXT("float"),   ESearchCase::IgnoreCase)) return UBlackboardKeyType_Float::StaticClass();
		if (Token.Equals(TEXT("string"),  ESearchCase::IgnoreCase)) return UBlackboardKeyType_String::StaticClass();
		if (Token.Equals(TEXT("name"),    ESearchCase::IgnoreCase)) return UBlackboardKeyType_Name::StaticClass();
		if (Token.Equals(TEXT("vector"),  ESearchCase::IgnoreCase)) return UBlackboardKeyType_Vector::StaticClass();
		if (Token.Equals(TEXT("rotator"), ESearchCase::IgnoreCase)) return UBlackboardKeyType_Rotator::StaticClass();
		if (Token.Equals(TEXT("object"),  ESearchCase::IgnoreCase)) return UBlackboardKeyType_Object::StaticClass();
		if (Token.Equals(TEXT("class"),   ESearchCase::IgnoreCase)) return UBlackboardKeyType_Class::StaticClass();
		return nullptr;
	}

	static const TCHAR* SupportedKeyTypesList()
	{
		return TEXT("bool|int|float|string|name|vector|rotator|object|class");
	}

	/**
	 * Resolve a `/Game/...` path to a `UBlackboardData` asset, creating a
	 * fresh one (and its package) when it does not yet exist. The asset is
	 * NOT saved here — the caller decides when to flush (so a batch of
	 * `add_blackboard_key` calls only writes the package once).
	 *
	 * Mirrors the create-package recipe used by `blueprint.create`
	 * (`UeMcpBlueprintAuthoringHandler.cpp` HandleBlueprintCreate):
	 * CreatePackage → FullyLoad → NewObject → AssetRegistry::AssetCreated
	 * → MarkPackageDirty.
	 *
	 * On failure returns nullptr with `OutErr` populated. `bOutCreated`
	 * reports whether a new asset was minted (vs an existing one loaded).
	 */
	static UBlackboardData* ResolveOrCreateBlackboardData(
		const FString& AssetPath,
		bool& bOutCreated,
		TSharedPtr<FJsonObject>& OutErr)
	{
		bOutCreated = false;

		// Normalise: strip a trailing `.AssetName` object suffix so we work
		// with the package path, then derive the object path consistently.
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
			OutErr = UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`blackboard_asset_path` must be a content path like ")
					TEXT("'/Game/AI/BB_Foo' (got '%s')"),
					*AssetPath));
			return nullptr;
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
			OutErr = UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("Could not derive an asset name from '%s'"), *AssetPath));
			return nullptr;
		}

		const FString FullObjectPath = PackagePath + TEXT(".") + ObjectName;

		// Existing asset? Load + return it (type-checked).
		if (UObject* Existing = LoadObject<UObject>(nullptr, *FullObjectPath))
		{
			UBlackboardData* AsBB = Cast<UBlackboardData>(Existing);
			if (AsBB == nullptr)
			{
				OutErr = UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					FString::Printf(
						TEXT("Asset at '%s' is a '%s', not a UBlackboardData"),
						*FullObjectPath, *Existing->GetClass()->GetName()));
				return nullptr;
			}
			return AsBB;
		}

		UPackage* Pkg = CreatePackage(*PackagePath);
		if (Pkg == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(TEXT("CreatePackage('%s') returned null"), *PackagePath));
			return nullptr;
		}
		Pkg->FullyLoad();

		UBlackboardData* NewBB = NewObject<UBlackboardData>(
			Pkg, FName(*ObjectName),
			RF_Public | RF_Standalone | RF_Transactional);
		if (NewBB == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(
					TEXT("NewObject<UBlackboardData>('%s') returned null"),
					*FullObjectPath));
			return nullptr;
		}

		FAssetRegistryModule::AssetCreated(NewBB);
		Pkg->MarkPackageDirty();
		bOutCreated = true;
		return NewBB;
	}

	/** Best-effort save of a loaded asset via the editor asset subsystem. */
	static bool SaveBlackboardAsset(UObject* Asset)
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

	/** Serialize the current key list of a blackboard asset to JSON. */
	static TArray<TSharedPtr<FJsonValue>> DescribeBlackboardKeys(UBlackboardData* BB)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (BB == nullptr)
		{
			return Out;
		}
		for (const FBlackboardEntry& Entry : BB->Keys)
		{
			TSharedRef<FJsonObject> KeyObj = MakeShared<FJsonObject>();
			KeyObj->SetStringField(TEXT("name"), Entry.EntryName.ToString());
			KeyObj->SetStringField(TEXT("type"),
				Entry.KeyType != nullptr
					? Entry.KeyType->GetClass()->GetName()
					: TEXT("(null)"));
			Out.Add(MakeShared<FJsonValueObject>(KeyObj));
		}
		return Out;
	}

	/**
	 * `ai.add_blackboard_key` body. Appends one typed key to a
	 * `UBlackboardData` asset (creating the asset if missing) and saves.
	 *
	 * Args: `blackboard_asset_path` (string), `key_name` (string),
	 *       `key_type` (string — see SupportedKeyTypesList), `save`
	 *       (optional bool, default true).
	 *
	 * Returns: `{ok, blackboard_asset_path, key_name, key_type,
	 *            created_asset, added, saved, keys:[{name,type}]}`.
	 *
	 * Idempotent on the key: an already-present key returns `added:false`
	 * (the engine's `UpdatePersistentKey` is a no-op when the name
	 * exists), so re-running an init sequence is safe.
	 */
	static TSharedRef<FJsonObject> HandleAddBlackboardKey(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("blackboard_asset_path"), AssetPath)
			|| AssetPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`blackboard_asset_path` is required and must be a non-empty string"));
		}
		FString KeyName;
		if (!Args->TryGetStringField(TEXT("key_name"), KeyName) || KeyName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`key_name` is required and must be a non-empty string"));
		}
		FString KeyTypeStr;
		if (!Args->TryGetStringField(TEXT("key_type"), KeyTypeStr) || KeyTypeStr.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`key_type` is required (one of %s)"),
					SupportedKeyTypesList()));
		}
		UClass* KeyClass = KeyTypeClassFromWire(KeyTypeStr);
		if (KeyClass == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`key_type` must be one of %s; got '%s'"),
					SupportedKeyTypesList(), *KeyTypeStr));
		}

		bool bSave = true;
		(void)Args->TryGetBoolField(TEXT("save"), bSave);

		bool bCreated = false;
		TSharedPtr<FJsonObject> Err;
		UBlackboardData* BB =
			ResolveOrCreateBlackboardData(AssetPath, bCreated, Err);
		if (BB == nullptr)
		{
			return Err.ToSharedRef();
		}

		const FName KeyFName(*KeyName);
		const bool bAlreadyExists =
			BB->GetKeyID(KeyFName) != FBlackboard::InvalidKey;

		bool bAdded = false;
		if (!bAlreadyExists)
		{
			// Mirror the body of `UBlackboardData::UpdatePersistentKey<T>`
			// (BlackboardData.h ~L109) but with the concrete requested
			// subclass instead of a fixed template arg: build the entry,
			// `NewObject<KeyClass>(this)` for the typed KeyType, append,
			// mark dirty, propagate to derived assets.
			FBlackboardEntry Entry;
			Entry.EntryName = KeyFName;
			Entry.KeyType = NewObject<UBlackboardKeyType>(BB, KeyClass);
			BB->Keys.Add(Entry);
			BB->MarkPackageDirty();
			BB->PropagateKeyChangesToDerivedBlackboardAssets();
			BB->UpdateKeyIDs();
			bAdded = true;
		}

		bool bSaved = false;
		if (bSave && (bAdded || bCreated))
		{
			bSaved = SaveBlackboardAsset(BB);
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("ok"), true);
		Data->SetStringField(TEXT("blackboard_asset_path"), BB->GetPathName());
		Data->SetStringField(TEXT("key_name"), KeyName);
		Data->SetStringField(TEXT("key_type"), KeyTypeStr.ToLower());
		Data->SetBoolField(TEXT("created_asset"), bCreated);
		Data->SetBoolField(TEXT("added"), bAdded);
		Data->SetBoolField(TEXT("already_existed"), bAlreadyExists);
		Data->SetBoolField(TEXT("saved"), bSaved);
		Data->SetArrayField(TEXT("keys"), DescribeBlackboardKeys(BB));
		return Data;
	}

	/**
	 * `ai.init_blackboard` body. Loads (or creates) a `UBlackboardData`
	 * asset and calls `AAIController::UseBlackboard` so the controller is
	 * given a live `UBlackboardComponent` — WITHOUT needing a behavior
	 * tree. After this, `ai.get_blackboard` / `ai.set_blackboard` work.
	 *
	 * Args: `target` (string — controller or possessed pawn),
	 *       `blackboard_asset_path` (string), `world` (optional).
	 *
	 * Returns: `{ok, target, blackboard_asset_path, created_asset,
	 *            has_bb_component, keys:[{name,type}]}`.
	 */
	static TSharedRef<FJsonObject> HandleInitBlackboard(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString TargetId;
		if (!Args->TryGetStringField(TEXT("target"), TargetId) || TargetId.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`target` is required and must be a non-empty string"));
		}
		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("blackboard_asset_path"), AssetPath)
			|| AssetPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`blackboard_asset_path` is required and must be a non-empty string"));
		}

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		TSharedPtr<FJsonObject> Err;
		AAIController* Controller = ResolveAIController(TargetId, World.World, Err);
		if (Controller == nullptr)
		{
			return Err.ToSharedRef();
		}

		bool bCreated = false;
		UBlackboardData* BB =
			ResolveOrCreateBlackboardData(AssetPath, bCreated, Err);
		if (BB == nullptr)
		{
			return Err.ToSharedRef();
		}

		// Recompute key ID cache before init — InitializeBlackboard reads
		// GetFirstKeyID()/GetNumKeys() and we may have minted the asset
		// just now (no PostLoad ran).
		BB->UpdateKeyIDs();

		UBlackboardComponent* BBComp = nullptr;
		const bool bUsed = Controller->UseBlackboard(BB, BBComp);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("ok"), bUsed);
		Data->SetStringField(TEXT("target"), TargetId);
		Data->SetStringField(TEXT("blackboard_asset_path"), BB->GetPathName());
		Data->SetBoolField(TEXT("created_asset"), bCreated);
		Data->SetBoolField(TEXT("has_bb_component"),
			Controller->GetBlackboardComponent() != nullptr);
		Data->SetArrayField(TEXT("keys"), DescribeBlackboardKeys(BB));
		return Data;
	}
}

void UeMcp::RegisterAIHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpAIHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ai.set_blackboard"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSetBlackboard);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ai.get_blackboard"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleGetBlackboard);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ai.start_bt"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleStartBT);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ai.add_blackboard_key"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleAddBlackboardKey);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ai.init_blackboard"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleInitBlackboard);
		Dispatcher.RegisterTool(Reg);
	}
}
