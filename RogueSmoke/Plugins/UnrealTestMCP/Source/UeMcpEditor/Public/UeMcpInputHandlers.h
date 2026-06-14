// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave-fanout agent-1 — synthetic input.
//
// Registers three handlers that drive `APlayerController` input from a
// test script, so a Claude-authored gameplay test can simulate the human
// input that the game-under-test reads:
//
//   * `input.simulate_key` — press, release, or both for one named key
//     (`"W"`, `"SpaceBar"`, `"LeftMouseButton"`, …) on the active local
//     player controller in the resolved world. Sync handler: a single
//     `UPlayerInput::InputKey(FInputKeyEventArgs)` per phase.
//   * `input.simulate_axis` — feed an analog axis value with an explicit
//     `delta_time` and optional `num_samples`. Use case: gamepad sticks,
//     mouse-X/Y, scroll wheel.
//   * `input.tap` — sugar for `simulate_key` with `phase="both"`,
//     emitting Pressed and Released back-to-back inside the same
//     handler invocation. Suitable when the receiver only cares that a
//     full press cycle landed (UI buttons, single-tap action mappings).
//
// All three are sync handlers — input dispatch is a one-shot call into
// `UPlayerInput`; nothing waits across ticks. The receiving game logic
// reacts on the next Tick / next axis-update poll, which the caller can
// observe via `pie.advance_frames` + `state.wait_until` if needed.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the `input.simulate_key`, `input.simulate_axis`, and
	 * `input.tap` handlers on the given dispatcher. Call once from
	 * `UUeMcpEditorSubsystem::Initialize`.
	 */
	UEMCPEDITOR_API void RegisterInputHandlers(FUeMcpDispatcher& Dispatcher);
}
