// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave E / v2_pivot Gap 1b — `event.wait_for_delegate`.
//
// Pending-handler that binds a UFUNCTION on a relay UObject to a
// multicast delegate property by name on a target UObject, then ticks
// each engine frame waiting for the delegate to fire (or the timeout to
// expire). Cleanup is destructor-driven on the captured state struct so
// the binding is torn down regardless of the exit path (Done / Failed /
// cancelled / abandoned).
//
// Why pending and not a synchronous handler: the delegate fire event is
// driven by other game-thread work (actor destruction, overlap, hit) —
// blocking the game thread until it fires would starve the very ticks
// that produce the event. The pending variant yields between checks.
//
// Why a separate UObject (`UUeMcpDelegateRelay`) instead of a lambda:
// every `DECLARE_DYNAMIC_MULTICAST_DELEGATE_*` is bound by
// `FScriptDelegate::BindUFunction(UObject*, FName)` — the engine's
// dynamic-multicast machinery resolves the function via
// `UObject::FindFunction` at fire time, so the bind target MUST be a
// UObject with a UFUNCTION-tagged method. Plain TFunction won't bind.
//
// Why we add the relay to root: the multicast holds an FScriptDelegate
// that references our relay by raw pointer; if the relay is GC'd while
// the bind is still live, the next fire would crash. AddToRoot keeps
// it alive for the duration of the request; the destructor calls
// RemoveFromRoot.
//
// 2s wall cap: the plugin socket is one-request-at-a-time and the
// Python client times out individual calls at ~4s. Any handler that
// blocks longer drops the connection. We cap below the client timeout
// even when the caller asked for longer; at expiry we return
// `{fired: false, timed_out: true}` rather than letting the socket die.

#include "UeMcpEventHandlers.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "UeMcpDelegateRelay.h"
#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeMcpEvent, Log, All);

namespace UeMcpEventHandlersPrivate
{
	/**
	 * Wall-clock ceiling for any single `event.wait_for_delegate` call.
	 * Mirrors `UeMcpPieHandlersPrivate::AdvanceFramesWallCapSeconds` —
	 * the Python client times out individual `conn.call` requests after
	 * about 4s, so anything longer drops the socket. Caller-supplied
	 * `timeout_ms` shorter than this still wins.
	 */
	static constexpr double WaitForDelegateWallCapSeconds = 2.0;

	/** Default timeout when the caller omits `timeout_ms`. */
	static constexpr int32 DefaultTimeoutMs = 1500;

	/** Hard ceiling so a misconfigured call can't request > the wall cap. */
	static constexpr int32 MaxTimeoutMs = 60 * 1000; // 60s — clamped by wall cap anyway

	/** Dispatcher timeout for the pending handler. Wall cap + slack. */
	static constexpr double DispatcherTimeoutSeconds = 30.0;

	/**
	 * Walk the property chain on `Object`'s class looking for a multicast
	 * delegate property by name. Returns nullptr if not found OR if the
	 * named property is not a multicast delegate.
	 *
	 * Both `FMulticastInlineDelegateProperty` and
	 * `FMulticastSparseDelegateProperty` derive from
	 * `FMulticastDelegateProperty`, which exposes the `AddDelegate` /
	 * `RemoveDelegate` virtuals we need. We bind through that base class
	 * and let the derived implementation do the right storage thing.
	 */
	static FMulticastDelegateProperty* FindMulticastDelegateProperty(
		UObject* Object, const FName& PropName)
	{
		if (Object == nullptr || PropName.IsNone())
		{
			return nullptr;
		}
		UClass* Class = Object->GetClass();
		if (Class == nullptr)
		{
			return nullptr;
		}
		FProperty* Prop = FindFProperty<FProperty>(Class, PropName);
		if (Prop == nullptr)
		{
			return nullptr;
		}
		return CastField<FMulticastDelegateProperty>(Prop);
	}

