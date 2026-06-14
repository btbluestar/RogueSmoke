// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// Transport union shape inspired by Incurian/AgentBridge (MIT).

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Typed value carrier on the path between the reflection accessor and the
 * JSON transport. Kept intentionally small — the wire always flattens to
 * `FJsonValue` / `FJsonObject`; this struct exists so the accessor can
 * report precise semantic types (enum vs int, special-struct-string vs
 * arbitrary-struct-object) to the handler without losing that precision
 * through a raw JSON round-trip.
 *
 * Variant philosophy: we don't use `TVariant` because UE's JSON round-trip
 * already paves over the fine distinctions — this struct is specifically
 * the hand-off between "I walked the property tree" and "go emit JSON", and
 * carrying an enum tag plus a pre-built `TSharedPtr<FJsonValue>` is the
 * minimum state needed.
 */
enum class EUeMcpValueType : uint8
{
	Null,
	Bool,
	Int64,
	Double,
	String,         // scalar string OR a "special-struct" ToString form (FVector etc.)
	Name,           // FName / FText source
	Enum,           // held as display-name string; numeric available in JSON field `value_numeric`
	ObjectRef,      // soft-object-path string
	Array,
	Map,
	Struct,         // arbitrary non-special struct, fields serialized
};

/**
 * Tagged value plus pre-rendered JSON representation.
 *
 * `Json` is always populated (it's what the handler emits). `Type` is for
 * the handler's `detail`/diagnostic use — e.g. "the field was an enum, here
 * is the numeric form under `.value_numeric`, consider it when comparing".
 */
struct UEMCPRUNTIME_API FUeMcpPropertyValue
{
	EUeMcpValueType Type = EUeMcpValueType::Null;

	/** Canonical JSON form of the value. Never null. */
	TSharedPtr<FJsonValue> Json;

	/** For `Enum`: the numeric form alongside the display name. */
	int64 EnumNumeric = 0;

	/** For `Array` / `Map` / `Struct`: true if a depth cap truncated output. */
	bool bTruncated = false;

	/** Convenience constructor for a simple JSON-null value. */
	static FUeMcpPropertyValue MakeNull();

	/** Wrap an existing JSON value as a typed carrier. `Json` set; callers fix Type. */
	static FUeMcpPropertyValue FromJson(TSharedPtr<FJsonValue> InJson, EUeMcpValueType InType);
};

/**
 * JSON -> value conversion for set-property writes. Accepts the JSON form
 * emitted by any MCP tool (strings, bools, numbers, arrays, objects) and
 * returns a carrier the accessor can coerce into the target `FProperty`.
 *
 * Best-effort: specific property types do further coercion in the accessor
 * (e.g. a JSON number going into an `FEnumProperty` is reinterpreted).
 */
UEMCPRUNTIME_API FUeMcpPropertyValue UeMcpValueFromJson(
	const TSharedPtr<FJsonValue>& InJson);
