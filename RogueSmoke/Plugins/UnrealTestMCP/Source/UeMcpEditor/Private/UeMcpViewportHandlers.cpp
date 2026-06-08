// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpViewportHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Editor/EditorEngine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HighResScreenshot.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#include "LevelEditor.h"
#include "LevelEditorViewport.h"
#include "SLevelViewport.h"

#include "Containers/Ticker.h"
#include "Modules/ModuleManager.h"

#include "UeMcpDispatcher.h"
#include "UeMcpEditorSubsystem.h" // LogUeMcpEditor
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpWorldResolver.h" // UeMcp::MakeInlineError

namespace UeMcpViewportHandlersPrivate
{
	/**
	 * Wall-cap on a single screenshot pending request.
	 *
	 * Longer than the canonical 2s cap used by `pie.advance_frames` because
	 * a screenshot has to drive an actual render tick to completion: when
	 * the editor window is unfocused the engine throttles to 3 Hz idle, and
	 * the high-res capture path internally renders multiple "warm-up"
	 * frames (`r.HighResScreenshotDelay`, default 4) before the bitmap is
	 * processed. 5s gives an unfocused-editor + 4-frame-delay capture
	 * comfortable margin while still bounding well under the Python-side
	 * call timeout (~30s default in the wrapper).
	 *
	 * On wall-cap exit we return `{saved_path, timed_out: true}` and the
	 * Python wrapper's file poll will still surface a TIMEOUT if the engine
	 * never wrote the PNG. (In practice the engine does land the PNG even
	 * when our delegate gating fires the wall cap; the timeout signal is
	 * defensive.)
	 */
	static constexpr double ScreenshotWallCapSeconds = 5.0;

	/**
	 * Engine ticks between forced viewport redraws while waiting for the
	 * screenshot delegate to fire. The editor's auto-redraw is suppressed
	 * when the window is unfocused, so we have to drive draws ourselves to
	 * push the queued screenshot through. One redraw per service call is
	 * usually enough; this throttle keeps a slow disk-write from triggering
	 * a redraw storm.
	 */
	static constexpr double RedrawIntervalSeconds = 0.05;

	/** Resolve the final absolute path for the screenshot.
	 *
	 *  Empty / missing `path` → `<ProjectSavedDir>/Screenshots/mcp-<ts>.<ext>`.
	 *  Relative path → anchored to `<ProjectDir>`.
	 *  Absolute path → used verbatim (after sanitisation).
	 *
	 *  The chosen extension is always `.png` in v0 — the engine's built-in
	 *  screenshot save path writes PNG and we don't transcode. Callers
	 *  requesting `format="jpg"` get a PNG + a warning; the wire contract
	 *  still holds (saved_path + format mismatch surfaced via warning).
	 */
	static FString ResolveOutputPath(const FString& InPath, const FString& Ext)
	{
		FString Candidate = InPath;
		if (Candidate.IsEmpty())
		{
			// Unique default name — timestamp to millisecond resolution so
			// rapid-fire test runs don't collide.
			const FDateTime Now = FDateTime::UtcNow();
			const FString Stamp = Now.ToString(TEXT("%Y%m%d-%H%M%S"));
			const int32 Millis = Now.GetMillisecond();
			Candidate = FPaths::ProjectSavedDir() / TEXT("Screenshots") /
				FString::Printf(TEXT("mcp-%s-%03d.%s"),
					*Stamp, Millis, *Ext);
		}
		else if (FPaths::IsRelative(Candidate))
		{
			Candidate = FPaths::ProjectDir() / Candidate;
		}

		// Guarantee the requested extension — callers often omit it.
		const FString CurExt = FPaths::GetExtension(Candidate, /*bIncludeDot*/ false);
		if (CurExt.IsEmpty())
		{
			Candidate += TEXT(".") + Ext;
		}

		FPaths::NormalizeFilename(Candidate);
		// Absolutify. FPaths::ProjectSavedDir() returns an engine-relative
		// path ("../../../../Saved/Screenshots/..."), and every client that
		// has to poll the file on disk needs a path resolvable from its own
		// CWD. Without this the Python wrapper's file-wait times out on a
		// path that DOES exist but under an unexpected base.
		Candidate = FPaths::ConvertRelativePathToFull(Candidate);
		return Candidate;
	}

