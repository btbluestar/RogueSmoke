// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Animation montage handlers — drive `UAnimMontage` playback on a
// target `USkeletalMeshComponent` via its `UAnimInstance`. Three sync
// handlers register here:
//
//   - `anim.play_montage`  — load a montage asset, call `Montage_Play`
//     on the resolved component's anim instance, optionally jumping to
//     `start_section` and using `play_rate`.
//   - `anim.stop_montage`  — call `Montage_Stop` on the resolved
//     component (optional `blend_out_time`, optional named montage).
//   - `anim.get_active_section` — snapshot the currently-playing
//     section name + montage position + position-within-section so
//     tests can assert against an active animation state.
//
// Issue #19 — montage authoring (no python_exec):
//   - `anim.create_montage` — build + save a `UAnimMontage` asset that
//     wraps a single source `UAnimSequence` (one slot, one "Default"
//     section). This is the authoring counterpart to play/stop — it
//     gives `anim.play_montage` something to load.
//
// All handlers run synchronously on the game thread (the engine APIs
// they wrap return immediately — playback then continues across ticks
// inside the anim graph).

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the `anim.play_montage`, `anim.stop_montage`,
	 * `anim.get_active_section`, and `anim.create_montage` handlers on
	 * the given dispatcher. Call once from
	 * `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterAnimHandlers(FUeMcpDispatcher& Dispatcher);
}
