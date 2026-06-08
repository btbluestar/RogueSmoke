// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// Path DSL and resolver inspired by Incurian/AgentBridge (MIT) —
// `THIRD_PARTY/licenses/Incurian-AgentBridge-LICENSE`. Clean-room
// reimplementation; no code copied, only patterns. See
// `THIRD_PARTY/notes/Incurian-AgentBridge.md` for the study.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"

/**
 * Parsed segment of a property path.
 *
 * Four kinds:
 *   - `Property`        : named member of the current container (`Health`,
 *                         `RelativeLocation`). Identifier stored in `Name`.
 *   - `ArrayIndex`      : integer index into a `FArrayProperty` or
 *                         `FSetProperty`. Stored in `Index`.
 *   - `MapKey`          : string key into a `FMapProperty`. Stored in `Key`.
 *                         M5 does not support non-string map keys; a typed
 *                         key carrier is deferred to v1.
 *   - `Synthesized`     : leading `@`-prefixed token — not a reflected
 *                         property. Currently `@Class`, `@Components`,
 *                         `@CDO`. `Name` carries the token WITHOUT the `@`.
 *                         Resolver handles these specially.
 *
 * The optional `EnumDisplay` flag (from a trailing `:enum_name` suffix on a
 * property segment) asks the resolver to return enum values as
 * display-string rather than numeric. Ignored on non-enum terminals.
 */
enum class EUeMcpPathSegmentKind : uint8
{
	Property,
	ArrayIndex,
	MapKey,
	Synthesized,
};

struct UEMCPRUNTIME_API FUeMcpPathSegment
{
	EUeMcpPathSegmentKind Kind = EUeMcpPathSegmentKind::Property;

	/** `Property` and `Synthesized`: the identifier. Empty for Index/Key. */
	FString Name;

	/** `ArrayIndex`: the index. Valid iff `Kind == ArrayIndex`. */
	int32 Index = 0;

	/** `MapKey`: the string form of the key. Valid iff `Kind == MapKey`. */
	FString Key;

	/** Trailing `:enum_name` suffix request on a `Property` leaf. */
	bool bEnumNameDisplay = false;
};

/**
 * Parse error detail returned when `ParsePath` fails. Mirrors the
 * structured-error shape used elsewhere in the plugin: an upper-snake code
 * plus a human-readable message plus optional `Detail` JSON for the
 * transport layer to surface.
 */
struct UEMCPRUNTIME_API FUeMcpPathParseError
{
	FString Code;       // e.g. "INVALID_PAYLOAD"
	FString Message;    // one-line description
	int32 Position = 0; // 0-based character index where parse failed
};

/**
 * Static path utilities. No state; all methods are thread-safe at parse
 * time but resolution (done in `FUeMcpPropertyAccessor`) is game-thread-only.
 *
 * Path syntax (summary; full grammar in `docs/handler-conventions.md` §11):
 *   Identifier     := [A-Za-z_@][A-Za-z0-9_]*
 *   Segment        := Identifier (":" Identifier)? ("[" Index "]" | "[\"" Key "\"]")?
 *   Path           := Segment ("." Segment)*
 *
 * Leading `@` reserves the identifier as synthesized (`@Class`, `@Components`,
 * `@CDO`); `@CDO` must be the first segment if used, subsequent segments
 * walk the CDO.
 *
 * The parser is permissive about whitespace between segments; case is
 * preserved (resolution is case-insensitive further down).
 */
class UEMCPRUNTIME_API FUeMcpPropertyPath
{
public:
	/**
	 * Parse `PathString` into a segment array.
	 *
	 * Returns `true` on success (`OutSegments` filled). On parse failure
	 * returns `false`, leaves `OutSegments` untouched, and fills
	 * `OutError` if non-null.
	 */
	static bool ParsePath(
		const FString& PathString,
		TArray<FUeMcpPathSegment>& OutSegments,
		FUeMcpPathParseError* OutError = nullptr);

	/**
	 * Pre-flight validation. Same logic as `ParsePath` but discards the
	 * output — useful for the tools' `SCHEMA_ERROR` path before the
	 * game-thread hop.
	 */
	static bool ValidatePath(
		const FString& PathString,
		FUeMcpPathParseError* OutError = nullptr);

	/** True iff `Name` is a recognised synthesized token without `@`. */
	static bool IsKnownSynthesizedName(const FString& Name);

private:
	FUeMcpPropertyPath() = delete;
};