	/** Resolve the current editor-viewport size in pixels. Returns (0,0)
	 *  if no active level viewport is available (e.g. headless commandlet
	 *  host — caller should then fall back to a sensible default). */
	static FIntPoint GetEditorViewportSize()
	{
		FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
		if (Client == nullptr || Client->Viewport == nullptr)
		{
			return FIntPoint::ZeroValue;
		}
		return Client->Viewport->GetSizeXY();
	}

	/**
	 * Per-request state for `viewport.screenshot`.
	 *
	 * Holds the bound `OnScreenshotRequestProcessed` delegate handle plus
	 * the "done" flag the delegate flips. The destructor unbinds the
	 * delegate handle — load-bearing for two reasons:
	 *
	 *   1. If the request is abandoned (caller timeout, executor shutdown)
	 *      the natural completion path never runs — without the destructor
	 *      we'd leave a dangling lambda capturing our state on a
	 *      module-static delegate and the next screenshot's broadcast
	 *      would crash on a freed `bDone` flag.
	 *   2. We deliberately bind to `OnScreenshotRequestProcessed` rather
	 *      than `OnScreenshotCaptured` because:
	 *        - Both editor (`FEditorViewportClient::ProcessScreenShots`)
	 *          and game (`UGameViewportClient::ProcessScreenshots`)
	 *          broadcast `OnScreenshotRequestProcessed` AFTER the
	 *          `RequestSaveScreenshot` / `ProcessScreenshotData` call,
	 *          guaranteeing the file is on disk (or queued + .Get()'d in
	 *          the editor path) when our handler fires.
	 *        - `OnScreenshotCaptured` is only fired conditionally
	 *          (`CVarScreenshotDelegate.GetValueOnGameThread()` in the
	 *          game viewport path), so it's not a reliable signal.
	 *
	 * The fix this state struct enables: previously each `viewport.screenshot`
	 * call registered the screenshot via the high-res config + a one-shot
	 * FTSTicker, then returned immediately. The next call would set
	 * `GIsHighResScreenshot=true` again but the previous request's queued
	 * state could leave the engine in a wedged "screenshot pending but
	 * never drained" state when no render tick happened (unfocused editor).
	 * Now the handler stays alive across ticks, drives the redraw itself,
	 * and observes completion via the engine's own broadcast — so each
	 * call is self-contained and the next can fire cleanly.
	 */
	struct FScreenshotState
	{
		FString AbsolutePath;
		FString WorldKind; // "editor" | "pie"
		int32 ResX = 0;
		int32 ResY = 0;
		bool bIncludeUI = false;

		// Weak ref to the level viewport we kicked the request against —
		// used to drive synchronous redraws between ticks. The editor
		// could close the viewport mid-wait (highly unlikely in practice
		// but cheap to defend against).
		TWeakPtr<SLevelViewport> LevelViewport;

		// Multicast-delegate handle for OnScreenshotRequestProcessed —
		// stored so we can unbind on completion / abandonment / GC.
		FDelegateHandle ProcessedHandle;

		// Shared-with-delegate flag. The lambda we bind into
		// OnScreenshotRequestProcessed sets this to true on broadcast;
		// the step closure observes it. TSharedRef so the delegate
		// lambda's captured copy outlives any path that might drop
		// FScreenshotState before the engine fires the broadcast.
		// (In practice the destructor unbinds, but a multicast broadcast
		// captures the lambda copies into a local array before invoking
		// — and would crash dereferencing freed memory if we used a
		// raw pointer / by-ref capture.)
		TSharedRef<TAtomic<bool>, ESPMode::ThreadSafe> Done;

		// Wall-clock + redraw bookkeeping.
		double StartWallSeconds = 0.0;
		double LastRedrawSeconds = -1.0;

		// Whether we've kicked the engine yet. The factory parses args
		// and creates state; the FIRST tick is what actually fires the
		// screenshot (so the engine's render tick has a clean post-
		// invocation gap to consume the request).
		bool bFired = false;

