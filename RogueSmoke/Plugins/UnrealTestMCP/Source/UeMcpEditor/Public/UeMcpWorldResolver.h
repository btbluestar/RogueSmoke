// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

class UWorld;

/**
 * World-scoping utilities for MCP handlers.
 *
 * Every handler that touches UObjects must decide which world to read
 * or write: the editor world (`GEditor->GetEditorWorldContext().World()`)
 * or the PIE world (`GEditor->PlayWorld`). Inconsistent handling across
 * handlers is the single biggest footgun in prior-art UE MCPs — hence a
 * single utility every handler is expected to call.
 *
 * Wire contract: the optional `world` arg is a string
 * `"auto" | "pie" | "editor"` (case-insensitive; default `"auto"`).
 *   - `"auto"`  prefers PIE when PIE is active, else editor.
 *   - `"pie"`   requires PIE to be active — errors `NOT_IN_PIE` otherwise.
 *   - `"editor"` always returns the editor world even when PIE is active.
 *
 * Error contract: on failure, `ErrorCode` is one of `NOT_IN_PIE`,
 * `EDITOR_NOT_READY`, `INVALID_PAYLOAD`, matching the taxonomy in
 * `07_V0_PLAN.md §2.7`. Handlers surface these by returning
 * `{error, message}` at the root of their JSON response — the
 * dispatcher hoists them to the top-level wire-format error shape.
 *
 * Module placement: this utility lives in `UeMcpEditor` (not
 * `UeMcpRuntime`) because its implementation reaches into `GEditor`
 * and `UnrealEd`, which the Runtime module deliberately does not link.
 * Handlers that need to scope a UObject touch live in `UeMcpEditor`
 * anyway, so the placement is natural.
 */

namespace UeMcp
{
	/** Caller-requested scope parsed from the wire. */
	enum class EWorldScope : uint8
	{
		Auto,
		PIE,
		Editor,
	};

	/** Result of a world-resolution request. */
	struct UEMCPEDITOR_API FWorldResolution
	{
		/** Resolved world pointer; `nullptr` on error. */
		UWorld* World = nullptr;

		/** Which branch `Auto` resolved to (or the caller's explicit pick). */
		EWorldScope ResolvedScope = EWorldScope::Editor;

		/** True when the resolved world is a PIE world. */
		bool bIsPIE = false;

		/** Non-empty when resolution failed. One of the §2.7 error codes. */
		FString ErrorCode;

		/** Human-readable one-liner; empty on success. */
		FString ErrorMessage;

		bool IsOk() const { return World != nullptr && ErrorCode.IsEmpty(); }
	};

	/**
	 * Parse the optional `"world"` string from a JSON args object.
	 * Missing or unrecognised values fall back to `Auto`.
	 */
	UEMCPEDITOR_API EWorldScope ParseWorldArg(const TSharedPtr<FJsonObject>& Args);

	/**
	 * Resolve a scope to a concrete `UWorld*` under the current editor
	 * state. Must be called on the game thread; callers are expected to
	 * already be inside the dispatcher's game-thread hop.
	 */
	UEMCPEDITOR_API FWorldResolution ResolveWorld(EWorldScope Scope);

	/**
	 * One-shot: parse the arg, resolve the scope. Most handlers want
	 * exactly this.
	 */
	UEMCPEDITOR_API FWorldResolution ResolveWorldFromArgs(
		const TSharedRef<FJsonObject>& Args);

	/**
	 * Build the `{error, message}` root used by handlers to signal
	 * structured errors. Thin wrapper — prevents every handler from
	 * rolling its own.
	 */
	UEMCPEDITOR_API TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message);

	/** Convert an `EWorldScope` to its wire-format string. */
	UEMCPEDITOR_API const TCHAR* WorldScopeToString(EWorldScope Scope);
}
