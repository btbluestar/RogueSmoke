// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpDispatcher.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"

#include "UeMcpGameThreadExecutor.h"

namespace UeMcpDispatcherPrivate
{
	// Error codes — must match 07_V0_PLAN.md §2.7 verbatim. Single source of
	// truth in string form; any handler emitting structured errors should use
	// the same vocabulary rather than free-form prose.
	static const FString ErrCodeUnknownTool    = TEXT("UNKNOWN_TOOL");
	static const FString ErrCodeTimeout        = TEXT("TIMEOUT");
	static const FString ErrCodeCancelled      = TEXT("CANCELLED");
	static const FString ErrCodeEditorBusy     = TEXT("EDITOR_BUSY");
	static const FString ErrCodeEditorNotReady = TEXT("EDITOR_NOT_READY");
	static const FString ErrCodeInternal       = TEXT("PLUGIN_INTERNAL_ERROR");

	/** The one tool allowed through before the editor signals ready: the
	 *  heartbeat. Its handler only reads uptime — no GEditor dependency —
	 *  so the bridge keeps its liveness signal during startup. (`cancel`
	 *  is intercepted in the transport layer before Dispatch is reached.) */
	static const FName ToolPing = FName(TEXT("ping"));

	/** Build a success response JSON object with the canonical wire shape. */
	static TSharedRef<FJsonObject> MakeSuccessResponse(
		const FGuid& Id,
		const TSharedPtr<FJsonObject>& Data,
		const TArray<FString>& Warnings)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("id"), Id.ToString(EGuidFormats::Digits));
		Out->SetBoolField(TEXT("ok"), true);
		if (Data.IsValid())
		{
			Out->SetObjectField(TEXT("data"), Data);
		}
		else
		{
			Out->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());
		}
		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> JsonWarnings;
			JsonWarnings.Reserve(Warnings.Num());
			for (const FString& W : Warnings)
			{
				JsonWarnings.Add(MakeShared<FJsonValueString>(W));
			}
			Out->SetArrayField(TEXT("warnings"), JsonWarnings);
		}
		return Out;
	}

	/** Build an error response JSON object with the canonical wire shape. */
	static TSharedRef<FJsonObject> MakeErrorResponse(
		const FGuid& Id,
		const FString& Code,
		const FString& Message,
		const TSharedPtr<FJsonObject>& Detail = nullptr)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("id"), Id.ToString(EGuidFormats::Digits));
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		if (Detail.IsValid())
		{
			Out->SetObjectField(TEXT("detail"), Detail);
		}
		return Out;
	}
}

FUeMcpDispatcher::FUeMcpDispatcher()
{
	StartupTime = FPlatformTime::Seconds();
	RegisterBuiltInTools();
}

FUeMcpDispatcher::~FUeMcpDispatcher()
{
	FScopeLock Lock(&ToolsLock);
	Tools.Reset();
}

void FUeMcpDispatcher::RegisterTool(const FUeMcpToolRegistration& Registration)
{
	checkf(Registration.ToolName != NAME_None,
		TEXT("RegisterTool: ToolName must be set"));
	const bool bHasSync = Registration.Handler.IsBound();
	const bool bHasPending = Registration.PendingHandler.IsBound();
	checkf(bHasSync || bHasPending,
		TEXT("RegisterTool(%s): one of Handler / PendingHandler must be bound"),
		*Registration.ToolName.ToString());
	checkf(!(bHasSync && bHasPending),
		TEXT("RegisterTool(%s): only one of Handler / PendingHandler may be bound"),
		*Registration.ToolName.ToString());

	FScopeLock Lock(&ToolsLock);
	checkf(!Tools.Contains(Registration.ToolName),
		TEXT("RegisterTool: duplicate registration for '%s' — silent collisions were a prior-art bug we refuse to reproduce"),
		*Registration.ToolName.ToString());

	Tools.Add(Registration.ToolName, Registration);

	UE_LOG(LogUeMcpRuntime, Verbose,
		TEXT("RegisterTool: '%s' registered (timeout=%.2fs, mutating=%s, pending=%s)"),
		*Registration.ToolName.ToString(),
		Registration.DefaultTimeoutSeconds,
		Registration.bMutating ? TEXT("true") : TEXT("false"),
		bHasPending ? TEXT("true") : TEXT("false"));
}