		FScreenshotState()
			: Done(MakeShared<TAtomic<bool>, ESPMode::ThreadSafe>(false))
		{
		}

		~FScreenshotState()
		{
			// Unbind on any exit path — natural completion, abandonment,
			// or executor shutdown. Safe to call from any thread (the
			// multicast delegate's Remove is internally locked for the
			// CoreUObject delegate types). If we never bound (early-fail
			// in the factory), `ProcessedHandle.IsValid()` is false and
			// Remove is a no-op.
			if (ProcessedHandle.IsValid())
			{
				FScreenshotRequest::OnScreenshotRequestProcessed().Remove(ProcessedHandle);
			}
		}
	};

	/** Fire the screenshot against the active level-editor viewport.
	 *
	 *  Mirrors `UAutomationBlueprintFunctionLibrary::TakeHighResScreenshot`
	 *  (FunctionalTesting/Private/AutomationBlueprintFunctionLibrary.cpp:1223):
	 *  resolves the active SLevelViewport through the LevelEditor module
	 *  (NOT `GCurrentLevelEditingViewportClient`, which stales out when
	 *  focus is elsewhere), configures the high-res screenshot config,
	 *  and kicks `TakeHighResScreenShot` directly.
	 *
	 *  Stashes the resolved level viewport into the state so the step
	 *  closure can drive synchronous redraws on it — needed when the
	 *  editor window is unfocused and Slate would otherwise not tick the
	 *  viewport on its own (the queued screenshot would never drain).
	 */
	static bool FireEditorViewportCapture(FScreenshotState& S)
	{
		if (!FModuleManager::Get().IsModuleLoaded(TEXT("LevelEditor")))
		{
			return false;
		}
		FLevelEditorModule& LevelEditor =
			FModuleManager::GetModuleChecked<FLevelEditorModule>(TEXT("LevelEditor"));
		TSharedPtr<SLevelViewport> LevelViewport =
			LevelEditor.GetFirstActiveLevelViewport();
		if (!LevelViewport.IsValid())
		{
			return false;
		}
		FViewport* ActiveVp = LevelViewport->GetActiveViewport();
		if (ActiveVp == nullptr)
		{
			return false;
		}

		// `bIncludeUI` is a no-op for the editor viewport capture — the
		// engine's TakeHighResScreenShot path always produces a
		// gameview-style capture. We accept the arg for shape stability.
		(void)S.bIncludeUI;

		FHighResScreenshotConfig& Config = GetHighResScreenshotConfig();
		Config.SetResolution(static_cast<uint32>(S.ResX),
			static_cast<uint32>(S.ResY), 1.0f);
		Config.SetFilename(S.AbsolutePath);
		Config.bDateTimeBasedNaming = false;

		const bool bRequested = ActiveVp->TakeHighResScreenShot();
		S.LevelViewport = LevelViewport;

		UE_LOG(LogUeMcpEditor, Log,
			TEXT("viewport.screenshot: editor TakeHighResScreenShot returned %s for %s at %dx%d"),
			bRequested ? TEXT("true") : TEXT("false"),
			*S.AbsolutePath, S.ResX, S.ResY);
		return true;
	}

	/** Fire the screenshot against the active PIE game viewport via
	 *  `FScreenshotRequest::RequestScreenshot`. Engine writes PNG on the
	 *  next game-viewport draw. Returns false if no game viewport is up. */
	static bool FirePieViewportCapture(const FScreenshotState& S)
	{
		if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
		{
			return false;
		}

		UGameViewportClient* GVC = GEditor->GameViewport;
		if (GVC == nullptr || GVC->Viewport == nullptr)
		{
			return false;
		}

		FScreenshotRequest::RequestScreenshot(
			S.AbsolutePath,
			S.bIncludeUI,
			/*bAddFilenameSuffix*/ false,
			/*bHdrScreenshot*/ false);

		UE_LOG(LogUeMcpEditor, Log,
			TEXT("viewport.screenshot: PIE RequestScreenshot for %s (UI=%d)"),
			*S.AbsolutePath, S.bIncludeUI ? 1 : 0);
		return true;
	}

