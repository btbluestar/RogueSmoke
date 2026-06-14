// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// Read/write/list walker inspired by Incurian/AgentBridge (MIT) — clean-room.
//
// The three layers top-down:
//   1. Public entry points (`GetValue`, `SetValue`, `ListPropertyPaths`,
//      `IsWritable`). Each asserts game-thread, does a parse pre-flight, then
//      delegates.
//   2. `ResolveSegments` — the walker. Consumes segments one at a time,
//      descending through structs, objects, arrays and maps. Returns either
//      a leaf-read result (JSON built) or a write-target address.
//   3. Type-dispatched read/write helpers — one per FProperty family, plus
//      five special-struct fast paths.
//
// Critical invariant (Incurian study §2.5, the "CRITICAL" comment): once
// we've resolved through containers (array index, map key, sub-struct member)
// we hold a DIRECT value pointer — not a container. `WriteProperty` (container
// based) and `WritePropertyDirect` (address based) remain split for that
// reason. The walker tracks which we're holding via `ResolutionState::bDirect`.

#include "UeMcpPropertyAccessor.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "Misc/CString.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Field.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace UeMcpAccessorPrivate
{
	/**
	 * Resolver state passed between segments.
	 *
	 * We track both the container (needed to call `WriteProperty` on a single-
	 * segment write) AND the direct value pointer (needed once we've
	 * descended through an array index, map key, or sub-struct member). The
	 * `bDirect` flag distinguishes the two so `SetValue` can pick the right
	 * write helper.
	 */
	struct FResolutionState
	{
		const void* Container = nullptr;
		FProperty* Property = nullptr;
		const void* ValuePtr = nullptr;

		/** Set when `ValuePtr` is a direct address (not a container field). */
		bool bDirect = false;
	};

	/**
	 * Fill an error info struct. Helper kept deliberately NOT named `MakeError` —
	 * UE Core's `TValueOrError::MakeError` is a variadic template that makes
	 * that name an ambiguous overload in a lot of contexts (see
	 * `docs/ue-api-gotchas.md §7`).
	 */
	static void FillError(
		FUeMcpAccessorErrorInfo& Out,
		EUeMcpAccessorError Code,
		FString Message,
		TSharedPtr<FJsonObject> Detail = nullptr)
	{
		Out.Code = Code;
		Out.Message = MoveTemp(Message);
		Out.Detail = Detail;
	}

	/** Case-insensitive property lookup with three strategies (Incurian §2.1). */
	static FProperty* FindPropertyByName(UStruct* Struct, const FString& PropertyName)
	{
		if (!Struct || PropertyName.IsEmpty())
		{
			return nullptr;
		}

		// Direct lookup: matches the C++ field name exactly.
		if (FProperty* Found = Struct->FindPropertyByName(FName(*PropertyName)))
		{
			return Found;
		}

		// Authored-name match strips Blueprint GUID suffixes (`_42_ABC123`).
		// Case-insensitive raw-name fallback catches typing-case drift from
		// callers who typed `health` expecting `Health`.
		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop)
			{
				continue;
			}
			if (Prop->GetAuthoredName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return Prop;
			}
			if (Prop->GetName().Equals(PropertyName, ESearchCase::IgnoreCase))
			{
				return Prop;
			}
		}
		return nullptr;
	}

	/**
	 * Component-by-name fallback on an actor (Incurian §2.2). Matches how
	 * people read actor paths in the editor: `LightComponent` should find
	 * `LightComponent0`. Order: exact → case-insensitive → prefix+trailing-
	 * digits.
	 */
	static UActorComponent* FindComponentByName(AActor* Actor, const FString& ComponentName)
	{
		if (!Actor || ComponentName.IsEmpty())
		{
			return nullptr;
		}

		TInlineComponentArray<UActorComponent*> Components;
		Actor->GetComponents(Components);

		for (UActorComponent* Comp : Components)
		{
			if (Comp && Comp->GetName() == ComponentName)
			{
				return Comp;
			}
		}
		for (UActorComponent* Comp : Components)
		{
			if (Comp && Comp->GetName().Equals(ComponentName, ESearchCase::IgnoreCase))
			{
				return Comp;
			}
		}
		for (UActorComponent* Comp : Components)
		{
			if (!Comp)
			{
				continue;
			}
			const FString CompName = Comp->GetName();
			if (!CompName.StartsWith(ComponentName, ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FString Suffix = CompName.Mid(ComponentName.Len());
			if (Suffix.IsEmpty())
			{
				continue; // handled by the exact/case-insensitive passes
			}
			bool bAllDigits = true;
			for (TCHAR Ch : Suffix)
			{
				if (!FChar::IsDigit(Ch))
				{
					bAllDigits = false;
					break;
				}
			}
			if (bAllDigits)
			{
				return Comp;
			}
		}
		return nullptr;
	}

	/** Is this struct one of the five that we render via ToString/InitFromString? */
	static bool IsSpecialStruct(UScriptStruct* Struct)
	{
		if (!Struct)
		{
			return false;
		}
		return Struct == TBaseStructure<FVector>::Get()
			|| Struct == TBaseStructure<FRotator>::Get()
			|| Struct == TBaseStructure<FTransform>::Get()
			|| Struct == TBaseStructure<FColor>::Get()
			|| Struct == TBaseStructure<FLinearColor>::Get();
	}

	/**
	 * ToString side of the special-struct fast path. Returns false if the
	 * struct isn't one we fast-path; caller falls through to generic struct
	 * rendering.
	 */
	static bool TryReadSpecialStruct(
		UScriptStruct* Struct,
		const void* ValuePtr,
		FUeMcpPropertyValue& OutValue)
	{
		if (!Struct || !ValuePtr)
		{
			return false;
		}
		FString StringForm;
		if (Struct == TBaseStructure<FVector>::Get())
		{
			StringForm = static_cast<const FVector*>(ValuePtr)->ToString();
		}
		else if (Struct == TBaseStructure<FRotator>::Get())
		{
			StringForm = static_cast<const FRotator*>(ValuePtr)->ToString();
		}
		else if (Struct == TBaseStructure<FTransform>::Get())
		{
			StringForm = static_cast<const FTransform*>(ValuePtr)->ToString();
		}
		else if (Struct == TBaseStructure<FColor>::Get())
		{
			StringForm = static_cast<const FColor*>(ValuePtr)->ToString();
		}
		else if (Struct == TBaseStructure<FLinearColor>::Get())
		{
			StringForm = static_cast<const FLinearColor*>(ValuePtr)->ToString();
		}
		else
		{
			return false;
		}

		OutValue.Type = EUeMcpValueType::String;
		OutValue.Json = MakeShared<FJsonValueString>(StringForm);
		return true;
	}

	/** InitFromString side of the special-struct fast path. */
	static bool TryWriteSpecialStruct(
		UScriptStruct* Struct,
		void* ValuePtr,
		const FString& StringForm)
	{
		if (!Struct || !ValuePtr)
		{
			return false;
		}
		if (Struct == TBaseStructure<FVector>::Get())
		{
			return static_cast<FVector*>(ValuePtr)->InitFromString(StringForm);
		}
		if (Struct == TBaseStructure<FRotator>::Get())
		{
			return static_cast<FRotator*>(ValuePtr)->InitFromString(StringForm);
		}
		if (Struct == TBaseStructure<FTransform>::Get())
		{
			return static_cast<FTransform*>(ValuePtr)->InitFromString(StringForm);
		}
		if (Struct == TBaseStructure<FColor>::Get())
		{
			return static_cast<FColor*>(ValuePtr)->InitFromString(StringForm);
		}
		if (Struct == TBaseStructure<FLinearColor>::Get())
		{
			return static_cast<FLinearColor*>(ValuePtr)->InitFromString(StringForm);
		}
		return false;
	}

	/** Standard wrapper around ExportTextItem_Direct for scalar-ish types. */
	static FString ExportTextDirect(FProperty* Property, const void* ValuePtr)
	{
		FString Out;
		if (Property && ValuePtr)
		{
			Property->ExportTextItem_Direct(Out, ValuePtr, nullptr, nullptr, PPF_None);
		}
		return Out;
	}

	// Forward declarations — the read/write path has several mutually
	// recursive helpers; declaring them up front keeps the ordering flexible.
	static TSharedPtr<FJsonValue> ReadPropertyValue(
		FProperty* Property,
		const void* ValuePtr,
		int32 Depth,
		int32 MaxDepth,
		bool bEnumNameDisplay,
		bool& bOutTruncated,
		int64& OutEnumNumeric,
		EUeMcpValueType& OutType);

	static bool WritePropertyValueDirect(
		FProperty* Property,
		void* ValuePtr,
		const TSharedPtr<FJsonValue>& Value,
		FUeMcpAccessorErrorInfo& OutError);

	/**
	 * Render the enum for a given property. Always produces the display form
	 * as the JSON payload (that's the primary `value`); the numeric form is
	 * returned via `OutEnumNumeric` so the wrapper can attach it under a
	 * sibling field if the caller asked for it.
	 */
	static TSharedPtr<FJsonValue> ReadEnumValue(
		FProperty* Property,
		const void* ValuePtr,
		int64& OutEnumNumeric)
	{
		UEnum* Enum = nullptr;
		int64 Numeric = 0;

		if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
		{
			Enum = EnumProp->GetEnum();
			// Underlying int reader operates on the same memory — for an
			// FEnumProperty the "container" and "value" overlap because we
			// got here via `Property->ContainerPtrToValuePtr<void>(Container)`
			// in the walker. The underlying property therefore reads from
			// `ValuePtr` directly (no further Container pointer math).
			FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
			if (Underlying)
			{
				Numeric = Underlying->GetSignedIntPropertyValue(ValuePtr);
			}
		}
		else if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
		{
			Enum = ByteProp->Enum;
			Numeric = ByteProp->GetPropertyValue(ValuePtr);
		}

		OutEnumNumeric = Numeric;

		if (Enum)
		{
			// UE 5.7's `GetNameStringByValue` returns the short value name
			// (`"Walking"`), not the fully-qualified `EState::Walking`. We
			// want the short name as the primary display — matches what the
			// editor shows in the details panel.
			FString Name = Enum->GetNameStringByValue(Numeric);
			if (Name.IsEmpty())
			{
				Name = FString::Printf(TEXT("%lld"), Numeric);
			}
			return MakeShared<FJsonValueString>(Name);
		}
		return MakeShared<FJsonValueNumber>(static_cast<double>(Numeric));
	}

	/** Read the given property into a JSON value. */
	static TSharedPtr<FJsonValue> ReadPropertyValue(
		FProperty* Property,
		const void* ValuePtr,
		int32 Depth,
		int32 MaxDepth,
		bool bEnumNameDisplay,
		bool& bOutTruncated,
		int64& OutEnumNumeric,
		EUeMcpValueType& OutType)
	{
		OutEnumNumeric = 0;
		OutType = EUeMcpValueType::Null;
		if (!Property || !ValuePtr)
		{
			return MakeShared<FJsonValueNull>();
		}

		// Depth gate — emit a placeholder string instead of descending. The
		// walker tracks the `bTruncated` flag for the caller to observe.
		if (Depth >= MaxDepth)
		{
			bOutTruncated = true;
			// Choose a shape-appropriate placeholder so the caller can tell
			// "I hit a container" at a glance without further typechecking.
			if (CastField<FStructProperty>(Property))
			{
				OutType = EUeMcpValueType::String;
				return MakeShared<FJsonValueString>(TEXT("{...}"));
			}
			if (CastField<FArrayProperty>(Property)
				|| CastField<FSetProperty>(Property))
			{
				OutType = EUeMcpValueType::String;
				return MakeShared<FJsonValueString>(TEXT("[...]"));
			}
			if (CastField<FMapProperty>(Property))
			{
				OutType = EUeMcpValueType::String;
				return MakeShared<FJsonValueString>(TEXT("{...}"));
			}
			// Scalars at depth — just render them; depth cap is about
			// collections, scalars don't explode context budgets.
		}

		// Bool.
		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			OutType = EUeMcpValueType::Bool;
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
		}

		// Enum — checked before FNumericProperty because `FByteProperty` can
		// be either a plain byte or an enum-backed byte.
		if (CastField<FEnumProperty>(Property))
		{
			OutType = EUeMcpValueType::Enum;
			(void)bEnumNameDisplay; // primary form is always the display name
			return ReadEnumValue(Property, ValuePtr, OutEnumNumeric);
		}
		if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
		{
			if (ByteProp->Enum)
			{
				OutType = EUeMcpValueType::Enum;
				return ReadEnumValue(Property, ValuePtr, OutEnumNumeric);
			}
			// Plain byte — fall through to numeric.
		}

		// Numeric (int / float / double / byte-without-enum).
		if (FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
		{
			if (NumProp->IsFloatingPoint())
			{
				OutType = EUeMcpValueType::Double;
				return MakeShared<FJsonValueNumber>(NumProp->GetFloatingPointPropertyValue(ValuePtr));
			}
			if (NumProp->IsInteger())
			{
				// JSON numbers are stored as double; int64 survives up to 2^53
				// exactly which covers every reasonable engine property. Out-
				// of-range integers at that scale are a sign of a bug anyway.
				OutType = EUeMcpValueType::Int64;
				const int64 Val = NumProp->GetSignedIntPropertyValue(ValuePtr);
				return MakeShared<FJsonValueNumber>(static_cast<double>(Val));
			}
		}

		// String / Name / Text.
		if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			OutType = EUeMcpValueType::String;
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		}
		if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			OutType = EUeMcpValueType::Name;
			return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
		{
			OutType = EUeMcpValueType::Name;
			return MakeShared<FJsonValueString>(TextProp->GetPropertyValue(ValuePtr).ToString());
		}

		// Object references — emit soft-object-path strings.
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
		{
			OutType = EUeMcpValueType::ObjectRef;
			UObject* Obj = ObjProp->GetObjectPropertyValue(ValuePtr);
			if (!Obj)
			{
				return MakeShared<FJsonValueString>(TEXT(""));
			}
			FSoftObjectPath SoftPath(Obj);
			return MakeShared<FJsonValueString>(SoftPath.ToString());
		}

		// Struct — special-struct fast path first, generic otherwise.
		if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			if (IsSpecialStruct(StructProp->Struct))
			{
				FUeMcpPropertyValue Tmp;
				if (TryReadSpecialStruct(StructProp->Struct, ValuePtr, Tmp))
				{
					OutType = Tmp.Type;
					return Tmp.Json;
				}
			}

			// Generic struct — walk members, build JsonObject. Depth+1 because
			// we're descending into the struct body now.
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (TFieldIterator<FProperty> It(StructProp->Struct); It; ++It)
			{
				FProperty* Member = *It;
				if (!Member)
				{
					continue;
				}
				// Member container is the struct pointer; member value address
				// is obtained via the member's own `ContainerPtrToValuePtr`.
				const void* MemberValue = Member->ContainerPtrToValuePtr<void>(ValuePtr);
				int64 MemberEnumNumeric = 0;
				EUeMcpValueType MemberType = EUeMcpValueType::Null;
				TSharedPtr<FJsonValue> MemberJson = ReadPropertyValue(
					Member, MemberValue, Depth + 1, MaxDepth,
					false, bOutTruncated, MemberEnumNumeric, MemberType);
				Obj->SetField(Member->GetAuthoredName(), MemberJson);
			}
			OutType = EUeMcpValueType::Struct;
			return MakeShared<FJsonValueObject>(Obj);
		}

		// Array.
		if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> JsonElements;
			JsonElements.Reserve(ArrayHelper.Num());
			for (int32 i = 0; i < ArrayHelper.Num(); ++i)
			{
				const void* ElementPtr = ArrayHelper.GetRawPtr(i);
				int64 ElEnum = 0;
				EUeMcpValueType ElType = EUeMcpValueType::Null;
				TSharedPtr<FJsonValue> Elem = ReadPropertyValue(
					ArrayProp->Inner, ElementPtr, Depth + 1, MaxDepth,
					false, bOutTruncated, ElEnum, ElType);
				JsonElements.Add(Elem);
			}
			OutType = EUeMcpValueType::Array;
			return MakeShared<FJsonValueArray>(JsonElements);
		}

		// Set.
		if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
		{
			FScriptSetHelper SetHelper(SetProp, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> JsonElements;
			for (int32 i = 0; i < SetHelper.GetMaxIndex(); ++i)
			{
				if (!SetHelper.IsValidIndex(i))
				{
					continue;
				}
				const void* ElementPtr = SetHelper.GetElementPtr(i);
				int64 ElEnum = 0;
				EUeMcpValueType ElType = EUeMcpValueType::Null;
				TSharedPtr<FJsonValue> Elem = ReadPropertyValue(
					SetProp->ElementProp, ElementPtr, Depth + 1, MaxDepth,
					false, bOutTruncated, ElEnum, ElType);
				JsonElements.Add(Elem);
			}
			OutType = EUeMcpValueType::Array;
			return MakeShared<FJsonValueArray>(JsonElements);
		}

		// Map — string-only keys in v0. Non-string key types yield a JSON
		// object whose key is ExportText_Direct'd. See Incurian study §3.2;
		// gameplay-tag and UObject*-keyed maps are flagged with a type-
		// mismatch error at the walker level, not here.
		if (FMapProperty* MapProp = CastField<FMapProperty>(Property))
		{
			FScriptMapHelper MapHelper(MapProp, ValuePtr);
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			for (int32 i = 0; i < MapHelper.GetMaxIndex(); ++i)
			{
				if (!MapHelper.IsValidIndex(i))
				{
					continue;
				}
				const void* KeyPtr = MapHelper.GetKeyPtr(i);
				const void* ValPtr = MapHelper.GetValuePtr(i);

				// Key — string-only in v0. For FName/FString/FText we read
				// directly; for anything else ExportText yields a stringified
				// form that may not round-trip, and the walker's map-key
				// segment handling flags the mismatch separately.
				FString KeyStr;
				if (CastField<FStrProperty>(MapProp->KeyProp))
				{
					KeyStr = CastField<FStrProperty>(MapProp->KeyProp)->GetPropertyValue(KeyPtr);
				}
				else if (CastField<FNameProperty>(MapProp->KeyProp))
				{
					KeyStr = CastField<FNameProperty>(MapProp->KeyProp)->GetPropertyValue(KeyPtr).ToString();
				}
				else if (CastField<FTextProperty>(MapProp->KeyProp))
				{
					KeyStr = CastField<FTextProperty>(MapProp->KeyProp)->GetPropertyValue(KeyPtr).ToString();
				}
				else
				{
					KeyStr = ExportTextDirect(MapProp->KeyProp, KeyPtr);
				}

				int64 ValEnum = 0;
				EUeMcpValueType ValType = EUeMcpValueType::Null;
				TSharedPtr<FJsonValue> ValJson = ReadPropertyValue(
					MapProp->ValueProp, ValPtr, Depth + 1, MaxDepth,
					false, bOutTruncated, ValEnum, ValType);
				Obj->SetField(KeyStr, ValJson);
			}
			OutType = EUeMcpValueType::Map;
			return MakeShared<FJsonValueObject>(Obj);
		}

		// Fallback: ExportTextItem_Direct. Note: some custom structs with
		// custom import/export don't round-trip cleanly through this path —
		// Incurian has this same limitation. We tolerate it in v0; callers
		// can address the members directly via path syntax.
		OutType = EUeMcpValueType::String;
		return MakeShared<FJsonValueString>(ExportTextDirect(Property, ValuePtr));
	}

	/**
	 * Write a JSON value into a property at a direct address. This is
	 * always called with an already-resolved `ValuePtr` — the walker holds
	 * state on whether to call this path or the container-based one.
	 */
	static bool WritePropertyValueDirect(
		FProperty* Property,
		void* ValuePtr,
		const TSharedPtr<FJsonValue>& Value,
		FUeMcpAccessorErrorInfo& OutError)
	{
		if (!Property || !ValuePtr)
		{
			FillError(OutError, EUeMcpAccessorError::InternalError,
				TEXT("WritePropertyValueDirect: null property or address"));
			return false;
		}
		if (!Value.IsValid())
		{
			FillError(OutError, EUeMcpAccessorError::TypeMismatch,
				TEXT("Cannot write: null JSON value"));
			return false;
		}

		// Bool.
		if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
		{
			bool Bool = false;
			if (Value->Type == EJson::Boolean)
			{
				Bool = Value->AsBool();
			}
			else if (Value->Type == EJson::Number)
			{
				Bool = (Value->AsNumber() != 0.0);
			}
			else if (Value->Type == EJson::String)
			{
				const FString& S = Value->AsString();
				Bool = (S.Equals(TEXT("true"), ESearchCase::IgnoreCase)
					|| S == TEXT("1"));
			}
			else
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					TEXT("Expected bool, got non-scalar JSON"));
				return false;
			}
			BoolProp->SetPropertyValue(ValuePtr, Bool);
			return true;
		}

		// Enum — write accepts either string name (short or `EnumName::Value`)
		// or numeric. Incurian-style: strip up through the last colon if
		// present, then try name lookup, falling back to Atoi64.
		if (CastField<FEnumProperty>(Property) || CastField<FByteProperty>(Property))
		{
			UEnum* Enum = nullptr;
			if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
			{
				Enum = EnumProp->GetEnum();
			}
			else if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
			{
				Enum = ByteProp->Enum;
			}

			if (Enum)
			{
				int64 Numeric = INDEX_NONE;
				if (Value->Type == EJson::Number)
				{
					Numeric = static_cast<int64>(Value->AsNumber());
				}
				else if (Value->Type == EJson::String)
				{
					FString S = Value->AsString();
					int32 ColonPos;
					if (S.FindLastChar(TEXT(':'), ColonPos))
					{
						S = S.Mid(ColonPos + 1);
					}
					Numeric = Enum->GetValueByNameString(S);
					if (Numeric == INDEX_NONE)
					{
						// Try the full-path form first (e.g. `EState::Walking`).
						Numeric = Enum->GetValueByNameString(Value->AsString());
					}
					if (Numeric == INDEX_NONE)
					{
						Numeric = FCString::Atoi64(*S);
					}
				}
				else
				{
					FillError(OutError, EUeMcpAccessorError::TypeMismatch,
						TEXT("Expected enum as string or number"));
					return false;
				}

				if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
				{
					FNumericProperty* Underlying = EnumProp->GetUnderlyingProperty();
					if (Underlying)
					{
						Underlying->SetIntPropertyValue(ValuePtr, Numeric);
						return true;
					}
				}
				else if (FByteProperty* ByteProp = CastField<FByteProperty>(Property))
				{
					ByteProp->SetPropertyValue(ValuePtr, static_cast<uint8>(Numeric));
					return true;
				}
			}
			// FByteProperty with no enum falls through to numeric handling.
			if (CastField<FByteProperty>(Property) && !Enum)
			{
				// Fall through to numeric branch below.
			}
		}

		// Numeric (non-enum).
		if (FNumericProperty* NumProp = CastField<FNumericProperty>(Property))
		{
			double N = 0.0;
			if (Value->Type == EJson::Number)
			{
				N = Value->AsNumber();
			}
			else if (Value->Type == EJson::String)
			{
				N = FCString::Atod(*Value->AsString());
			}
			else if (Value->Type == EJson::Boolean)
			{
				N = Value->AsBool() ? 1.0 : 0.0;
			}
			else
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					TEXT("Expected number for numeric property"));
				return false;
			}

			if (NumProp->IsFloatingPoint())
			{
				NumProp->SetFloatingPointPropertyValue(ValuePtr, N);
			}
			else
			{
				NumProp->SetIntPropertyValue(ValuePtr, static_cast<int64>(N));
			}
			return true;
		}

		// String / Name / Text — accept any scalar JSON, stringify if needed.
		auto CoerceToString = [&](const TSharedPtr<FJsonValue>& V) -> FString
		{
			if (V->Type == EJson::String)
			{
				return V->AsString();
			}
			if (V->Type == EJson::Number)
			{
				// Integer-looking numbers render without trailing .0 so callers
				// reading back get sensible strings.
				const double N = V->AsNumber();
				const double R = FMath::RoundToDouble(N);
				if (FMath::Abs(N - R) < SMALL_NUMBER)
				{
					return FString::Printf(TEXT("%lld"), static_cast<int64>(N));
				}
				return FString::Printf(TEXT("%.17g"), N);
			}
			if (V->Type == EJson::Boolean)
			{
				return V->AsBool() ? TEXT("true") : TEXT("false");
			}
			return FString();
		};

		if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
		{
			StrProp->SetPropertyValue(ValuePtr, CoerceToString(Value));
			return true;
		}
		if (FNameProperty* NameProp = CastField<FNameProperty>(Property))
		{
			NameProp->SetPropertyValue(ValuePtr, FName(*CoerceToString(Value)));
			return true;
		}
		if (FTextProperty* TextProp = CastField<FTextProperty>(Property))
		{
			TextProp->SetPropertyValue(ValuePtr, FText::FromString(CoerceToString(Value)));
			return true;
		}

		// Struct — special-struct fast path first.
		if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
		{
			if (IsSpecialStruct(StructProp->Struct))
			{
				FString AsString;
				if (Value->Type == EJson::String)
				{
					AsString = Value->AsString();
				}
				else
				{
					FillError(OutError, EUeMcpAccessorError::TypeMismatch,
						FString::Printf(TEXT("Expected string form for special struct %s"),
							*StructProp->Struct->GetName()));
					return false;
				}
				if (!TryWriteSpecialStruct(StructProp->Struct, ValuePtr, AsString))
				{
					FillError(OutError, EUeMcpAccessorError::TypeMismatch,
						FString::Printf(TEXT("Failed to parse '%s' into %s"),
							*AsString, *StructProp->Struct->GetName()));
					return false;
				}
				return true;
			}

			// Generic struct — accept a JSON object, assign member-by-member.
			// This is best-effort; some structs with custom import/export
			// won't round-trip. Documented limitation in header.
			if (Value->Type != EJson::Object)
			{
				// Fallback: if the caller passed a string, try ImportText_Direct.
				// Useful when they copy out an ExportText string for an arbitrary
				// struct and paste it back.
				if (Value->Type == EJson::String)
				{
					const TCHAR* ImportResult = Property->ImportText_Direct(
						*Value->AsString(), ValuePtr, nullptr, PPF_None);
					return ImportResult != nullptr;
				}
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					FString::Printf(TEXT("Expected JSON object for struct %s"),
						*StructProp->Struct->GetName()));
				return false;
			}

			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (!Value->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid())
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					TEXT("Expected JSON object for struct"));
				return false;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*ObjPtr)->Values)
			{
				FProperty* Member = FindPropertyByName(StructProp->Struct, P.Key);
				if (!Member)
				{
					continue; // silently ignore unknown member keys
				}
				if (!FUeMcpPropertyAccessor::IsWritable(Member))
				{
					continue;
				}
				void* MemberPtr = Member->ContainerPtrToValuePtr<void>(ValuePtr);
				FUeMcpAccessorErrorInfo MemberErr;
				WritePropertyValueDirect(Member, MemberPtr, P.Value, MemberErr);
			}
			return true;
		}

		// Array — whole-array replace semantics. See header: setting
		// `Tags[2]` writes one element; setting `Tags` with a JSON array
		// replaces the whole array.
		if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
			if (!Value->TryGetArray(ArrPtr) || !ArrPtr)
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					TEXT("Expected JSON array for array property"));
				return false;
			}
			FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
			ArrayHelper.Resize(ArrPtr->Num());
			for (int32 i = 0; i < ArrPtr->Num(); ++i)
			{
				void* ElemPtr = ArrayHelper.GetRawPtr(i);
				FUeMcpAccessorErrorInfo ElemErr;
				WritePropertyValueDirect(ArrayProp->Inner, ElemPtr, (*ArrPtr)[i], ElemErr);
			}
			return true;
		}

		// Set — whole-set replace. Same semantics as array.
		if (FSetProperty* SetProp = CastField<FSetProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
			if (!Value->TryGetArray(ArrPtr) || !ArrPtr)
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					TEXT("Expected JSON array for set property"));
				return false;
			}
			FScriptSetHelper SetHelper(SetProp, ValuePtr);
			SetHelper.EmptyElements();
			for (const TSharedPtr<FJsonValue>& ElemVal : *ArrPtr)
			{
				int32 NewIdx = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
				void* ElemPtr = SetHelper.GetElementPtr(NewIdx);
				FUeMcpAccessorErrorInfo ElemErr;
				WritePropertyValueDirect(SetProp->ElementProp, ElemPtr, ElemVal, ElemErr);
			}
			SetHelper.Rehash();
			return true;
		}

		// Map — whole-map replace. Key must be string-coercible.
		if (FMapProperty* MapProp = CastField<FMapProperty>(Property))
		{
			if (Value->Type != EJson::Object)
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					TEXT("Expected JSON object for map property"));
				return false;
			}
			const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
			if (!Value->TryGetObject(ObjPtr) || !ObjPtr || !ObjPtr->IsValid())
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					TEXT("Expected JSON object for map"));
				return false;
			}
			FScriptMapHelper MapHelper(MapProp, ValuePtr);
			MapHelper.EmptyValues();
			for (const TPair<FString, TSharedPtr<FJsonValue>>& KV : (*ObjPtr)->Values)
			{
				const int32 NewIdx = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
				void* KeyPtr = MapHelper.GetKeyPtr(NewIdx);
				void* MapValPtr = MapHelper.GetValuePtr(NewIdx);

				// Key writer: string-only. If the key property isn't one of
				// {Str, Name, Text} we fall through to ImportText_Direct which
				// may fail silently on structs (documented limitation — §map
				// keys in handler-conventions).
				if (FStrProperty* KeyStr = CastField<FStrProperty>(MapProp->KeyProp))
				{
					KeyStr->SetPropertyValue(KeyPtr, KV.Key);
				}
				else if (FNameProperty* KeyName = CastField<FNameProperty>(MapProp->KeyProp))
				{
					KeyName->SetPropertyValue(KeyPtr, FName(*KV.Key));
				}
				else if (FTextProperty* KeyText = CastField<FTextProperty>(MapProp->KeyProp))
				{
					KeyText->SetPropertyValue(KeyPtr, FText::FromString(KV.Key));
				}
				else
				{
					MapProp->KeyProp->ImportText_Direct(*KV.Key, KeyPtr, nullptr, PPF_None);
				}

				FUeMcpAccessorErrorInfo ValErr;
				WritePropertyValueDirect(MapProp->ValueProp, MapValPtr, KV.Value, ValErr);
			}
			MapHelper.Rehash();
			return true;
		}

		// Object references — write a soft-object-path string.
		if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Property))
		{
			FString PathStr;
			if (Value->Type == EJson::String)
			{
				PathStr = Value->AsString();
			}
			else if (Value->Type == EJson::Null)
			{
				PathStr = FString();
			}
			else
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					TEXT("Expected string (object path) for object reference"));
				return false;
			}

			// Soft paths keep the path as-is; hard paths resolve first.
			// Braced-init avoids the most-vexing-parse on the FSoftObjectPtr
			// constructor (which has a variadic template that C++ otherwise
			// reads as a function declaration).
			if (FSoftObjectProperty* SoftProp = CastField<FSoftObjectProperty>(Property))
			{
				const FSoftObjectPath SoftPath{PathStr};
				FSoftObjectPtr SoftPtr{SoftPath};
				SoftProp->SetPropertyValue(ValuePtr, SoftPtr);
				return true;
			}
			FSoftObjectPath SoftPath(PathStr);
			UObject* Loaded = PathStr.IsEmpty() ? nullptr : SoftPath.ResolveObject();
			if (!Loaded && !PathStr.IsEmpty())
			{
				Loaded = SoftPath.TryLoad();
			}
			if (Loaded && ObjProp->PropertyClass && !Loaded->IsA(ObjProp->PropertyClass))
			{
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					FString::Printf(TEXT("Object at '%s' (%s) is not a %s"),
						*PathStr, *Loaded->GetClass()->GetName(),
						*ObjProp->PropertyClass->GetName()));
				return false;
			}
			ObjProp->SetObjectPropertyValue(ValuePtr, Loaded);
			return true;
		}

		// Last resort — ImportText_Direct on a string form.
		if (Value->Type == EJson::String)
		{
			const TCHAR* ImportResult = Property->ImportText_Direct(
				*Value->AsString(), ValuePtr, nullptr, PPF_None);
			return ImportResult != nullptr;
		}

		FillError(OutError, EUeMcpAccessorError::TypeMismatch,
			FString::Printf(TEXT("Unsupported property type for write: %s"),
				*Property->GetCPPType()));
		return false;
	}

	/**
	 * Resolve segments starting from an object. Handles synthesized tokens at
	 * the start (`@Class`/`@Components`/`@CDO`) and the component-by-name
	 * fallback on the first segment.
	 *
	 * On success with `bForWrite=false`, the state fields pointing at the
	 * property value are populated — caller reads from `State.ValuePtr` via
	 * `State.Property`.
	 *
	 * On success with `bForWrite=true`, `State.ValuePtr` and `State.Property`
	 * identify the write target; `State.bDirect` tells the caller whether to
	 * write via the container-based or address-based helper.
	 *
	 * Synthesized segments are read-only; attempting to write through one
	 * returns `ReadOnlySegment`.
	 */
	static bool ResolveObjectSegments(
		UObject* Root,
		const TArray<FUeMcpPathSegment>& Segments,
		bool bForWrite,
		FResolutionState& OutState,
		// Set when we handled the whole path as a synthesized read-only leaf
		// (e.g. `@Class`). The caller short-circuits and uses `OutSyntheticJson`.
		TSharedPtr<FJsonValue>& OutSyntheticJson,
		FUeMcpAccessorErrorInfo& OutError)
	{
		if (!Root)
		{
			FillError(OutError, EUeMcpAccessorError::NullObject,
				TEXT("Root object is null"));
			return false;
		}
		if (Segments.Num() == 0)
		{
			FillError(OutError, EUeMcpAccessorError::InvalidPath,
				TEXT("Empty path"));
			return false;
		}

		const FUeMcpPathSegment& First = Segments[0];

		// Synthesized token handling. `@Class` and `@Components` are terminal;
		// `@CDO` redirects root and then walks remaining segments normally.
		if (First.Kind == EUeMcpPathSegmentKind::Synthesized)
		{
			if (First.Name.Equals(TEXT("Class"), ESearchCase::IgnoreCase))
			{
				if (bForWrite)
				{
					FillError(OutError, EUeMcpAccessorError::ReadOnlySegment,
						TEXT("@Class is read-only"));
					return false;
				}
				OutSyntheticJson = MakeShared<FJsonValueString>(
					Root->GetClass() ? Root->GetClass()->GetPathName() : FString());
				return true;
			}
			if (First.Name.Equals(TEXT("Components"), ESearchCase::IgnoreCase))
			{
				if (bForWrite)
				{
					FillError(OutError, EUeMcpAccessorError::ReadOnlySegment,
						TEXT("@Components is read-only"));
					return false;
				}
				TArray<TSharedPtr<FJsonValue>> Entries;
				if (AActor* Actor = Cast<AActor>(Root))
				{
					TInlineComponentArray<UActorComponent*> Comps;
					Actor->GetComponents(Comps);
					for (UActorComponent* C : Comps)
					{
						if (C)
						{
							Entries.Add(MakeShared<FJsonValueString>(C->GetName()));
						}
					}
				}
				OutSyntheticJson = MakeShared<FJsonValueArray>(Entries);
				return true;
			}
			if (First.Name.Equals(TEXT("CDO"), ESearchCase::IgnoreCase))
			{
				UObject* CDO = Root->GetClass() ? Root->GetClass()->GetDefaultObject() : nullptr;
				if (!CDO)
				{
					FillError(OutError, EUeMcpAccessorError::NullObject,
						TEXT("No CDO available for object's class"));
					return false;
				}
				if (Segments.Num() == 1)
				{
					// `@CDO` alone — emit the class path, same as @Class would.
					if (bForWrite)
					{
						FillError(OutError, EUeMcpAccessorError::ReadOnlySegment,
							TEXT("@CDO alone is read-only; append a property path to address"));
						return false;
					}
					OutSyntheticJson = MakeShared<FJsonValueString>(
						CDO->GetClass() ? CDO->GetClass()->GetPathName() : FString());
					return true;
				}
				// Recurse into the CDO with the remaining segments.
				TArray<FUeMcpPathSegment> Remaining;
				Remaining.Reserve(Segments.Num() - 1);
				for (int32 i = 1; i < Segments.Num(); ++i)
				{
					Remaining.Add(Segments[i]);
				}
				return ResolveObjectSegments(CDO, Remaining, bForWrite, OutState,
					OutSyntheticJson, OutError);
			}
			// Should have been caught at parse time.
			FillError(OutError, EUeMcpAccessorError::InvalidPath,
				FString::Printf(TEXT("Unknown synthesized token '@%s'"), *First.Name));
			return false;
		}

		// Non-synthesized first segment must be a property name (or a component
		// fallback on an actor root).
		if (First.Kind != EUeMcpPathSegmentKind::Property)
		{
			FillError(OutError, EUeMcpAccessorError::InvalidPath,
				TEXT("Path must start with a property name"));
			return false;
		}

		FProperty* FirstProp = FindPropertyByName(Root->GetClass(), First.Name);
		if (!FirstProp)
		{
			// Component fallback. Only on the first segment; intermediate
			// components must be reflected `UPROPERTY`s (same rule as Incurian).
			if (AActor* Actor = Cast<AActor>(Root))
			{
				if (UActorComponent* Comp = FindComponentByName(Actor, First.Name))
				{
					if (Segments.Num() == 1)
					{
						// The path resolved to the component itself — emit a
						// path string under read. Writes to a whole component
						// aren't meaningful.
						if (bForWrite)
						{
							FillError(OutError, EUeMcpAccessorError::TypeMismatch,
								TEXT("Cannot write to a bare component reference"));
							return false;
						}
						OutSyntheticJson = MakeShared<FJsonValueString>(
							FSoftObjectPath(Comp).ToString());
						return true;
					}
					TArray<FUeMcpPathSegment> Remaining;
					Remaining.Reserve(Segments.Num() - 1);
					for (int32 i = 1; i < Segments.Num(); ++i)
					{
						Remaining.Add(Segments[i]);
					}
					return ResolveObjectSegments(Comp, Remaining, bForWrite, OutState,
						OutSyntheticJson, OutError);
				}
			}

			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			// Include the class name for diagnostic — agents can use the
			// class name to look up candidates via `list_property_paths`.
			Detail->SetStringField(TEXT("class"), Root->GetClass()->GetName());
			FillError(OutError, EUeMcpAccessorError::PropNotFound,
				FString::Printf(TEXT("Property '%s' not found on %s"),
					*First.Name, *Root->GetClass()->GetName()),
				Detail);
			return false;
		}

		OutState.Container = Root;
		OutState.Property = FirstProp;
		OutState.ValuePtr = FirstProp->ContainerPtrToValuePtr<void>(Root);
		OutState.bDirect = false;

		// If this is the last segment, we're done — caller reads or writes
		// using `{Container, Property}` (container-based) or `ValuePtr`
		// (direct). For a single-segment path, prefer the container-based
		// path on writes since it's equivalent and matches Incurian's shape.
		if (Segments.Num() == 1)
		{
			return true;
		}

		// Walk remaining segments.
		for (int32 SegIdx = 1; SegIdx < Segments.Num(); ++SegIdx)
		{
			const FUeMcpPathSegment& Seg = Segments[SegIdx];

			if (Seg.Kind == EUeMcpPathSegmentKind::Synthesized)
			{
				FillError(OutError, EUeMcpAccessorError::InvalidPath,
					TEXT("Synthesized tokens can only appear as the first segment"));
				return false;
			}

			if (Seg.Kind == EUeMcpPathSegmentKind::Property)
			{
				// Property continuation — either we're walking into a sub-struct
				// or we're crossing an object reference into a new UObject.
				if (FStructProperty* StructProp = CastField<FStructProperty>(OutState.Property))
				{
					FProperty* NestedProp = FindPropertyByName(StructProp->Struct, Seg.Name);
					if (!NestedProp)
					{
						FillError(OutError, EUeMcpAccessorError::PropNotFound,
							FString::Printf(TEXT("Property '%s' not found in struct '%s'"),
								*Seg.Name, *StructProp->Struct->GetName()));
						return false;
					}
					// Nested property's container is the struct pointer we were
					// holding; the new value pointer is obtained directly.
					const void* StructPtr = OutState.ValuePtr;
					OutState.Container = StructPtr;
					OutState.Property = NestedProp;
					OutState.ValuePtr = NestedProp->ContainerPtrToValuePtr<void>(StructPtr);
					OutState.bDirect = false;
					continue;
				}
				if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(OutState.Property))
				{
					// Dereference the object reference and continue from the
					// referenced UObject. `GetObjectPropertyValue` is given
					// the direct-value pointer for the object reference field.
					UObject* Nested = ObjProp->GetObjectPropertyValue(OutState.ValuePtr);
					if (!Nested)
					{
						FillError(OutError, EUeMcpAccessorError::NullObject,
							FString::Printf(TEXT("Object reference '%s' is null"),
								*OutState.Property->GetName()));
						return false;
					}
					// Reset the walk at the nested object and build the
					// remaining-segments slice.
					TArray<FUeMcpPathSegment> Remaining;
					Remaining.Reserve(Segments.Num() - SegIdx);
					for (int32 j = SegIdx; j < Segments.Num(); ++j)
					{
						Remaining.Add(Segments[j]);
					}
					return ResolveObjectSegments(Nested, Remaining, bForWrite, OutState,
						OutSyntheticJson, OutError);
				}
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					FString::Printf(TEXT("Cannot descend into '%s' — not a struct or object reference"),
						*OutState.Property->GetName()));
				return false;
			}

			if (Seg.Kind == EUeMcpPathSegmentKind::ArrayIndex)
			{
				if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(OutState.Property))
				{
					// `ValuePtr` for an array IS the FScriptArray address when
					// we got here via `ContainerPtrToValuePtr`. Indexing hands
					// us a direct pointer to one element.
					FScriptArrayHelper ArrayHelper(ArrayProp, const_cast<void*>(OutState.ValuePtr));
					if (Seg.Index < 0 || Seg.Index >= ArrayHelper.Num())
					{
						TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
						Detail->SetNumberField(TEXT("actual_size"), ArrayHelper.Num());
						Detail->SetNumberField(TEXT("requested_index"), Seg.Index);
						FillError(OutError, EUeMcpAccessorError::IndexOOB,
							FString::Printf(TEXT("Array index %d out of bounds (size %d)"),
								Seg.Index, ArrayHelper.Num()),
							Detail);
						return false;
					}
					OutState.Container = OutState.ValuePtr; // the FScriptArray
					OutState.Property = ArrayProp->Inner;
					OutState.ValuePtr = ArrayHelper.GetRawPtr(Seg.Index);
					OutState.bDirect = true;
					continue;
				}
				if (FSetProperty* SetProp = CastField<FSetProperty>(OutState.Property))
				{
					FScriptSetHelper SetHelper(SetProp, const_cast<void*>(OutState.ValuePtr));
					// Sets expose a "max index" that includes holes; we map
					// dense indices to valid slots so `[0]` means "first live
					// element". Callers rarely index into sets by position,
					// so this is a best-effort convenience.
					int32 DenseIdx = 0;
					int32 FoundIdx = INDEX_NONE;
					for (int32 i = 0; i < SetHelper.GetMaxIndex(); ++i)
					{
						if (!SetHelper.IsValidIndex(i))
						{
							continue;
						}
						if (DenseIdx == Seg.Index)
						{
							FoundIdx = i;
							break;
						}
						DenseIdx++;
					}
					if (FoundIdx == INDEX_NONE)
					{
						TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
						Detail->SetNumberField(TEXT("actual_size"), SetHelper.Num());
						Detail->SetNumberField(TEXT("requested_index"), Seg.Index);
						FillError(OutError, EUeMcpAccessorError::IndexOOB,
							FString::Printf(TEXT("Set index %d out of bounds (size %d)"),
								Seg.Index, SetHelper.Num()),
							Detail);
						return false;
					}
					OutState.Container = OutState.ValuePtr;
					OutState.Property = SetProp->ElementProp;
					OutState.ValuePtr = SetHelper.GetElementPtr(FoundIdx);
					OutState.bDirect = true;
					continue;
				}
				FillError(OutError, EUeMcpAccessorError::TypeMismatch,
					FString::Printf(TEXT("Cannot index '%s' — not an array or set"),
						*OutState.Property->GetName()));
				return false;
			}

			if (Seg.Kind == EUeMcpPathSegmentKind::MapKey)
			{
				FMapProperty* MapProp = CastField<FMapProperty>(OutState.Property);
				if (!MapProp)
				{
					FillError(OutError, EUeMcpAccessorError::TypeMismatch,
						FString::Printf(TEXT("Cannot key '%s' — not a map"),
							*OutState.Property->GetName()));
					return false;
				}
				// Non-string key types are a stated v0 restriction. Flagging
				// the specific key type in `detail.key_type` lets the caller
				// surface a precise error.
				const bool bKeyIsString =
					CastField<FStrProperty>(MapProp->KeyProp) ||
					CastField<FNameProperty>(MapProp->KeyProp) ||
					CastField<FTextProperty>(MapProp->KeyProp);
				if (!bKeyIsString)
				{
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("key_type"), MapProp->KeyProp->GetCPPType());
					FillError(OutError, EUeMcpAccessorError::TypeMismatch,
						FString::Printf(TEXT("v0 map keys must be FString/FName/FText, got %s"),
							*MapProp->KeyProp->GetCPPType()),
						Detail);
					return false;
				}

				FScriptMapHelper MapHelper(MapProp, const_cast<void*>(OutState.ValuePtr));
				void* FoundVal = nullptr;
				for (int32 i = 0; i < MapHelper.GetMaxIndex(); ++i)
				{
					if (!MapHelper.IsValidIndex(i))
					{
						continue;
					}
					const void* KeyPtr = MapHelper.GetKeyPtr(i);
					FString KeyStr;
					if (CastField<FStrProperty>(MapProp->KeyProp))
					{
						KeyStr = CastField<FStrProperty>(MapProp->KeyProp)->GetPropertyValue(KeyPtr);
					}
					else if (CastField<FNameProperty>(MapProp->KeyProp))
					{
						KeyStr = CastField<FNameProperty>(MapProp->KeyProp)->GetPropertyValue(KeyPtr).ToString();
					}
					else if (CastField<FTextProperty>(MapProp->KeyProp))
					{
						KeyStr = CastField<FTextProperty>(MapProp->KeyProp)->GetPropertyValue(KeyPtr).ToString();
					}
					if (KeyStr.Equals(Seg.Key, ESearchCase::CaseSensitive))
					{
						FoundVal = MapHelper.GetValuePtr(i);
						break;
					}
				}
				if (!FoundVal)
				{
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("missing_key"), Seg.Key);
					FillError(OutError, EUeMcpAccessorError::KeyNotFound,
						FString::Printf(TEXT("Key '%s' not found in map"), *Seg.Key),
						Detail);
					return false;
				}
				OutState.Container = OutState.ValuePtr;
				OutState.Property = MapProp->ValueProp;
				OutState.ValuePtr = FoundVal;
				OutState.bDirect = true;
				continue;
			}
		}

		return true;
	}

	/** Helper used by ListPropertyPaths — recursively append addressable paths. */
	static void CollectPaths(
		UStruct* Struct,
		const FString& Prefix,
		int32 Depth,
		int32 MaxDepth,
		TArray<FString>& OutPaths)
	{
		if (!Struct || Depth >= MaxDepth)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* Prop = *It;
			if (!Prop)
			{
				continue;
			}
			const FString Name = Prop->GetAuthoredName();
			const FString Full = Prefix.IsEmpty() ? Name : FString::Printf(TEXT("%s.%s"), *Prefix, *Name);

			// Special-struct fast-path leaves emit as atomic paths (one path,
			// the string form). Non-special structs emit both themselves and
			// their members.
			if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				if (IsSpecialStruct(StructProp->Struct))
				{
					OutPaths.Add(Full);
				}
				else
				{
					OutPaths.Add(Full);
					CollectPaths(StructProp->Struct, Full, Depth + 1, MaxDepth, OutPaths);
				}
				continue;
			}

			// Containers — emit the container path only. Element-level paths
			// depend on runtime contents (indices, keys), which the caller
			// discovers with a `GetValue` on the container.
			if (CastField<FArrayProperty>(Prop)
				|| CastField<FSetProperty>(Prop)
				|| CastField<FMapProperty>(Prop))
			{
				OutPaths.Add(Full);
				continue;
			}

			// Scalars and object refs — single addressable path.
			OutPaths.Add(Full);
		}
	}
} // namespace UeMcpAccessorPrivate

