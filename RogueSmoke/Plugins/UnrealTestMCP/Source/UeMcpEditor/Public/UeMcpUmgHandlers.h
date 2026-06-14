// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave F (agent-5) — UMG widget interaction handlers (`ui.*`).
//
// Walk the active `UWidgetTree` of any `UUserWidget` currently added to
// the viewport in PIE. Locate by widget name, read text from
// `UTextBlock` / `UEditableText`, synthesize a click on a `UButton` via
// `OnClicked.Broadcast()`, dump the entire widget hierarchy as JSON.
//
// All handlers are synchronous (one game-thread hop, no waiting) — the
// widget tree is walked inline, the OnClicked broadcast returns
// immediately. No pending-handler variant needed.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

namespace UeMcp
{
	/**
	 * Register the `ui.*` tool handlers on the given dispatcher. Call once
	 * from `UUeMcpEditorSubsystem::Initialize`.
	 *
	 * Tools registered:
	 *   - `ui.find_widget` — locate a widget by name (and optionally class)
	 *     across all UserWidgets currently in the viewport.
	 *   - `ui.get_text`    — read text content of a `UTextBlock` or
	 *     `UEditableText` widget.
	 *   - `ui.click`       — synthesize a click on a `UButton` via its
	 *     `OnClicked.Broadcast()` multicast delegate.
	 *   - `ui.dump_tree`   — dump the full widget hierarchy of a UserWidget
	 *     (or all UserWidgets in the viewport) as nested JSON.
	 */
	UEMCPEDITOR_API void RegisterUmgHandlers(FUeMcpDispatcher& Dispatcher);
}
