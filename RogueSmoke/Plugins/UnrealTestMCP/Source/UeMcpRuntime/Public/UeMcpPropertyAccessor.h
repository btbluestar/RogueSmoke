// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// Accessor shape inspired by Incurian/AgentBridge (MIT).

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "UeMcpPropertyPath.h"
#include "UeMcpPropertyValue.h"

/**
 * Structured error codes the accessor returns. Handler code maps these
 * 1:1 to the upper-snake codes in `docs/handler-conventions.md §2` (e.g.
 * `PropNotFound -> "PROP_NOT_FOUND"`).
 *
 * `Success` is the only non-error value. All others carry human-readable
 * `Message` and optional structured `Detail` (e.g. `actual_properties:
 * [...]` on `PropNotFound`, `actual_size` on `IndexOOB`).
 */
enum class EUeMcpAccessorError : uint8
{
	Success,
	InvalidPath,      // parse failure; wraps FUeMcpPathParseError
	NullObject,       // root (or a walked intermediate) was null
	PropNotFound,     // no FProperty by that name
	IndexOOB,         // array/set index out of bounds
	KeyNotFound,      // map key absent
	TypeMismatch,     // expected a container at this segment but got scalar, or vice versa
	DepthExceeded,    // max-depth truncation on read (still Success-ish with bTruncated)
	NotWritable,      // CPF_EditConst; no write
	ReadOnlySegment,  // attempt to Set through a @Synthesized segment
	InternalError,    // fallthrough / unexpected engine state
};

namespace UeMcp
{
	/**
	 * THE single source of truth for `EUeMcpAccessorError` -> wire error
	 * code. Every handler that surfaces an accessor failure MUST route
	 * through this function — no handler may keep a local switch (this
	 * mapper was hand-copied into 8 sites and drifted; see issue #62).
	 *
	 * Canonical mapping (also documented in `docs/handler-conventions.md`):
	 *   - InvalidPath      -> SCHEMA_ERROR
	 *   - NullObject       -> NOT_FOUND
	 *   - PropNotFound     -> PROP_NOT_FOUND
	 *   - IndexOOB         -> INDEX_OOB
	 *   - KeyNotFound      -> KEY_NOT_FOUND
	 *   - TypeMismatch     -> TYPE_MISMATCH
	 *   - NotWritable      -> TYPE_MISMATCH (message names CPF_EditConst)
	 *   - ReadOnlySegment  -> SCHEMA_ERROR  (@-segment is read-only)
	 *   - DepthExceeded    -> TYPE_MISMATCH
	 *   - InternalError    -> PLUGIN_INTERNAL_ERROR
	 *   - Success          -> PLUGIN_INTERNAL_ERROR (mapping Success is a
	 *                         programming error; mapped defensively, never
	 *                         crashes)
	 *
	 * Implemented as an exhaustive `switch` with NO `default:` label so a
	 * future enumerator fails `-Wswitch` and forces a conscious mapping
	 * decision here (the regression guard issue #62 asks for).
	 */
	UEMCPRUNTIME_API FString AccessorErrorToCode(EUeMcpAccessorError Err);
}

/**
 * Rich error detail for the handler layer.
 */
struct UEMCPRUNTIME_API FUeMcpAccessorErrorInfo
{
	EUeMcpAccessorError Code = EUeMcpAccessorError::Success;
	FString Message;
	TSharedPtr<FJsonObject> Detail; // optional; handler attaches to response

	bool IsOk() const { return Code == EUeMcpAccessorError::Success; }
};

/**
 * Options for read traversals.
 */
struct UEMCPRUNTIME_API FUeMcpReadOptions
{
	/** Max nesting depth when serializing struct/array/map values. 10 matches Incurian default. */
	int32 MaxDepth = 10;
};

/**
 * Options for `ListPropertyPaths`. The walker flattens the tree into a
 * list of strings, one per addressable leaf (scalars + special-struct-strings).
 */
struct UEMCPRUNTIME_API FUeMcpListPathsOptions
{
	/** Max nesting depth for the walk. */
	int32 MaxDepth = 3;

	/** Include synthesized `@Class` / `@Components` entries in the output. */
	bool bIncludeSynthesized = true;
};

/**
 * Game-thread-only accessor. Every public entry point asserts
 * `check(IsInGameThread())`.
 *
 * Usage:
 *
 *   FUeMcpAccessorErrorInfo Err;
 *   FUeMcpPropertyValue Value;
 *   if (FUeMcpPropertyAccessor::GetValue(Obj, TEXT("Health"), Value, Err)) { ... }
 *
 * The core never calls into UnrealEd — this lets us retarget `UeMcpRuntime`
 * at a cooked harness later. Object resolution (string -> UObject*) lives
 * in the editor module; the accessor takes a raw `UObject*` root.
 */
class UEMCPRUNTIME_API FUeMcpPropertyAccessor
{
public:
	/**
	 * Read the value at `PathString` relative to `Root`.
	 * On success, `OutValue.Json` is populated. On error, returns false
	 * and fills `OutError`.
	 */
	static bool GetValue(
		UObject* Root,
		const FString& PathString,
		FUeMcpPropertyValue& OutValue,
		FUeMcpAccessorErrorInfo& OutError,
		const FUeMcpReadOptions& Options = {});

	/**
	 * Write the JSON-shaped `InValue` into the property at `PathString`
	 * relative to `Root`. JSON is coerced to the target property type;
	 * type mismatches surface as `TypeMismatch`. Gated on `CPF_EditConst`
	 * (never on `CPF_BlueprintReadOnly` — we're editor code, not BP).
	 */
	static bool SetValue(
		UObject* Root,
		const FString& PathString,
		const TSharedPtr<FJsonValue>& InValue,
		FUeMcpAccessorErrorInfo& OutError);

	/**
	 * Enumerate addressable paths under `Root`, one string per leaf.
	 * Output paths are relative (no `<RootLabel>.` prefix).
	 */
	static bool ListPropertyPaths(
		UObject* Root,
		TArray<FString>& OutPaths,
		FUeMcpAccessorErrorInfo& OutError,
		const FUeMcpListPathsOptions& Options = {});

	/**
	 * True iff `Property` is writable by our policy: writable if NOT
	 * `CPF_EditConst`. `CPF_BlueprintReadOnly` is NOT a write barrier
	 * for us (see Incurian note §2.4 in the study).
	 */
	static bool IsWritable(const FProperty* Property);

private:
	FUeMcpPropertyAccessor() = delete;
};