bool FUeMcpPropertyAccessor::IsWritable(const FProperty* Property)
{
	if (!Property)
	{
		return false;
	}
	// Incurian §2.4: CPF_BlueprintReadOnly is NOT a write barrier for us.
	// We're editor code, not Blueprint script. Tests need to poke gameplay
	// state that BP designers intentionally mark read-only.
	return !Property->HasAnyPropertyFlags(CPF_EditConst);
}

bool FUeMcpPropertyAccessor::GetValue(
	UObject* Root,
	const FString& PathString,
	FUeMcpPropertyValue& OutValue,
	FUeMcpAccessorErrorInfo& OutError,
	const FUeMcpReadOptions& Options)
{
	check(IsInGameThread());
	using namespace UeMcpAccessorPrivate;

	TArray<FUeMcpPathSegment> Segments;
	FUeMcpPathParseError ParseErr;
	if (!FUeMcpPropertyPath::ParsePath(PathString, Segments, &ParseErr))
	{
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetNumberField(TEXT("position"), ParseErr.Position);
		FillError(OutError, EUeMcpAccessorError::InvalidPath, ParseErr.Message, Detail);
		return false;
	}

	FResolutionState State;
	TSharedPtr<FJsonValue> Synthetic;
	if (!ResolveObjectSegments(Root, Segments, /*bForWrite=*/ false, State, Synthetic, OutError))
	{
		return false;
	}

	// Synthesized leaf resolution (e.g. `@Class`, `@Components`, bare component).
	if (Synthetic.IsValid())
	{
		OutValue = FUeMcpPropertyValue::FromJson(Synthetic,
			Synthetic->Type == EJson::Array ? EUeMcpValueType::Array
			: EUeMcpValueType::String);
		return true;
	}

	if (!State.Property || !State.ValuePtr)
	{
		FillError(OutError, EUeMcpAccessorError::InternalError,
			TEXT("Resolver succeeded but produced a null property/value"));
		return false;
	}

	bool bTruncated = false;
	int64 EnumNumeric = 0;
	EUeMcpValueType ValueType = EUeMcpValueType::Null;

	// The :enum_name suffix only meaningfully applies at the leaf; propagate
	// it from the last segment.
	const bool bEnumNameDisplay = Segments.Last().bEnumNameDisplay;

	TSharedPtr<FJsonValue> Json = ReadPropertyValue(
		State.Property, State.ValuePtr,
		/*Depth=*/ 0, Options.MaxDepth,
		bEnumNameDisplay, bTruncated, EnumNumeric, ValueType);

	OutValue.Json = Json;
	OutValue.Type = ValueType;
	OutValue.EnumNumeric = EnumNumeric;
	OutValue.bTruncated = bTruncated;
	return true;
}

