// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// Object resolver pattern inspired by Incurian/AgentBridge (MIT) —
// `THIRD_PARTY/notes/Incurian-AgentBridge.md §2.8`. Clean-room
// reimplementation: one string -> either a live actor in the resolved
// world OR a loaded asset/class, with a five-strategy fallback chain.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"

class UObject;
class UWorld;

namespace UeMcp
{
	/**
	 * Outcome of `ResolveObject`. On success, `Object` is non-null and the
	 * `bFromAsset` / `bLoaded` flags describe which strategy matched so the
	 * caller can emit accurate warnings (e.g. "I had to load this asset,
	 * subsequent calls will be cheaper").
	 *
	 * On failure, `Object` is null and `ErrorInfo` carries an inline-error
	 * JSON object of the shape `{error, message, detail}` already populated
	 * with the tried strategies list — handlers can splice it straight into
	 * their response root.
	 */
	struct UEMCPEDITOR_API FUeMcpResolvedObject
	{
		/** Resolved UObject pointer; `nullptr` on failure. */
		UObject* Object = nullptr;

		/** True when matched by actor iteration (strategy 1). False otherwise. */
		bool bFromWorld = false;

		/** True when matched via soft-object-path resolution (strategies 2/4). */
		bool bFromAsset = false;

		/** True when the resolver had to call `TryLoad()` (strategies 3/4). */
		bool bLoaded = false;

		/** Non-null on failure; root-level JSON ready to return from a handler. */
		TSharedPtr<FJsonObject> ErrorInfo;

		bool IsOk() const { return Object != nullptr; }
	};

	/**
	 * Resolve a user-supplied string `ObjectId` to a concrete `UObject*`.
	 *
	 * Strategy chain, tried in order:
	 *   1. Iterate `WorldHint`'s actors — case-insensitive `GetActorLabel()`
	 *      match, then `GetName()`. This is how tests address live actors
	 *      by their editor label ("BP_Door_3").
	 *   2. If the string looks like an object path (starts with `/` or
	 *      contains `.`), try `FSoftObjectPath::ResolveObject()` — returns
	 *      non-null only if the asset is already loaded.
	 *   3. Same `FSoftObjectPath`, but `TryLoad()` — loads on demand. This
	 *      is how we reach uninstantiated data assets, class-default
	 *      objects, etc.
	 *   4. Append `_C` to the ObjectId (e.g. `/Game/Actors/BP_Door` becomes
	 *      `/Game/Actors/BP_Door.BP_Door_C`) and retry 2/3 — handles the
	 *      common "pass a BP asset path, want the generated class" case.
	 *   5. Give up; return `NOT_FOUND` with `detail.searched_strategies`
	 *      listing the names of every strategy we tried.
	 *
	 * The resolver MUST be called on the game thread (it iterates actors,
	 * triggers asset loads, and touches GEditor). It does not allocate a
	 * world — the caller passes `WorldHint` from `ResolveWorldFromArgs`.
	 *
	 * Note on blueprints: when the resolution hits a `UBlueprint` asset,
	 * the handler that needs a class should explicitly unwrap it to
	 * `BP->GeneratedClass`. `ResolveObject` returns the raw asset — the
	 * handler's context (e.g. "I want a class" vs "I want the outer BP
	 * asset") determines the final type. The `_C` fallback at strategy 4
	 * handles the common case of "caller meant the class" ergonomically.
	 */
	UEMCPEDITOR_API FUeMcpResolvedObject ResolveObject(
		const FString& ObjectId, UWorld* WorldHint);
}