	/** Build the success payload for a completed (or wall-capped) capture. */
	static TSharedRef<FJsonObject> BuildScreenshotResult(
		const FScreenshotState& S, bool bDelegateFired, bool bWallCapped,
		bool bWarnJpg)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("saved_path"), S.AbsolutePath);
		Data->SetNumberField(TEXT("width"), S.ResX);
		Data->SetNumberField(TEXT("height"), S.ResY);
		Data->SetStringField(TEXT("format"), TEXT("png"));
		Data->SetStringField(TEXT("world"), S.WorldKind);
		// `pending` retained for wire-shape stability with the previous
		// fire-and-forget contract — the Python wrapper still polls the
		// file on disk for size + dims regardless. False when our delegate
		// fired (engine has produced the bitmap and queued the write);
		// true when we exited via wall-cap so the Python poll knows to be
		// patient.
		Data->SetBoolField(TEXT("pending"), !bDelegateFired);
		if (bWallCapped)
		{
			Data->SetBoolField(TEXT("timed_out"), true);
		}
		const double ElapsedMs =
			(FPlatformTime::Seconds() - S.StartWallSeconds) * 1000.0;
		Data->SetNumberField(TEXT("elapsed_ms"),
			FMath::RoundToDouble(ElapsedMs));
		if (bWarnJpg)
		{
			TArray<TSharedPtr<FJsonValue>> Warnings;
			Warnings.Add(MakeShared<FJsonValueString>(
				TEXT("format='jpg' requested but v0 writes PNG; saved_path is PNG.")));
			Data->SetArrayField(TEXT("warnings"), Warnings);
		}
		return Data;
	}

	/** Build a one-shot step closure that emits the given payload and signals Failed. */
	static FUeMcpPendingStep MakeImmediateFailStep(TSharedRef<FJsonObject> Payload)
	{
		return [Payload](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			Out = Payload;
			return EUeMcpStep::Failed;
		};
	}

	/**
	 * `viewport.screenshot` factory.
	 *
	 * Pending-handler variant. Each request:
	 *   1. Parses + validates args at factory time.
	 *   2. Binds a one-shot lambda to `FScreenshotRequest::OnScreenshotRequestProcessed`
	 *      that flips a shared atomic flag.
	 *   3. Returns a step closure. First service-tick fires the screenshot
	 *      (PIE or editor). Subsequent ticks force a level-viewport redraw
	 *      (so the engine has work to do even when the editor is
	 *      unfocused) and check the flag. When the flag flips OR the wall
	 *      cap fires, we return Done.
	 *   4. Destructor unbinds the delegate on every exit path
	 *      (natural / cancel / abandon / executor shutdown).
	 *
	 * This replaces the prior fire-and-forget implementation that
	 * registered a one-shot ticker. The bug it fixes:
	 *
	 *   The old path set `GIsHighResScreenshot` then exited; the next
	 *   request might run before the engine had a chance to consume the
	 *   first one (no render tick on an unfocused editor), wedging the
	 *   capture queue. Now each request is alive across ticks and only
	 *   completes when the engine's own broadcast confirms the capture
	 *   processed — so the next request finds a clean queue.
	 */
	static FUeMcpPendingStep BuildScreenshotStep(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		UE_LOG(LogUeMcpEditor, Verbose, TEXT("viewport.screenshot dispatch"));

		if (GEditor == nullptr)
		{
			return MakeImmediateFailStep(UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; viewport.screenshot cannot run")));
		}

		// --- Parse args ---
		FString InPath;
		Args->TryGetStringField(TEXT("path"), InPath);

		FString Format = TEXT("png");
		Args->TryGetStringField(TEXT("format"), Format);
		Format = Format.ToLower();
		if (Format != TEXT("png") && Format != TEXT("jpg"))
		{
			return MakeImmediateFailStep(UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("format must be 'png' or 'jpg', got '%s'"),
					*Format)));
		}

		bool bIncludeUI = false;
		Args->TryGetBoolField(TEXT("include_ui"), bIncludeUI);

		FString World = TEXT("editor");
		Args->TryGetStringField(TEXT("world"), World);
		World = World.ToLower();
		if (World != TEXT("editor") && World != TEXT("pie"))
		{
			return MakeImmediateFailStep(UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("world must be 'editor' or 'pie', got '%s'"),
					*World)));
		}

		// Resolution: [w, h] int pair; default = current editor viewport.
		int32 ResX = 0;
		int32 ResY = 0;
		{
			const TArray<TSharedPtr<FJsonValue>>* ResArray = nullptr;
			if (Args->TryGetArrayField(TEXT("resolution"), ResArray) &&
				ResArray != nullptr && ResArray->Num() == 2)
			{
				double WRaw = 0.0;
				double HRaw = 0.0;
				(*ResArray)[0]->TryGetNumber(WRaw);
				(*ResArray)[1]->TryGetNumber(HRaw);
				ResX = FMath::Max(1, static_cast<int32>(WRaw));
				ResY = FMath::Max(1, static_cast<int32>(HRaw));
			}
		}
		if (ResX <= 0 || ResY <= 0)
		{
			const FIntPoint Viewport = GetEditorViewportSize();
			ResX = Viewport.X > 0 ? Viewport.X : 1280;
			ResY = Viewport.Y > 0 ? Viewport.Y : 720;
		}

		TSharedRef<FScreenshotState> S = MakeShared<FScreenshotState>();
		S->WorldKind = World;
		S->ResX = ResX;
		S->ResY = ResY;
		S->bIncludeUI = bIncludeUI;
		// Extension is always .png in v0 — the engine save path writes
		// PNG. JPG callers get a warning later.
		S->AbsolutePath = ResolveOutputPath(InPath, TEXT("png"));
		S->StartWallSeconds = FPlatformTime::Seconds();

		// Ensure parent directory exists. The engine's image-write queue
		// will also try to create it, but doing it here surfaces permission
		// issues early rather than as a silent drop.
		const FString ParentDir = FPaths::GetPath(S->AbsolutePath);
		if (!IFileManager::Get().DirectoryExists(*ParentDir))
		{
			IFileManager::Get().MakeDirectory(*ParentDir, /*Tree*/ true);
		}

		// Delete any previous file at the target path so the Python-side
		// file-existence poll is unambiguous about "this run produced it".
		if (FPaths::FileExists(S->AbsolutePath))
		{
			IFileManager::Get().Delete(*S->AbsolutePath,
				/*bRequireExists*/ false, /*bEvenReadOnly*/ true,
				/*bQuiet*/ true);
		}

		// Bind the completion delegate BEFORE firing the screenshot. The
		// engine broadcasts `OnScreenshotRequestProcessed` synchronously
		// inside `ProcessScreenShots` (game viewport: line 2452;
		// editor viewport: line 6783) — there's no race window between
		// our request and the broadcast in normal operation, but binding
		// first makes the contract bulletproof.
		//
		// Capture the shared TAtomic<bool> by value so the lambda owns
		// its own ref; the multicast delegate stores lambda copies and
		// will outlive our FScreenshotState if the request is abandoned.
		TSharedRef<TAtomic<bool>, ESPMode::ThreadSafe> DoneFlag = S->Done;
		S->ProcessedHandle =
			FScreenshotRequest::OnScreenshotRequestProcessed().AddLambda(
				[DoneFlag]()
				{
					DoneFlag->Store(true);
				});

		const bool bWarnJpg = (Format == TEXT("jpg"));
		return [S, bWarnJpg](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			check(IsInGameThread());

			const double NowSeconds = FPlatformTime::Seconds();
			const double WallElapsed = NowSeconds - S->StartWallSeconds;

			// First service tick: actually kick the screenshot. We defer
			// from the factory so the executor's tick has a clean post-
			// invocation gap before the engine's render tick consumes
			// the request — empirically the in-factory + same-tick fire
			// races the editor's draw cycle and the screenshot silently
			// no-ops on the FIRST capture (matches the prior FTSTicker
			// 0-second deferral pattern).
			if (!S->bFired)
			{
				bool bFired = false;
				if (S->WorldKind == TEXT("pie"))
				{
					bFired = FirePieViewportCapture(*S);
					if (!bFired)
					{
						Out = UeMcp::MakeInlineError(
							TEXT("EDITOR_NOT_READY"),
							TEXT("PIE is not active; cannot capture PIE viewport"));
						return EUeMcpStep::Failed;
					}
				}
				else
				{
					bFired = FireEditorViewportCapture(*S);
					if (!bFired)
					{
						Out = UeMcp::MakeInlineError(
							TEXT("EDITOR_NOT_READY"),
							TEXT("No active level viewport; cannot capture editor viewport"));
						return EUeMcpStep::Failed;
					}
				}
				S->bFired = true;
				// Leave `LastRedrawSeconds` at its default (-1.0) so the
				// next service tick draws immediately rather than waiting
				// out the throttle interval. The first post-firing draw
				// is the one that matters most; subsequent ticks honour
				// the throttle.
				return EUeMcpStep::Continue;
			}

			// Has the engine broadcast completion?
			if (S->Done->Load())
			{
				Out = BuildScreenshotResult(*S, /*bDelegateFired*/ true,
					/*bWallCapped*/ false, bWarnJpg);
				return EUeMcpStep::Done;
			}

			// Wall cap — tell the caller we couldn't observe the
			// completion broadcast in time. The PNG may still land on
			// disk (the Python wrapper's file poll has its own grace
			// window), so we return Done with `timed_out: true` rather
			// than Failed — the wire shape is the same and the caller
			// can decide whether to retry.
			if (WallElapsed >= ScreenshotWallCapSeconds)
			{
				UE_LOG(LogUeMcpEditor, Warning,
					TEXT("viewport.screenshot: wall cap %.2fs exceeded waiting on OnScreenshotRequestProcessed for %s"),
					ScreenshotWallCapSeconds, *S->AbsolutePath);
				Out = BuildScreenshotResult(*S, /*bDelegateFired*/ false,
					/*bWallCapped*/ true, bWarnJpg);
				return EUeMcpStep::Done;
			}

			// Drive a synchronous viewport draw between ticks. This is
			// the load-bearing step for the unfocused-editor case:
			// `RedrawLevelEditingViewports` only schedules an
			// invalidation, and Slate doesn't tick the viewport when the
			// window is unfocused — so the queued high-res request would
			// never drain. `FViewport::Draw()` runs the full draw cycle
			// inline, including `ProcessScreenShots`, which broadcasts
			// `OnScreenshotRequestProcessed` and flips our Done flag in
			// the same call. Empirically a single Draw() per service tick
			// is enough; the throttle keeps a slow disk-write from
			// driving a redraw storm.
			if (S->LastRedrawSeconds < 0.0
				|| (NowSeconds - S->LastRedrawSeconds) >= RedrawIntervalSeconds)
			{
				TSharedPtr<SLevelViewport> Vp = S->LevelViewport.Pin();
				FViewport* ActiveVp = Vp.IsValid() ? Vp->GetActiveViewport() : nullptr;
				if (ActiveVp != nullptr)
				{
					ActiveVp->Draw(/*bShouldPresent*/ true);
				}
				S->LastRedrawSeconds = NowSeconds;

				// Re-check Done immediately — Draw() runs ProcessScreenShots
				// synchronously which fires the broadcast we're waiting on.
				if (S->Done->Load())
				{
					Out = BuildScreenshotResult(*S, /*bDelegateFired*/ true,
						/*bWallCapped*/ false, bWarnJpg);
					return EUeMcpStep::Done;
				}
			}

			return EUeMcpStep::Continue;
		};
	}
}

void UeMcp::RegisterViewportHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpViewportHandlersPrivate;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("viewport.screenshot"));
	// Dispatcher timeout must comfortably exceed the handler's own
	// wall cap (5s) plus a slack for the executor's tick-rate.
	Reg.DefaultTimeoutSeconds = 15.0;
	// Not "mutating" in the game-state sense (writes a file, no world
	// mutation), but we flag it true so the dispatcher picks longer
	// timeouts if it ever gates on the bit.
	Reg.bMutating = true;
	Reg.PendingHandler = FUeMcpToolPendingHandler::CreateStatic(&BuildScreenshotStep);
	Dispatcher.RegisterTool(Reg);
}