void FUeMcpDispatcher::UnregisterTool(const FName& ToolName)
{
	FScopeLock Lock(&ToolsLock);
	Tools.Remove(ToolName);
}

bool FUeMcpDispatcher::HasTool(const FName& ToolName) const
{
	FScopeLock Lock(&ToolsLock);
	return Tools.Contains(ToolName);
}

TSharedRef<FJsonObject> FUeMcpDispatcher::Dispatch(
	const FGuid& RequestId,
	const FName& ToolName,
	const TSharedRef<FJsonObject>& Args)
{
	using namespace UeMcpDispatcherPrivate;

	// Pre-ready short-circuit (issue #57 mitigation #3). Before the editor
	// subsystem signals editor-ready, the game thread is saturated with
	// editor boot work and the executor's ticker may not service a handler
	// inside the per-tool timeout (e.g. editor.set_unattended's 2 s). The
	// bridge fires set_unattended on connect; a raw TIMEOUT there made it
	// degrade into per-request-connection mode — fatal in combination with
	// the single-connection serve bug. Returning a *retryable*
	// EDITOR_NOT_READY instead lets the bridge back off and retry without
	// degrading. `ping` is exempt so the heartbeat survives startup;
	// `cancel` never reaches Dispatch (intercepted in the transport layer).
	if (!bEditorReady.Load() && ToolName != ToolPing)
	{
		UE_LOG(LogUeMcpRuntime, Verbose,
			TEXT("Dispatch: '%s' rejected EDITOR_NOT_READY (editor not ready yet, id=%s)"),
			*ToolName.ToString(),
			*RequestId.ToString(EGuidFormats::Digits));
		return MakeErrorResponse(RequestId, ErrCodeEditorNotReady,
			TEXT("Editor is still initializing; retry shortly."));
	}

	// Look up the tool under the registry lock. We copy the registration
	// record out so we don't hold the lock across the game-thread hop.
	FUeMcpToolRegistration Registration;
	{
		FScopeLock Lock(&ToolsLock);
		const FUeMcpToolRegistration* Found = Tools.Find(ToolName);
		if (Found == nullptr)
		{
			UE_LOG(LogUeMcpRuntime, Verbose,
				TEXT("Dispatch: unknown tool '%s' (id=%s)"),
				*ToolName.ToString(),
				*RequestId.ToString(EGuidFormats::Digits));
			return MakeErrorResponse(RequestId, ErrCodeUnknownTool,
				FString::Printf(TEXT("No tool registered for '%s'"), *ToolName.ToString()));
		}
		Registration = *Found;
	}

	UE_LOG(LogUeMcpRuntime, Verbose,
		TEXT("Dispatch: '%s' starting (id=%s)"),
		*ToolName.ToString(),
		*RequestId.ToString(EGuidFormats::Digits));

	// Capture by value into the game-thread closure. `Args` is already a
	// shared ref so the copy is cheap.
	const TSharedRef<FJsonObject> ArgsRef = Args;

	FUeMcpExecReport Report;
	if (Registration.PendingHandler.IsBound())
	{
		const FUeMcpToolPendingHandler PendingHandler = Registration.PendingHandler;
		FUeMcpPendingFactory Factory =
			[PendingHandler, ArgsRef](FUeMcpCancelToken& Cancel) -> FUeMcpPendingStep
			{
				if (!PendingHandler.IsBound())
				{
					return FUeMcpPendingStep();
				}
				return PendingHandler.Execute(ArgsRef, Cancel);
			};
		Report = Executor.ExecuteOnGameThreadPending(
			RequestId, MoveTemp(Factory), Registration.DefaultTimeoutSeconds);
	}
	else
	{
		const FUeMcpToolHandler Handler = Registration.Handler;
		TFunction<TSharedRef<FJsonObject>(FUeMcpCancelToken&)> Work =
			[Handler, ArgsRef](FUeMcpCancelToken& Cancel) -> TSharedRef<FJsonObject>
			{
				// The delegate returns a TSharedRef<FJsonObject>; we forward it.
				// If the handler is somehow unbound at invocation time, surface
				// an empty object — the caller will treat it as InternalError via
				// the returned Report.
				if (!Handler.IsBound())
				{
					return MakeShared<FJsonObject>();
				}
				return Handler.Execute(ArgsRef, Cancel);
			};
		Report = Executor.ExecuteOnGameThread(
			RequestId, MoveTemp(Work), Registration.DefaultTimeoutSeconds);
	}

	// Slow-op log: anything over 250ms is worth noting at Log level.
	if (Report.ElapsedMs > 250.0)
	{
		UE_LOG(LogUeMcpRuntime, Log,
			TEXT("Dispatch: '%s' took %.2fms (id=%s, outcome=%d)"),
			*ToolName.ToString(), Report.ElapsedMs,
			*RequestId.ToString(EGuidFormats::Digits),
			(int32)Report.Result);
	}
	else
	{
		UE_LOG(LogUeMcpRuntime, Verbose,
			TEXT("Dispatch: '%s' took %.2fms (id=%s, outcome=%d)"),
			*ToolName.ToString(), Report.ElapsedMs,
			*RequestId.ToString(EGuidFormats::Digits),
			(int32)Report.Result);
	}

	switch (Report.Result)
	{
	case EUeMcpExecResult::Success:
	{
		// Handler-returned error hoist: if the handler populated
		// {error: "CODE", message: "..."} at the root of the returned
		// object, promote it to the top-level wire shape rather than
		// burying it in `data`. This is the contract handlers use to
		// signal structured errors without exceptions.
		if (Report.Data.IsValid())
		{
			FString HoistedCode;
			if (Report.Data->TryGetStringField(TEXT("error"), HoistedCode)
				&& !HoistedCode.IsEmpty())
			{
				FString HoistedMessage;
				Report.Data->TryGetStringField(TEXT("message"), HoistedMessage);
				const TSharedPtr<FJsonObject>* HoistedDetail = nullptr;
				Report.Data->TryGetObjectField(TEXT("detail"), HoistedDetail);
				return MakeErrorResponse(RequestId, HoistedCode, HoistedMessage,
					HoistedDetail != nullptr ? *HoistedDetail : nullptr);
			}
		}

		// If the handler returned ok but the error-capture device saw ERROR
		// lines, we surface those in `warnings` rather than promoting to a
		// hard failure. The handler was in the best position to know whether
		// the errors were fatal, and by returning a value it told us "no."
		TArray<FString> AllWarnings = MoveTemp(Report.CapturedWarnings);
		for (FString& E : Report.CapturedErrors)
		{
			AllWarnings.Add(MoveTemp(E));
		}
		return MakeSuccessResponse(RequestId, Report.Data, AllWarnings);
	}
	case EUeMcpExecResult::TimedOut:
		return MakeErrorResponse(RequestId, ErrCodeTimeout, Report.ErrorMessage);

	case EUeMcpExecResult::Cancelled:
		return MakeErrorResponse(RequestId, ErrCodeCancelled,
			Report.ErrorMessage.IsEmpty() ? TEXT("Cancelled") : Report.ErrorMessage);

	case EUeMcpExecResult::DeferredEngineBusy:
		return MakeErrorResponse(RequestId, ErrCodeEditorBusy, Report.ErrorMessage);

	case EUeMcpExecResult::InternalError:
	default:
		return MakeErrorResponse(RequestId, ErrCodeInternal,
			Report.ErrorMessage.IsEmpty() ? TEXT("Internal dispatcher error") : Report.ErrorMessage);
	}
}

void FUeMcpDispatcher::Cancel(const FGuid& RequestId)
{
	Executor.Cancel(RequestId);
}

void FUeMcpDispatcher::RegisterBuiltInTools()
{
	// `ping` — the heartbeat tool. Runs on the game thread like everything
	// else so a wedged game thread will cause `ping` to time out, which is
	// exactly the signal the transport layer wants for liveness detection.
	FUeMcpToolRegistration Ping;
	Ping.ToolName = FName(TEXT("ping"));
	Ping.DefaultTimeoutSeconds = 1.0;
	Ping.bMutating = false;
	const double CapturedStartup = StartupTime;
	Ping.Handler.BindLambda(
		[CapturedStartup](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/) -> TSharedRef<FJsonObject>
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("pong"), true);
			const int64 UptimeMs = (int64)((FPlatformTime::Seconds() - CapturedStartup) * 1000.0);
			Data->SetNumberField(TEXT("server_uptime_ms"), (double)UptimeMs);
			return Data;
		});
	RegisterTool(Ping);
}