	/** Build the per-request response payload. */
	static TSharedRef<FJsonObject> BuildResult(
		bool bFired,
		bool bTimedOut,
		bool bCancelled,
		double ElapsedMs,
		const FString& ObjectRef,
		const FString& DelegateProp)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("fired"), bFired);
		if (bTimedOut)
		{
			Out->SetBoolField(TEXT("timed_out"), true);
		}
		if (bCancelled)
		{
			Out->SetBoolField(TEXT("cancelled"), true);
		}
		Out->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);
		Out->SetStringField(TEXT("object_ref"), ObjectRef);
		Out->SetStringField(TEXT("delegate_property"), DelegateProp);
		return Out;
	}

	/**
	 * Per-request state for the pending `event.wait_for_delegate` handler.
	 *
	 * Owns the relay UObject + the bind into the multicast. The destructor
	 * is the unconditional teardown — it runs whether the request retired
	 * normally (Done/Failed) or was abandoned by the executor (caller
	 * timeout, plugin shutdown). The pattern mirrors `FAdvanceSecondsState`
	 * in `UeMcpPieHandlers.cpp`: detect off-thread destruction and
	 * dispatch the UObject-touching teardown back to the game thread.
	 */
	struct FWaitForDelegateState
	{
		FString ObjectRef;
		FString DelegateProp;
		int32 TimeoutMs = DefaultTimeoutMs;

		// Captured relay + the multicast we bound into. Both are needed
		// at teardown to RemoveDelegate and clear AddToRoot.
		TWeakObjectPtr<UObject> Target;
		TWeakObjectPtr<UUeMcpDelegateRelay> Relay;

		// FName of the multicast property — we re-find it at teardown
		// rather than holding the FProperty* (which is owned by the class
		// and stable, but holding the FName keeps the contract simpler).
		FName DelegatePropName;

		// Shared fire-flag.
		TSharedPtr<FUeMcpDelegateRelayState> Shared;

		// Wall-clock budget management.
		double StartWallSeconds = 0.0;

		// Cancel hook from the executor.
		FUeMcpCancelToken* Cancel = nullptr;

		// One-shot guards so re-entry through the destructor (e.g. if
		// the closure released its ref while another thread held one) is
		// safe.
		bool bUnbound = false;

		~FWaitForDelegateState()
		{
			if (bUnbound)
			{
				return;
			}

			TWeakObjectPtr<UObject> TargetWeak = Target;
			TWeakObjectPtr<UUeMcpDelegateRelay> RelayWeak = Relay;
			const FName PropName = DelegatePropName;

			bUnbound = true;

			auto DoTeardown = [TargetWeak, RelayWeak, PropName]()
			{
				UUeMcpDelegateRelay* RelayObj = RelayWeak.Get();
				if (RelayObj == nullptr)
				{
					return;
				}
				if (UObject* TargetObj = TargetWeak.Get())
				{
					FMulticastDelegateProperty* Prop =
						FindMulticastDelegateProperty(TargetObj, PropName);
					if (Prop != nullptr)
					{
						FScriptDelegate Bind;
						Bind.BindUFunction(RelayObj,
							GET_FUNCTION_NAME_CHECKED(
								UUeMcpDelegateRelay, OnDelegateFired));
						Prop->RemoveDelegate(Bind, TargetObj);
					}
				}
				// Pair AddToRoot from BuildWaitForDelegateStep — relay
				// becomes GC-eligible. The shared FireFlag stays valid
				// until both the relay and any pending request closure
				// have dropped their refs; a late multicast fire is a
				// safe no-op (flips a bool nobody reads).
				RelayObj->RemoveFromRoot();
			};

			if (IsInGameThread())
			{
				DoTeardown();
				return;
			}

			// Off-thread destruction: dispatch UObject teardown to the
			// game thread. Same pattern as `FAdvanceSecondsState`.
			AsyncTask(ENamedThreads::GameThread, MoveTemp(DoTeardown));
		}
	};

	/** Build a step closure that fails immediately with `Payload`. */
	static FUeMcpPendingStep MakeImmediateFailStep(TSharedRef<FJsonObject> Payload)
	{
		return [Payload](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			Out = Payload;
			return EUeMcpStep::Failed;
		};
	}

	/**
	 * `event.wait_for_delegate` factory.
	 *
	 * Validates args, resolves the target object, locates the multicast
	 * property by name, allocates the relay UObject, binds it into the
	 * multicast, and returns the pending step closure that polls the
	 * shared fire-flag.
	 */
	static FUeMcpPendingStep BuildWaitForDelegateStep(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		TSharedRef<FWaitForDelegateState> S = MakeShared<FWaitForDelegateState>();
		S->Cancel = &Cancel;
		S->StartWallSeconds = FPlatformTime::Seconds();

		// --- World + arg parsing ---
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage));
		}

		if (!Args->TryGetStringField(TEXT("object_ref"), S->ObjectRef)
			|| S->ObjectRef.IsEmpty())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`object_ref` is required and must be a non-empty string")));
		}
		if (!Args->TryGetStringField(TEXT("delegate_property"), S->DelegateProp)
			|| S->DelegateProp.IsEmpty())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`delegate_property` is required and must be a non-empty string")));
		}

		{
			double Raw = static_cast<double>(DefaultTimeoutMs);
			if (Args->TryGetNumberField(TEXT("timeout_ms"), Raw))
			{
				if (Raw < 0.0 || Raw > static_cast<double>(MaxTimeoutMs))
				{
					return MakeImmediateFailStep(
						UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
							FString::Printf(
								TEXT("`timeout_ms` must be in [0, %d]"),
								MaxTimeoutMs)));
				}
				S->TimeoutMs = static_cast<int32>(Raw);
			}
		}

		// --- Resolve target ---
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(S->ObjectRef, World.World);
		if (!Resolved.IsOk())
		{
			return MakeImmediateFailStep(Resolved.ErrorInfo.ToSharedRef());
		}
		S->Target = Resolved.Object;
		S->DelegatePropName = FName(*S->DelegateProp);

		// --- Locate the multicast property ---
		FMulticastDelegateProperty* Prop =
			FindMulticastDelegateProperty(Resolved.Object, S->DelegatePropName);
		if (Prop == nullptr)
		{
			TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
				TEXT("PROP_NOT_FOUND"),
				FString::Printf(
					TEXT("Multicast delegate property '%s' not found on '%s' (class %s)"),
					*S->DelegateProp, *S->ObjectRef,
					*Resolved.Object->GetClass()->GetName()));
			return MakeImmediateFailStep(Err);
		}

		// --- Allocate + bind the relay ---
		// Outer the relay to the transient package so it doesn't try to
		// follow Resolved.Object's lifetime; we manage GC liveness via
		// AddToRoot / RemoveFromRoot ourselves.
		UUeMcpDelegateRelay* Relay = NewObject<UUeMcpDelegateRelay>(
			GetTransientPackage(), UUeMcpDelegateRelay::StaticClass(),
			NAME_None, RF_Transient);
		if (Relay == nullptr)
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
					TEXT("Failed to allocate UUeMcpDelegateRelay")));
		}
		S->Relay = Relay;
		S->Shared = MakeShared<FUeMcpDelegateRelayState>();
		Relay->Init(S->Shared.ToSharedRef());
		Relay->AddToRoot();

		FScriptDelegate Bind;
		Bind.BindUFunction(Relay,
			GET_FUNCTION_NAME_CHECKED(UUeMcpDelegateRelay, OnDelegateFired));
		Prop->AddDelegate(Bind, Resolved.Object);

		UE_LOG(LogUeMcpEvent, Verbose,
			TEXT("event.wait_for_delegate: bound to %s.%s (class %s) timeout_ms=%d"),
			*S->ObjectRef, *S->DelegateProp,
			*Resolved.Object->GetClass()->GetName(),
			S->TimeoutMs);

		// Per-tick step. Captures S by shared ref so the bind state
		// persists across the request's lifetime regardless of which
		// thread holds the executor's reference at any point.
		return [S](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			check(IsInGameThread());

			const double NowSeconds = FPlatformTime::Seconds();
			const double ElapsedSeconds = NowSeconds - S->StartWallSeconds;
			const double ElapsedMs = ElapsedSeconds * 1000.0;

			const double CallerTimeoutSeconds =
				static_cast<double>(S->TimeoutMs) / 1000.0;
			const double EffectiveTimeoutSeconds = FMath::Min(
				CallerTimeoutSeconds, WaitForDelegateWallCapSeconds);

			const bool bFired = S->Shared.IsValid()
				&& S->Shared->bFired.Load();

			const bool bCancelled = S->Cancel != nullptr
				&& S->Cancel->IsCancellationRequested();

			const bool bTimedOut = ElapsedSeconds >= EffectiveTimeoutSeconds;

			if (bFired || bCancelled || bTimedOut)
			{
				Out = BuildResult(
					/*bFired*/ bFired,
					/*bTimedOut*/ bTimedOut && !bFired,
					/*bCancelled*/ bCancelled && !bFired,
					/*ElapsedMs*/ ElapsedMs,
					S->ObjectRef,
					S->DelegateProp);
				return EUeMcpStep::Done;
			}

			return EUeMcpStep::Continue;
		};
	}

	/**
	 * `event.subscribe` body.
	 *
	 * Reserved name. v0 has no durable subscription channel — the plugin
	 * socket is one-request-at-a-time and the Python client model is
	 * call/response, not server-push. Returning a structured
	 * `NOT_IMPLEMENTED` keeps the wire stable so a future durable model
	 * can fill it in without renaming.
	 */
	static TSharedRef<FJsonObject> HandleEventSubscribe(
		const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		return UeMcp::MakeInlineError(
			TEXT("NOT_IMPLEMENTED"),
			TEXT("event.subscribe requires a durable subscription channel "
				"the v0 plugin socket does not provide. Use "
				"event.wait_for_delegate for one-shot binds."));
	}

	/** `event.unsubscribe` body — symmetric stub to `event.subscribe`. */
	static TSharedRef<FJsonObject> HandleEventUnsubscribe(
		const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		return UeMcp::MakeInlineError(
			TEXT("NOT_IMPLEMENTED"),
			TEXT("event.unsubscribe is reserved alongside event.subscribe; "
				"v0 has no durable subscription channel."));
	}
}

void UeMcp::RegisterEventHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpEventHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("event.wait_for_delegate"));
		Reg.DefaultTimeoutSeconds = DispatcherTimeoutSeconds;
		Reg.bMutating = false;
		Reg.PendingHandler =
			FUeMcpToolPendingHandler::CreateStatic(&BuildWaitForDelegateStep);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("event.subscribe"));
		Reg.DefaultTimeoutSeconds = 5.0;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleEventSubscribe);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("event.unsubscribe"));
		Reg.DefaultTimeoutSeconds = 5.0;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleEventUnsubscribe);
		Dispatcher.RegisterTool(Reg);
	}
}
