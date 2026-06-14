// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Logging/LogMacros.h"
#include "Templates/UniquePtr.h"

// Full includes (not forward declarations): the TUniquePtr<> members
// below need a complete type for UHT's generated destructor.
#include "UeMcpDispatcher.h"
#include "UeMcpServerRunnable.h"

#include "UeMcpEditorSubsystem.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUeMcpEditor, Log, All);

/**
 * Editor subsystem that owns the unreal-test-mcp server lifecycle.
 *
 * Holds the dispatcher (owner), the transport runnable (owner), and all
 * tool handlers registered against the dispatcher. Follows the
 * `UUnrealMCPBridge` shape from prior art, cleaned of its defects.
 */
UCLASS()
class UUeMcpEditorSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	/** Boots the dialog auto-decline hook, editor-ready ticker, and the transport server. */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Tears down the server and releases hooks installed during Initialize. */
	virtual void Deinitialize() override;

private:
	/** Bring the transport listener + dispatcher online. */
	void StartServer();

	/** Mirror of StartServer — stops the listener and drains in-flight commands. */
	void StopServer();

	/** Install the global "auto-decline save/overwrite modals" hook so PIE teardown cannot wedge. */
	void StartDialogAutoDecline();

	/** Restore the prior `FCoreDelegates::ModalMessageDialog` binding installed before our hook. */
	void StopDialogAutoDecline();

	/**
	 * Hook body bound to `FCoreDelegates::ModalMessageDialog`. Substring-
	 * matches the title/message against the auto-decline patterns; on a
	 * match returns a non-blocking "decline" answer, otherwise forwards
	 * to the editor's original handler (or a safe default if none).
	 */
	EAppReturnType::Type HandleModalMessageDialog(
		EAppMsgCategory MessageCategory,
		EAppMsgType::Type MessageType,
		const FText& Message,
		const FText& Title);

	/** One-shot ticker that fires once GEditor and the editor world context are live. */
	void WaitForEditorReady();

	/** Owns the dispatcher for the lifetime of the subsystem. */
	TUniquePtr<FUeMcpDispatcher> Dispatcher;

	/** Owns the listener runnable; created in StartServer, destroyed in StopServer. */
	TUniquePtr<FUeMcpServerRunnable> Server;

	/**
	 * The `FCoreDelegates::ModalMessageDialog` binding that was installed
	 * before ours (the editor engine's `OnModalMessageDialog`). It is a
	 * single-bind `TDelegate`, so we capture the prior binding, forward
	 * non-matching dialogs to it, and restore it on Deinitialize.
	 */
	TDelegate<EAppReturnType::Type(EAppMsgCategory, EAppMsgType::Type, const FText&, const FText&)>
		PreviousModalDialogDelegate;

	/** True once we have rebound `ModalMessageDialog`; gates StopDialogAutoDecline. */
	bool bDialogAutoDeclineInstalled = false;
};
