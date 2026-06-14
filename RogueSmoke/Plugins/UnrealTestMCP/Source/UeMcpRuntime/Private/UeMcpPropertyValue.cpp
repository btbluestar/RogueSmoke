// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// Transport union helpers inspired by Incurian/AgentBridge (MIT) — clean-room.
//
// Deliberately tiny. The header explains why there's no TVariant here: the
// accessor already decides what kind of JSON to emit, and this carrier just
// preserves one semantic-type tag alongside that JSON. Values flow one-way
// through the accessor, so this file's job is mostly the null/from-JSON
// conversion used on the write path.

#include "UeMcpPropertyValue.h"

#include "Dom/JsonValue.h"

FUeMcpPropertyValue FUeMcpPropertyValue::MakeNull()
{
	FUeMcpPropertyValue V;
	V.Type = EUeMcpValueType::Null;
	V.Json = MakeShared<FJsonValueNull>();
	return V;
}

FUeMcpPropertyValue FUeMcpPropertyValue::FromJson(TSharedPtr<FJsonValue> InJson, EUeMcpValueType InType)
{
	FUeMcpPropertyValue V;
	V.Type = InType;
	V.Json = InJson.IsValid() ? InJson : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());
	return V;
}

FUeMcpPropertyValue UeMcpValueFromJson(const TSharedPtr<FJsonValue>& InJson)
{
	// Shape inference: we pick the most specific type we can see without
	// type context. The accessor overrides this when it knows the target
	// FProperty demands coercion (e.g. a JSON number going into an
	// FEnumProperty is reinterpreted at write time).
	FUeMcpPropertyValue V;
	V.Json = InJson.IsValid() ? InJson : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>());

	if (!InJson.IsValid() || InJson->Type == EJson::Null)
	{
		V.Type = EUeMcpValueType::Null;
		return V;
	}

	switch (InJson->Type)
	{
	case EJson::Boolean:
		V.Type = EUeMcpValueType::Bool;
		break;
	case EJson::Number:
	{
		// We cannot distinguish int vs double at JSON parse time — UE's JSON
		// parser normalises numbers to double. The accessor decides whether
		// the target property is integer-typed and truncates accordingly.
		const double N = InJson->AsNumber();
		const double Rounded = FMath::RoundToDouble(N);
		const bool bIsIntegral = (FMath::Abs(N - Rounded) < SMALL_NUMBER);
		V.Type = bIsIntegral ? EUeMcpValueType::Int64 : EUeMcpValueType::Double;
		break;
	}
	case EJson::String:
		V.Type = EUeMcpValueType::String;
		break;
	case EJson::Array:
		V.Type = EUeMcpValueType::Array;
		break;
	case EJson::Object:
		V.Type = EUeMcpValueType::Map; // ambiguous: could be Map or Struct, accessor re-tags
		break;
	default:
		V.Type = EUeMcpValueType::Null;
		break;
	}
	return V;
}
