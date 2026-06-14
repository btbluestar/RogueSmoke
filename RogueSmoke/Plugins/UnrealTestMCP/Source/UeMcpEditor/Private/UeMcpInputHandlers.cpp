// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpInputHandlers.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"

#include "UeMcpDispatcher.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpInputHandlersPrivate
{
	/** Sync-handler dispatcher timeout. Input dispatch is one game-thread
	 *  hop + a couple of `UPlayerInput::InputKey` calls — well under 1s on
	 *  any conceivable hardware. 5s leaves headroom for game-thread queue
	 *  contention without making a misconfigured call wedge a run. */
	static constexpr double InputDispatcherTimeoutSeconds = 5.0;

	/** Bounds on `delta_time` for `input.simulate_axis`. UE clamps to a
	 *  small positive number internally; we reject obviously-wrong values
	 *  early so a typo (e.g. passing milliseconds) doesn't silently
	 *  produce garbage axis samples. */
	static constexpr double MinAxisDeltaTimeSeconds = 0.0;
	static constexpr double MaxAxisDeltaTimeSeconds = 10.0;

	/** Bounds on `num_samples` (per UE's simulated-input convention,
	 *  analog defaults to 1, digital defaults to 0; we accept up to 64
	 *  to allow accumulated mouse-delta packing without inviting abuse). */
	static constexpr int32 MinNumSamples = 0;
	static constexpr int32 MaxNumSamples = 64;

	/**
	 * Resolve the active local `APlayerController` in the requested world.
	 *
	 * Input simulation only makes sense against a live player controller —
	 * in PIE that's `World->GetFirstPlayerController()`; in the editor world
	 * outside PIE there is no PC and we surface `NOT_IN_PIE` so the caller
	 * gets a clear "you forgot to start PIE" signal rather than a generic
	 * NOT_FOUND on the controller.
	 */
	static APlayerController* ResolvePlayerController(
		UWorld* World,
		FString& OutErrorCode,
		FString& OutErrorMessage)
	{
		if (World == nullptr)
		{
			OutErrorCode = TEXT("EDITOR_NOT_READY");
			OutErrorMessage = TEXT("input handler invoked with null world");
			return nullptr;
		}

		APlayerController* PC = World->GetFirstPlayerController();
		if (PC == nullptr)
		{
			OutErrorCode = TEXT("NOT_IN_PIE");
			OutErrorMessage = TEXT(
				"No local PlayerController in the resolved world. "
				"Input simulation requires PIE (or SIE) to be active so a "
				"PlayerController exists to receive the synthesized event.");
			return nullptr;
		}
		return PC;
	}

	/**
	 * Resolve a wire `phase` string to the pair of `EInputEvent` values to
	 * emit. Returns false (and fills OutErr) on an unknown phase.
	 *
	 * Wire mapping:
	 *   "press"    → { IE_Pressed }
	 *   "release"  → { IE_Released }
	 *   "both"     → { IE_Pressed, IE_Released } (back-to-back)
	 */
	static bool ResolvePhaseEvents(
		const FString& Phase,
		TArray<EInputEvent, TInlineAllocator<2>>& OutEvents,
		FString& OutErr)
	{
		OutEvents.Reset();
		const FString Lower = Phase.ToLower();
		if (Lower == TEXT("press"))   { OutEvents.Add(IE_Pressed);  return true; }
		if (Lower == TEXT("release")) { OutEvents.Add(IE_Released); return true; }
		if (Lower == TEXT("both"))
		{
			OutEvents.Add(IE_Pressed);
			OutEvents.Add(IE_Released);
			return true;
		}
		OutErr = FString::Printf(
			TEXT("`phase` must be one of 'press', 'release', 'both' (got '%s')"),
			*Phase);
		return false;
	}

	/**
	 * Construct an `FKey` from a wire-supplied string and validate it. We
	 * do not constrain to a known whitelist — UE's `FKey::IsValid()` walks
	 * the registered key catalogue (which includes user-added input
	 * actions / virtual keys), so we trust the engine's answer.
	 */
	static bool ResolveKey(const FString& KeyName, FKey& OutKey, FString& OutErr)
	{
		OutKey = FKey(FName(*KeyName));
		if (!OutKey.IsValid())
		{
			OutErr = FString::Printf(
				TEXT("`key` '%s' is not a registered FKey. Use registered FName "
					 "tokens such as 'W', 'SpaceBar' (NOT 'Space'), "
					 "'LeftMouseButton', 'Gamepad_FaceButton_Bottom'."),
				*KeyName);
			return false;
		}
		return true;
	}

	/**
	 * Drive a single `FInputKeyEventArgs` into the player's `UPlayerInput`.
	 * `AmountDepressed` defaults to 1.0 for digital press, 0.0 for release;
	 * the axis variant overrides this with the caller's value.
	 */
	static bool DispatchOneKeyEvent(
		APlayerController* PC,
		const FKey& Key,
		EInputEvent EventType,
		float AmountDepressed,
		float DeltaTime,
		int32 NumSamples)
	{
		if (PC == nullptr || PC->PlayerInput == nullptr)
		{
			return false;
		}

		FInputKeyEventArgs Args = FInputKeyEventArgs::CreateSimulated(
			Key,
			EventType,
			AmountDepressed,
			NumSamples);
		Args.DeltaTime = DeltaTime;

		return PC->PlayerInput->InputKey(Args);
	}

	/**
	 * Build the response root common to all three handlers. Carries the
	 * resolved key name + which world the input landed in so callers can
	 * audit log lines without re-querying.
	 */
	static TSharedRef<FJsonObject> BuildBaseResponse(
		const FKey& Key,
		const UeMcp::FWorldResolution& World)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("key"), Key.GetFName().ToString());
		Data->SetStringField(TEXT("world"),
			UeMcp::WorldScopeToString(World.ResolvedScope));
		Data->SetBoolField(TEXT("is_pie"), World.bIsPIE);
		return Data;
	}

	// -----------------------------------------------------------------------
	// input.simulate_key
	// -----------------------------------------------------------------------

	/** `input.simulate_key` body. */
	static TSharedRef<FJsonObject> HandleSimulateKey(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		// 1) Args.
		FString KeyName;
		if (!Args->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`key` is required and must be a non-empty string"));
		}
		FString Phase;
		if (!Args->TryGetStringField(TEXT("phase"), Phase) || Phase.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`phase` is required (one of 'press', 'release', 'both')"));
		}

		FKey Key;
		FString KeyErr;
		if (!ResolveKey(KeyName, Key, KeyErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), KeyErr);
		}

		TArray<EInputEvent, TInlineAllocator<2>> Events;
		FString PhaseErr;
		if (!ResolvePhaseEvents(Phase, Events, PhaseErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), PhaseErr);
		}

		// 2) World + PC.
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}
		FString PCErrCode, PCErrMsg;
		APlayerController* PC = ResolvePlayerController(World.World, PCErrCode, PCErrMsg);
		if (PC == nullptr)
		{
			return UeMcp::MakeInlineError(PCErrCode, PCErrMsg);
		}

		// 3) Dispatch one event per phase. For digital keys, `1.0` for
		//    Pressed / `0.0` for Released matches what real input would
		//    produce — `UPlayerInput::InputKey` reads `AmountDepressed`
		//    to drive its key-state edge detection.
		int32 EventsDispatched = 0;
		bool bAnyConsumed = false;
		for (EInputEvent Ev : Events)
		{
			const float Amount = (Ev == IE_Pressed) ? 1.0f : 0.0f;
			const bool bConsumed = DispatchOneKeyEvent(
				PC, Key, Ev, Amount, /*DeltaTime*/ 1.0f / 60.0f, /*NumSamples*/ 0);
			++EventsDispatched;
			bAnyConsumed = bAnyConsumed || bConsumed;
		}

		// 4) Response.
		TSharedRef<FJsonObject> Data = BuildBaseResponse(Key, World);
		Data->SetStringField(TEXT("phase"), Phase.ToLower());
		Data->SetNumberField(TEXT("events_dispatched"), EventsDispatched);
		Data->SetBoolField(TEXT("consumed"), bAnyConsumed);
		return Data;
	}

	// -----------------------------------------------------------------------
	// input.simulate_axis
	// -----------------------------------------------------------------------

	/** `input.simulate_axis` body. */
	static TSharedRef<FJsonObject> HandleSimulateAxis(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		// 1) Args.
		FString KeyName;
		if (!Args->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`key` is required and must be a non-empty string "
					 "(axis name like 'MouseX', 'Gamepad_LeftX', 'MouseWheelAxis')"));
		}
		double ValueArg = 0.0;
		if (!Args->TryGetNumberField(TEXT("value"), ValueArg))
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`value` is required and must be a number"));
		}
		// `delta_time` is required to make the axis sample meaningful —
		// per-tick rates depend on it. Default 1/60s if omitted, matching
		// the engine's typical poll cadence.
		double DeltaTimeArg = 1.0 / 60.0;
		bool bHaveDelta = Args->TryGetNumberField(TEXT("delta_time"), DeltaTimeArg);
		if (bHaveDelta)
		{
			if (DeltaTimeArg < MinAxisDeltaTimeSeconds || DeltaTimeArg > MaxAxisDeltaTimeSeconds)
			{
				return UeMcp::MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("`delta_time` must be in [%.3f, %.1f] seconds (got %f)"),
						MinAxisDeltaTimeSeconds, MaxAxisDeltaTimeSeconds, DeltaTimeArg));
			}
		}
		// `num_samples` is optional. UE's `CreateSimulated` defaults to
		// 1 for analog keys, 0 for digital — we leave that contract
		// intact when the caller doesn't override.
		int32 NumSamples = -1;
		{
			int32 Parsed = -1;
			if (Args->TryGetNumberField(TEXT("num_samples"), Parsed))
			{
				if (Parsed < MinNumSamples || Parsed > MaxNumSamples)
				{
					return UeMcp::MakeInlineError(
						TEXT("SCHEMA_ERROR"),
						FString::Printf(
							TEXT("`num_samples` must be in [%d, %d] (got %d)"),
							MinNumSamples, MaxNumSamples, Parsed));
				}
				NumSamples = Parsed;
			}
		}

		FKey Key;
		FString KeyErr;
		if (!ResolveKey(KeyName, Key, KeyErr))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), KeyErr);
		}

		// 2) World + PC.
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}
		FString PCErrCode, PCErrMsg;
		APlayerController* PC = ResolvePlayerController(World.World, PCErrCode, PCErrMsg);
		if (PC == nullptr)
		{
			return UeMcp::MakeInlineError(PCErrCode, PCErrMsg);
		}

		// 3) Dispatch the axis sample. `IE_Axis` is the wire for analog
		//    samples; `AmountDepressed` carries the value, `NumSamples`
		//    differentiates "one big sample" from "N tiny accumulated".
		const float ValueF = static_cast<float>(ValueArg);
		const float DeltaF = static_cast<float>(DeltaTimeArg);
		const int32 Samples = (NumSamples >= 0)
			? NumSamples
			: (Key.IsAnalog() ? 1 : 0);

		const bool bConsumed = DispatchOneKeyEvent(
			PC, Key, IE_Axis, ValueF, DeltaF, Samples);

		// 4) Response.
		TSharedRef<FJsonObject> Data = BuildBaseResponse(Key, World);
		Data->SetNumberField(TEXT("value"), ValueArg);
		Data->SetNumberField(TEXT("delta_time"), DeltaTimeArg);
		Data->SetNumberField(TEXT("num_samples"), Samples);
		Data->SetBoolField(TEXT("consumed"), bConsumed);
		Data->SetBoolField(TEXT("is_analog"), Key.IsAnalog());
		return Data;
	}

	// -----------------------------------------------------------------------
	// input.tap
	// -----------------------------------------------------------------------

	/**
	 * `input.tap` body — `simulate_key` with `phase="both"` sugar.
	 *
	 * Same-frame Pressed+Released. Game logic that polls "is the key
	 * down right now?" will see down for one frame and up the next; logic
	 * that listens to OnPressed/OnReleased delegates sees both fire
	 * within the same handler invocation (ordering preserved).
	 *
	 * For a multi-frame hold (e.g. "press W and walk forward for 1s"),
	 * the caller composes `simulate_key(press)` + `pie.advance_seconds`
	 * + `simulate_key(release)` themselves — keeping `tap` strictly
	 * one-shot avoids smuggling time-control into an input primitive.
	 */
	static TSharedRef<FJsonObject> HandleTap(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		// Synthesise a `phase: both` arg copy and forward to simulate_key.
		// We don't mutate the caller's args because the dispatcher reuses
		// them across error-capture brackets.
		TSharedRef<FJsonObject> Forward = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Args->Values)
		{
			if (!Pair.Key.Equals(TEXT("phase"), ESearchCase::IgnoreCase))
			{
				Forward->SetField(Pair.Key, Pair.Value);
			}
		}
		Forward->SetStringField(TEXT("phase"), TEXT("both"));
		return HandleSimulateKey(Forward, Cancel);
	}
}

void UeMcp::RegisterInputHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpInputHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("input.simulate_key"));
		Reg.DefaultTimeoutSeconds = InputDispatcherTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSimulateKey);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("input.simulate_axis"));
		Reg.DefaultTimeoutSeconds = InputDispatcherTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSimulateAxis);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("input.tap"));
		Reg.DefaultTimeoutSeconds = InputDispatcherTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTap);
		Dispatcher.RegisterTool(Reg);
	}
}
