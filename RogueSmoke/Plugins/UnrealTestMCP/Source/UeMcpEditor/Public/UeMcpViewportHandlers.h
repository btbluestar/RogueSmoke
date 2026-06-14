// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Viewport capture handlers.
 *
 * Registers:
 *   - `viewport.screenshot` — capture the editor or PIE viewport to a PNG
 *     or JPG on disk. Explicit opt-in per CLAUDE.md's pixels-second
 *     principle; never auto-attached.
 *
 * Capture implementation: binds a one-shot
 * `FScreenshotRequest::OnScreenshotCaptured` handler that encodes via
 * `IImageWrapperModule` to PNG/JPG and writes to the configured path.
 * Triggers the engine via `FScreenshotRequest::RequestScreenshot` for
 * PIE / game viewports, and via `FHighResScreenshotConfig` +
 * `FViewport::TakeHighResScreenShot()` for editor-only captures.
 *
 * The handler is fire-and-forget — it returns immediately with
 * `{saved_path, pending: true}` and the Python wrapper polls the file
 * on disk. This keeps the game thread unblocked (screenshots complete
 * on the next render tick, which can't run while we're inside
 * `FTSTicker::Tick`).
 */
namespace UeMcp
{
	UEMCPEDITOR_API void RegisterViewportHandlers(FUeMcpDispatcher& Dispatcher);
}
