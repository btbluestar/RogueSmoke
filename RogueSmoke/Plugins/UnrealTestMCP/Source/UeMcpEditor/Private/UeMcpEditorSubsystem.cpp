// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpEditorSubsystem.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DefaultValueHelper.h"

#include "UeMcpActorHandlers.h"
#include "UeMcpAnimHandlers.h"
#include "UeMcpAIHandlers.h"
#include "UeMcpArenaHandlers.h"
#include "UeMcpAssertionHandlers.h"
#include "UeMcpAssetHandlers.h"
#include "UeMcpBlueprintAuthoringHandler.h"
#include "UeMcpBlueprintGraphHandler.h"
#include "UeMcpBlueprintReferencesHandlers.h"
#include "UeMcpCallMethodHandlers.h"
#include "UeMcpClassReflectionHandlers.h"
#include "UeMcpConsoleHandlers.h"
#include "UeMcpDispatcher.h"
#include "UeMcpEventHandlers.h"
#include "UeMcpFunctionalTestHandlers.h"
#include "UeMcpInputHandlers.h"
#include "UeMcpIntrospectionHandlers.h"
#include "UeMcpLevelHandlers.h"
#include "UeMcpLogHandler.h"
#include "UeMcpPerfHandlers.h"
#include "UeMcpPieHandlers.h"
#include "UeMcpProjectHandlers.h"
#include "UeMcpPythonHandlers.h"
#include "UeMcpReflectionHandlers.h"
#include "UeMcpSaveHandlers.h"
#include "UeMcpServerRunnable.h"
#include "UeMcpSubsystemHandlers.h"
#include "UeMcpTagHandlers.h"
#include "UeMcpTestHandlers.h"
#include "UeMcpTestRunHandlers.h"
#include "UeMcpUmgHandlers.h"
#include "UeMcpViewportHandlers.h"
#include "UeMcpWorldQueryHandlers.h"

DEFINE_LOG_CATEGORY(LogUeMcpEditor);

namespace UeMcpEditorSubsystemPrivate
{
	/** Default listen config. Overridable via env vars per 07_V0_PLAN.md §8. */
	static const FString DefaultHost = TEXT("127.0.0.1");
	static constexpr int32 DefaultPort = 55557;
	static constexpr int32 DefaultMaxMessageBytes = 4 * 1024 * 1024;

	/**
	 * Resolve listener config from environment variables, falling back to the
	 * compiled defaults. `UE_MCP_PORT` is the documented override; others
	 * stay internal for now.
	 */
	static int32 ResolvePort()
	{
		const FString EnvPort = FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_PORT"));
		if (!EnvPort.IsEmpty())
		{
			int32 Parsed = 0;
			if (FDefaultValueHelper::ParseInt(EnvPort, Parsed) && Parsed >= 0 && Parsed <= 65535)
			{
				return Parsed;
			}
			UE_LOG(LogUeMcpEditor, Warning,
				TEXT("UE_MCP_PORT='%s' is not a valid 0..65535 integer; using default %d"),
				*EnvPort, DefaultPort);
		}
		return DefaultPort;
	}
}

void UUeMcpEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// Construct the dispatcher first — the handler registrations and the
	// server runnable both depend on it being alive.
	Dispatcher = MakeUnique<FUeMcpDispatcher>();

	// Gate non-meta dispatches until the editor is genuinely ready (issue
	// #57 mitigation #3). The dispatcher defaults to ready for runtime
	// unit tests; the production editor path explicitly opts into the gate
	// here and lifts it from WaitForEditorReady once GEditor is live.
	Dispatcher->SetEditorReady(false);

	UeMcp::RegisterProjectHandlers(*Dispatcher);
	UeMcp::RegisterPythonExecHandler(*Dispatcher);
	UeMcp::RegisterTestHandlers(*Dispatcher);
	UeMcp::RegisterTestRunHandlers(*Dispatcher);
	UeMcp::RegisterFunctionalTestHandlers(*Dispatcher);
	UeMcp::RegisterPieHandlers(*Dispatcher);
	UeMcp::RegisterLogHandler(*Dispatcher);
	UeMcp::RegisterLevelHandlers(*Dispatcher);
	UeMcp::RegisterActorHandlers(*Dispatcher);
	UeMcp::RegisterReflectionHandlers(*Dispatcher);
	UeMcp::RegisterClassReflectionHandlers(*Dispatcher);
	UeMcp::RegisterCallMethodHandlers(*Dispatcher);
	UeMcp::RegisterIntrospectionHandlers(*Dispatcher);
	UeMcp::RegisterBlueprintGraphHandler(*Dispatcher);
	UeMcp::RegisterBlueprintAuthoringHandler(*Dispatcher);
	UeMcp::RegisterBlueprintReferencesHandlers(*Dispatcher);
	UeMcp::RegisterAssertionHandlers(*Dispatcher);
	UeMcp::RegisterInputHandlers(*Dispatcher);
	UeMcp::RegisterEventHandlers(*Dispatcher);
	UeMcp::RegisterConsoleHandlers(*Dispatcher);
	UeMcp::RegisterSubsystemHandlers(*Dispatcher);
	UeMcp::RegisterUmgHandlers(*Dispatcher);
	UeMcp::RegisterTagHandlers(*Dispatcher);
	UeMcp::RegisterPerfHandlers(*Dispatcher);
	UeMcp::RegisterSaveHandlers(*Dispatcher);
	UeMcp::RegisterViewportHandlers(*Dispatcher);
	UeMcp::RegisterArenaHandlers(*Dispatcher);
	UeMcp::RegisterWorldQueryHandlers(*Dispatcher);
	UeMcp::RegisterAnimHandlers(*Dispatcher);
	UeMcp::RegisterAIHandlers(*Dispatcher);
	UeMcp::RegisterAssetHandlers(*Dispatcher);

	// Attach the log ring buffer before StartServer so startup-phase log
	// lines are captured and later tail-queryable.
	UeMcp::InitializeLogCapture();
	UeMcp::InitializePieTracking();

	StartDialogAutoDecline();
	WaitForEditorReady();
	StartServer();

	UE_LOG(LogUeMcpEditor, Log, TEXT("UeMcpEditorSubsystem initialized."));
}

void UUeMcpEditorSubsystem::Deinitialize()
{
	StopServer();

	// Release the dispatcher after the server so no late request touches a
	// destroyed dispatcher.
	Dispatcher.Reset();

	// Tear down the log ring buffer, PIE-tracking delegates, and the
	// live-test-runner ticker. Order doesn't matter relative to each
	// other; all three are idempotent.
	UeMcp::ShutdownTestRunner();
	UeMcp::ShutdownPieTracking();
	UeMcp::ShutdownLogCapture();

	// Restore the editor's original modal-dialog handler. Done after the
	// server is stopped so no late in-flight request can trigger a dialog
	// while the hook is half-removed.
	StopDialogAutoDecline();

	UE_LOG(LogUeMcpEditor, Log, TEXT("UeMcpEditorSubsystem shutting down."));

	Super::Deinitialize();
}

void UUeMcpEditorSubsystem::StartServer()
{
	using namespace UeMcpEditorSubsystemPrivate;

	if (!Dispatcher.IsValid())
	{
		UE_LOG(LogUeMcpEditor, Error, TEXT("StartServer: dispatcher is null; refusing to start"));
		return;
	}

	if (Server.IsValid())
	{
		UE_LOG(LogUeMcpEditor, Warning, TEXT("StartServer: server already running; ignoring"));
		return;
	}

	const int32 Port = ResolvePort();

	Server = MakeUnique<FUeMcpServerRunnable>(
		*Dispatcher, DefaultHost, Port, DefaultMaxMessageBytes);

	if (!Server->Start())
	{
		UE_LOG(LogUeMcpEditor, Error,
			TEXT("StartServer: server failed to start on %s:%d"),
			*DefaultHost, Port);
		Server.Reset();
		return;
	}

	UE_LOG(LogUeMcpEditor, Log,
		TEXT("StartServer: listening on %s:%d (bound port %d)"),
		*DefaultHost, Port, Server->GetBoundPort());
}