bool FUeMcpPropertyAccessor::SetValue(
	UObject* Root,
	const FString& PathString,
	const TSharedPtr<FJsonValue>& InValue,
	FUeMcpAccessorErrorInfo& OutError)
{
	check(IsInGameThread());
	using namespace UeMcpAccessorPrivate;

	TArray<FUeMcpPathSegment> Segments;
	FUeMcpPathParseError ParseErr;
	if (!FUeMcpPropertyPath::ParsePath(PathString, Segments, &ParseErr))
	{
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetNumberField(TEXT("position"), ParseErr.Position);
		FillError(OutError, EUeMcpAccessorError::InvalidPath, ParseErr.Message, Detail);
		return false;
	}

	FResolutionState State;
	TSharedPtr<FJsonValue> Synthetic;
	if (!ResolveObjectSegments(Root, Segments, /*bForWrite=*/ true, State, Synthetic, OutError))
	{
		return false;
	}

	// If the whole path resolved as a synthesized read-only (shouldn't happen
	// because ResolveObjectSegments flags that path with bForWrite=true, but
	// defensive check in case a future change slips through).
	if (Synthetic.IsValid() && !State.Property)
	{
		FillError(OutError, EUeMcpAccessorError::ReadOnlySegment,
			TEXT("Cannot write through a synthesized segment"));
		return false;
	}

	if (!State.Property || !State.ValuePtr)
	{
		FillError(OutError, EUeMcpAccessorError::InternalError,
			TEXT("Write resolver produced null target"));
		return false;
	}

	if (!IsWritable(State.Property))
	{
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetBoolField(TEXT("edit_const"),
			State.Property->HasAnyPropertyFlags(CPF_EditConst));
		FillError(OutError, EUeMcpAccessorError::NotWritable,
			FString::Printf(TEXT("Property '%s' is CPF_EditConst"),
				*State.Property->GetName()),
			Detail);
		return false;
	}

	// Single-segment writes use the container-based form (we held a Container
	// pointer the whole way). Multi-segment walks that crossed a container
	// (array, map, sub-struct) leave `bDirect=true`; we write at the direct
	// address in that case — matches Incurian §2.5's CRITICAL note.
	void* Target = const_cast<void*>(
		State.bDirect
			? State.ValuePtr
			: State.Property->ContainerPtrToValuePtr<void>(const_cast<void*>(State.Container)));

	FUeMcpAccessorErrorInfo WriteErr;
	if (!WritePropertyValueDirect(State.Property, Target, InValue, WriteErr))
	{
		OutError = WriteErr;
		return false;
	}
	return true;
}

bool FUeMcpPropertyAccessor::ListPropertyPaths(
	UObject* Root,
	TArray<FString>& OutPaths,
	FUeMcpAccessorErrorInfo& OutError,
	const FUeMcpListPathsOptions& Options)
{
	check(IsInGameThread());
	using namespace UeMcpAccessorPrivate;

	if (!Root)
	{
		FillError(OutError, EUeMcpAccessorError::NullObject, TEXT("Root object is null"));
		return false;
	}

	OutPaths.Reset();

	CollectPaths(Root->GetClass(), FString(), /*Depth=*/ 0, Options.MaxDepth, OutPaths);

	// Component-name paths — emit one "ComponentName" entry per component on
	// an actor root. The caller can descend into each with a separate call.
	if (AActor* Actor = Cast<AActor>(Root))
	{
		TInlineComponentArray<UActorComponent*> Comps;
		Actor->GetComponents(Comps);
		for (UActorComponent* C : Comps)
		{
			if (C)
			{
				OutPaths.Add(C->GetName());
			}
		}
	}

	if (Options.bIncludeSynthesized)
	{
		OutPaths.Add(TEXT("@Class"));
		if (Cast<AActor>(Root))
		{
			OutPaths.Add(TEXT("@Components"));
		}
		OutPaths.Add(TEXT("@CDO"));
	}

	return true;
}