void UUeMcpEditorSubsystem::StopServer()
{
	if (!Server.IsValid())
	{
		return;
	}

	Server->RequestShutdownAndWait();
	Server.Reset();

	UE_LOG(LogUeMcpEditor, Log, TEXT("StopServer: server stopped."));
}

namespace UeMcpDialogAutoDecline
{
	/**
	 * Substring triggers (case-insensitive) for modal dialogs we
	 * auto-decline so an unmanned session is never wedged on a human
	 * click. Matched against both the dialog title and body text.
	 *
	 * Source: 07_V0_PLAN.md §2.3 and THIRD_PARTY/notes/db-lyon-ue-mcp.md
	 * §2f (clean-room — pattern list, not code, was taken from the note).
	 */
	static const TCHAR* const TriggerSubstrings[] = {
		TEXT("save changes"),
		TEXT("overwrite"),
		TEXT("unsaved"),
		TEXT("already exists"),
		TEXT("save your changes"),
		TEXT("save the level"),
		TEXT("save the package"),
		TEXT("save content"),
		TEXT("untitled"),
	};

	/** True if `Haystack` (already lowercased) contains any trigger. */
	static bool MatchesTrigger(const FString& LowerHaystack)
	{
		for (const TCHAR* Needle : TriggerSubstrings)
		{
			if (LowerHaystack.Contains(Needle, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * The non-blocking "decline" answer for a given button set. For
	 * Yes/No-style prompts (the "Save Changes?" family) `No` means
	 * "don't save, proceed" — exactly the unmanned-session intent.
	 * Cancel-bearing Ok/Retry prompts decline via `Cancel`. A bare
	 * `Ok` acknowledgement is answered `Ok` (there is no decline; the
	 * point is just to not block).
	 */
	static EAppReturnType::Type DeclineAnswerFor(EAppMsgType::Type MessageType)
	{
		switch (MessageType)
		{
			case EAppMsgType::YesNo:
			case EAppMsgType::YesNoCancel:
			case EAppMsgType::YesNoYesAllNoAll:
			case EAppMsgType::YesNoYesAllNoAllCancel:
			case EAppMsgType::YesNoYesAll:
				return EAppReturnType::No;
			case EAppMsgType::OkCancel:
			case EAppMsgType::CancelRetryContinue:
				return EAppReturnType::Cancel;
			case EAppMsgType::Ok:
			default:
				return EAppReturnType::Ok;
		}
	}
}

void UUeMcpEditorSubsystem::StartDialogAutoDecline()
{
	using TModalDialogDelegate = TDelegate<
		EAppReturnType::Type(EAppMsgCategory, EAppMsgType::Type, const FText&, const FText&)>;

	if (bDialogAutoDeclineInstalled)
	{
		UE_LOG(LogUeMcpEditor, Warning,
			TEXT("StartDialogAutoDecline: already installed; ignoring"));
		return;
	}

	// `FCoreDelegates::ModalMessageDialog` is a *single-bind* TDelegate.
	// `UEditorEngine::Init` binds its own `OnModalMessageDialog` long
	// before any UEditorSubsystem initializes, so by the time we get here
	// that binding is live. Capture it, then rebind ourselves so we can
	// substring-filter and forward everything we don't recognise to the
	// editor's original handler. Restored verbatim in StopDialogAutoDecline.
	//
	// (Clean-room of the db-lyon FDialogHandlers idea — capture/chain/
	// restore on a single-bind core delegate — not its code.)
	PreviousModalDialogDelegate = FCoreDelegates::ModalMessageDialog;

	FCoreDelegates::ModalMessageDialog.BindUObject(
		this, &UUeMcpEditorSubsystem::HandleModalMessageDialog);

	bDialogAutoDeclineInstalled = true;

	UE_LOG(LogUeMcpEditor, Log,
		TEXT("dialog auto-decline hook installed (had-previous=%s)"),
		PreviousModalDialogDelegate.IsBound() ? TEXT("yes") : TEXT("no"));
}

void UUeMcpEditorSubsystem::StopDialogAutoDecline()
{
	if (!bDialogAutoDeclineInstalled)
	{
		return;
	}

	// Only restore if *we* are still the bound handler. If something else
	// rebound ModalMessageDialog after us, clobbering it back to the saved
	// (now stale) editor delegate would be worse than leaving it alone.
	if (FCoreDelegates::ModalMessageDialog.IsBoundToObject(this))
	{
		FCoreDelegates::ModalMessageDialog = PreviousModalDialogDelegate;
	}
	else
	{
		UE_LOG(LogUeMcpEditor, Warning,
			TEXT("StopDialogAutoDecline: ModalMessageDialog no longer bound to us; "
				 "leaving the current binding intact"));
	}

	PreviousModalDialogDelegate.Unbind();
	bDialogAutoDeclineInstalled = false;

	UE_LOG(LogUeMcpEditor, Log, TEXT("dialog auto-decline hook removed."));
}

EAppReturnType::Type UUeMcpEditorSubsystem::HandleModalMessageDialog(
	EAppMsgCategory MessageCategory,
	EAppMsgType::Type MessageType,
	const FText& Message,
	const FText& Title)
{
	using namespace UeMcpDialogAutoDecline;

	const FString LowerTitle = Title.ToString().ToLower();
	const FString LowerMessage = Message.ToString().ToLower();

	if (MatchesTrigger(LowerTitle) || MatchesTrigger(LowerMessage))
	{
		const EAppReturnType::Type Answer = DeclineAnswerFor(MessageType);
		UE_LOG(LogUeMcpEditor, Log,
			TEXT("dialog auto-declined: title='%s' -> %s"),
			*Title.ToString(), LexToString(Answer));
		return Answer;
	}

	// Not one of ours — hand back to the editor's original handler so
	// genuinely interactive prompts still work for a human-driven session.
	if (PreviousModalDialogDelegate.IsBound())
	{
		return PreviousModalDialogDelegate.Execute(
			MessageCategory, MessageType, Message, Title);
	}

	// No prior handler (headless / commandlet-shaped boot). Fall back to
	// the same conservative decline so we still never block the thread.
	const EAppReturnType::Type Fallback = DeclineAnswerFor(MessageType);
	UE_LOG(LogUeMcpEditor, Verbose,
		TEXT("dialog (no prior handler) auto-answered: title='%s' -> %s"),
		*Title.ToString(), LexToString(Fallback));
	return Fallback;
}

void UUeMcpEditorSubsystem::WaitForEditorReady()
{
	// One-shot ticker that polls until GEditor + the editor world context are live, logs once,
	// then unregisters. Pattern lifted from db-lyon UE_MCP_Bridge.cpp:47-61 (clean-room).
	//
	// This is also where we lift the dispatcher's editor-ready gate (issue
	// #57 mitigation #3): until this fires, Dispatch short-circuits non-meta
	// tools with a retryable EDITOR_NOT_READY instead of letting them hit
	// the 2 s game-thread timeout. We capture a weak pointer to the
	// subsystem so a teardown before GEditor is ready can't dangle — the
	// core ticker outlives Deinitialize (we never explicitly remove it).
	TWeakObjectPtr<UUeMcpEditorSubsystem> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([WeakThis](float /*DeltaTime*/) -> bool
		{
			UUeMcpEditorSubsystem* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				// Subsystem torn down before the editor came up — stop.
				return false;
			}

			if (!GEditor || !GEditor->GetEditorWorldContext(false).World())
			{
				// Not ready yet — keep ticking.
				return true;
			}

			if (Self->Dispatcher.IsValid())
			{
				Self->Dispatcher->SetEditorReady(true);
			}

			UE_LOG(LogUeMcpEditor, Log, TEXT("Editor ready — accepting requests."));
			// Returning false unregisters the ticker.
			return false;
		})
	);
}