// THE single source of truth for accessor-error -> wire-code. Contract
// (the doc-comment table) lives on the declaration in
// `UeMcpPropertyAccessor.h`. Exhaustive switch with NO `default:` so any
// future enumerator fails `-Wswitch` and forces a mapping decision here.
FString UeMcp::AccessorErrorToCode(EUeMcpAccessorError Err)
{
	switch (Err)
	{
		case EUeMcpAccessorError::InvalidPath:     return TEXT("SCHEMA_ERROR");
		case EUeMcpAccessorError::NullObject:      return TEXT("NOT_FOUND");
		case EUeMcpAccessorError::PropNotFound:    return TEXT("PROP_NOT_FOUND");
		case EUeMcpAccessorError::IndexOOB:        return TEXT("INDEX_OOB");
		case EUeMcpAccessorError::KeyNotFound:     return TEXT("KEY_NOT_FOUND");
		case EUeMcpAccessorError::TypeMismatch:    return TEXT("TYPE_MISMATCH");
		case EUeMcpAccessorError::NotWritable:     return TEXT("TYPE_MISMATCH");
		case EUeMcpAccessorError::ReadOnlySegment: return TEXT("SCHEMA_ERROR");
		case EUeMcpAccessorError::DepthExceeded:   return TEXT("TYPE_MISMATCH");
		case EUeMcpAccessorError::InternalError:   return TEXT("PLUGIN_INTERNAL_ERROR");
		// Mapping Success is a programming error; map defensively, do not crash.
		case EUeMcpAccessorError::Success:         return TEXT("PLUGIN_INTERNAL_ERROR");
	}
	// unreachable: switch is exhaustive (no default: so new enumerators fail -Wswitch)
	return TEXT("PLUGIN_INTERNAL_ERROR");
}
