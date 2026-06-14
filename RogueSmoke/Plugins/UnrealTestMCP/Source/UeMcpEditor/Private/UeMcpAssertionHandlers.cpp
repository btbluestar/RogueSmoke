// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave E / Gap 1 — `state.wait_until`.
//
// The primitive every realistic gameplay test reduces to: "poll property
// X on actor Y until comparison(value) holds or timeout." Today every
// test reimplements this with 20-50 lines of Python asyncio.sleep +
// conn.call loop. This handler is the one-line replacement.
//
// Execution shape:
//   * Registered as a pending-style handler: the factory validates args +
//     resolves the object, returns a step closure that captures the parsed
//     state, and the executor invokes that closure once per game-thread
//     tick until it returns Done.
//   * Each tick reads the property via `FUeMcpPropertyAccessor::GetValue`.
//     `poll_ms` acts as a throttle — the step returns Continue without
//     reading if the prior poll was less than `poll_ms` ago. Ticks happen
//     at the engine's frame rate; poll_ms < 16ms effectively polls every
//     frame.
//   * The step does NOT sleep. Between ticks the game thread is free to
//     run PIE, AI, animation, damage etc., which is exactly what was
//     broken in the pre-pending version (see `d1b6a…` history) — a
//     `FPlatformProcess::Sleep` on the game thread starved the PIE tick
//     and made every tick-driven state-change test flake.
//   * Respects `FUeMcpCancelToken` — the executor flips it on cancel +
//     timeout; the next step invocation sees it and returns Done with
//     `cancelled: true`. The outer executor timeout (set to `timeout_ms
//     + slack`) is the hard ceiling.
//
// Non-match at timeout is NOT an error. We return `matched: false` with
// the final_value and elapsed_ms. The caller decides how to report —
// this is the soft-assert primitive, not the hard-fail one.
//
// Comparison semantics:
//   * `eq` / `neq` — JSON equality for all types. Numeric types honour
//     `tolerance` when provided; non-numeric types ignore it.
//   * `lt` / `le` / `gt` / `ge` — numeric only (bool coerced to 0/1 is
//     allowed; strings use lexicographic order via FString::Compare).
//   * `truthy` / `falsy` — no `value` arg; booleanness of the read value
//     (bool, non-zero number, non-empty string/array/object).
//
// Why a dedicated handler, not a BP node: tests ship as Python scripts
// authored by an agent. The agent already knows the tool; we don't want
// to force it into Blueprint graph authoring for every wait-for.

#include "UeMcpAssertionHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FunctionalTest.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "Math/UnrealMathUtility.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpPropertyAccessor.h"
#include "UeMcpPropertyValue.h"
#include "UeMcpWorldResolver.h"

// File-local log category for test-assert-property diagnostics. The wire
// category (`LogUeMcpTest`) is what appears in `log.tail` output.
DEFINE_LOG_CATEGORY_STATIC(LogUeMcpTest, Log, All);

namespace UeMcpAssertionHandlersPrivate
{
	/**
	 * Hard ceilings — the executor also has a 600s sanity cap, so we
	 * clamp lower here to keep a misconfigured call from wedging a run
	 * for 10 minutes.
	 */
	static constexpr int32 MaxTimeoutMs = 120 * 1000;   // 2 min
	static constexpr int32 MinPollMs    = 1;            // can't poll faster than 1 ms
	static constexpr int32 MaxPollMs    = 5 * 1000;     // 5 s per poll

	static constexpr int32 DefaultTimeoutMs = 5000;
	static constexpr int32 DefaultPollMs    = 50;

	/**
	 * Comparison operators. Wire strings kept stable.
	 */
	enum class EOp : uint8
	{
		Eq,
		Neq,
		Lt,
		Le,
		Gt,
		Ge,
		Truthy,
		Falsy,
	};

	static const TCHAR* OpToWire(EOp Op)
	{
		switch (Op)
		{
			case EOp::Eq:     return TEXT("eq");
			case EOp::Neq:    return TEXT("neq");
			case EOp::Lt:     return TEXT("lt");
			case EOp::Le:     return TEXT("le");
			case EOp::Gt:     return TEXT("gt");
			case EOp::Ge:     return TEXT("ge");
			case EOp::Truthy: return TEXT("truthy");
			case EOp::Falsy:  return TEXT("falsy");
		}
		return TEXT("unknown");
	}

	static bool ParseOp(const FString& In, EOp& OutOp)
	{
		const FString Lower = In.ToLower();
		if (Lower == TEXT("eq"))     { OutOp = EOp::Eq;     return true; }
		if (Lower == TEXT("neq"))    { OutOp = EOp::Neq;    return true; }
		if (Lower == TEXT("lt"))     { OutOp = EOp::Lt;     return true; }
		if (Lower == TEXT("le"))     { OutOp = EOp::Le;     return true; }
		if (Lower == TEXT("gt"))     { OutOp = EOp::Gt;     return true; }
		if (Lower == TEXT("ge"))     { OutOp = EOp::Ge;     return true; }
		if (Lower == TEXT("truthy")) { OutOp = EOp::Truthy; return true; }
		if (Lower == TEXT("falsy"))  { OutOp = EOp::Falsy;  return true; }
		return false;
	}

	static bool IsUnaryOp(EOp Op)
	{
		return Op == EOp::Truthy || Op == EOp::Falsy;
	}

	static bool IsOrderingOp(EOp Op)
	{
		return Op == EOp::Lt || Op == EOp::Le || Op == EOp::Gt || Op == EOp::Ge;
	}

	/**
	 * Try to pull a double out of a JsonValue. Numbers succeed directly;
	 * bools coerce to 0/1; strings parse when they look numeric;
	 * otherwise fail. Used for ordering ops and for numeric eq with
	 * tolerance.
	 */
	static bool TryAsNumber(const TSharedPtr<FJsonValue>& V, double& Out)
	{
		if (!V.IsValid()) { return false; }
		switch (V->Type)
		{
		case EJson::Number:
			Out = V->AsNumber();
			return true;
		case EJson::Boolean:
			Out = V->AsBool() ? 1.0 : 0.0;
			return true;
		case EJson::String:
		{
			const FString S = V->AsString();
			if (S.IsNumeric())
			{
				Out = FCString::Atod(*S);
				return true;
			}
			return false;
		}
		default:
			return false;
		}
	}

	/**
	 * Truthiness per "JSON-flavoured Python": null/false/zero/empty-*
	 * are falsy, everything else truthy.
	 */
	static bool IsTruthy(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) { return false; }
		switch (V->Type)
		{
		case EJson::Null:    return false;
		case EJson::Boolean: return V->AsBool();
		case EJson::Number:  return V->AsNumber() != 0.0;
		case EJson::String:  return !V->AsString().IsEmpty();
		case EJson::Array:   return V->AsArray().Num() > 0;
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (V->TryGetObject(Obj) && Obj != nullptr && (*Obj).IsValid())
			{
				return (*Obj)->Values.Num() > 0;
			}
			return false;
		}
		default:
			return false;
		}
	}

	/**
	 * Serialise a JsonValue to a compact string for structural equality
	 * (struct/object/array). Cheap and correct for the "did the value
	 * match" case; not intended to be a canonical form (key order
	 * follows FJsonObject's internal map order, which is insertion).
	 */
	static FString SerializeCompact(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) { return TEXT("null"); }
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(V.ToSharedRef(), TEXT(""), Writer);
		return Out;
	}

	/**
	 * Equality test between two JSON values. Numeric types honour the
	 * optional tolerance. Strings, bools, null compare directly. All
	 * other types serialise-and-compare.
	 *
	 * Returns true if the two values are equal under the op's semantics.
	 */
	static bool JsonEquals(
		const TSharedPtr<FJsonValue>& A,
		const TSharedPtr<FJsonValue>& B,
		bool bHaveTolerance,
		double Tolerance)
	{
		if (!A.IsValid() || !B.IsValid())
		{
			return A.IsValid() == B.IsValid();
		}

		// Numeric path: both sides should coerce. If either fails, fall
		// through to string-compare.
		double NA = 0.0, NB = 0.0;
		const bool bANum = TryAsNumber(A, NA);
		const bool bBNum = TryAsNumber(B, NB);
		if (bANum && bBNum)
		{
			const double Tol = bHaveTolerance ? Tolerance : 0.0;
			return FMath::Abs(NA - NB) <= Tol;
		}

		// Same-type non-numeric: quick paths.
		if (A->Type == B->Type)
		{
			switch (A->Type)
			{
			case EJson::Null:
				return true;
			case EJson::Boolean:
				return A->AsBool() == B->AsBool();
			case EJson::String:
				return A->AsString().Equals(B->AsString());
			default:
				break;
			}
		}

		// Fallback: structural compare via serialised form.
		return SerializeCompact(A).Equals(SerializeCompact(B));
	}

	/**
	 * Ordering comparison: numeric if both sides coerce; otherwise
	 * lexicographic on stringified form. Returns true if `A OP B` holds
	 * under the given ordering op.
	 */
	static bool JsonOrder(
		const TSharedPtr<FJsonValue>& A,
		const TSharedPtr<FJsonValue>& B,
		EOp Op,
		bool& bOutComparable)
	{
		bOutComparable = true;
		double NA = 0.0, NB = 0.0;
		if (TryAsNumber(A, NA) && TryAsNumber(B, NB))
		{
			switch (Op)
			{
			case EOp::Lt: return NA <  NB;
			case EOp::Le: return NA <= NB;
			case EOp::Gt: return NA >  NB;
			case EOp::Ge: return NA >= NB;
			default:      bOutComparable = false; return false;
			}
		}

		// String fallback — only when both sides are strings; otherwise
		// refuse to compare (caller gets TYPE_MISMATCH at the top level).
		if (A.IsValid() && B.IsValid()
			&& A->Type == EJson::String && B->Type == EJson::String)
		{
			const int32 Cmp = A->AsString().Compare(B->AsString());
			switch (Op)
			{
			case EOp::Lt: return Cmp <  0;
			case EOp::Le: return Cmp <= 0;
			case EOp::Gt: return Cmp >  0;
			case EOp::Ge: return Cmp >= 0;
			default:      bOutComparable = false; return false;
			}
		}

		bOutComparable = false;
		return false;
	}

	/**
	 * Apply the comparison op. `Expected` is unused for Truthy/Falsy.
	 * On success, `bOutMatch` carries the boolean outcome. On type
	 * mismatch (e.g. ordering op against non-numeric + non-string),
	 * returns false and `bOutMatch` is undefined — the caller should
	 * treat that as a permanent failure, not a retryable poll.
	 */
	static bool EvaluateOp(
		EOp Op,
		const TSharedPtr<FJsonValue>& Actual,
		const TSharedPtr<FJsonValue>& Expected,
		bool bHaveTolerance,
		double Tolerance,
		bool& bOutMatch,
		FString& OutTypeError)
	{
		switch (Op)
		{
		case EOp::Truthy: bOutMatch =  IsTruthy(Actual); return true;
		case EOp::Falsy:  bOutMatch = !IsTruthy(Actual); return true;
		case EOp::Eq:     bOutMatch =  JsonEquals(Actual, Expected, bHaveTolerance, Tolerance); return true;
		case EOp::Neq:    bOutMatch = !JsonEquals(Actual, Expected, bHaveTolerance, Tolerance); return true;
		case EOp::Lt:
		case EOp::Le:
		case EOp::Gt:
		case EOp::Ge:
		{
			bool bComparable = false;
			const bool bRes = JsonOrder(Actual, Expected, Op, bComparable);
			if (!bComparable)
			{
				OutTypeError = FString::Printf(
					TEXT("Ordering op '%s' requires numeric (or string+string) operands"),
					OpToWire(Op));
				return false;
			}
			bOutMatch = bRes;
			return true;
		}
		}
		OutTypeError = TEXT("Unknown op");
		return false;
	}

	/**
	 * Build the inline-error root for accessor failures. Accessor-error ->
	 * wire-code goes through the shared `UeMcp::AccessorErrorToCode`
	 * (issue #62); no local switch here.
	 */
	static TSharedRef<FJsonObject> BuildErrorFromAccessor(
		const FUeMcpAccessorErrorInfo& Info)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), UeMcp::AccessorErrorToCode(Info.Code));
		Out->SetStringField(TEXT("message"), Info.Message);
		if (Info.Detail.IsValid())
		{
			Out->SetObjectField(TEXT("detail"), Info.Detail);
		}
		return Out;
	}

	/**
	 * Pull an integer-ish field with bounds and a default. Returns false
	 * if the field was present but out of the accepted range; fills
	 * `OutError` with a human-readable reason.
	 */
	static bool GetOptionalIntInRange(
		const TSharedRef<FJsonObject>& Args,
		const TCHAR* FieldName,
		int32 Default,
		int32 Min,
		int32 Max,
		int32& OutValue,
		FString& OutError)
	{
		OutValue = Default;
		double Raw = 0.0;
		if (!Args->TryGetNumberField(FieldName, Raw))
		{
			return true;
		}
		const int32 Clamped = static_cast<int32>(Raw);
		if (Clamped < Min || Clamped > Max)
		{
			OutError = FString::Printf(
				TEXT("`%s` must be in [%d, %d] (got %d)"),
				FieldName, Min, Max, Clamped);
			return false;
		}
		OutValue = Clamped;
		return true;
	}

	/**
	 * Per-request polling state for `state.wait_until`. Held as a
	 * TSharedRef inside the step closure so repeated invocations can see
	 * the accumulated counters + last-read value without re-parsing args.
	 */
	struct FWaitUntilState
	{
		// Parsed/resolved once at factory time.
		FString ObjectRef;
		FString PropertyPath;
		EOp Op = EOp::Eq;
		TSharedPtr<FJsonValue> Expected;
		bool bHaveTolerance = false;
		double Tolerance = 0.0;
		int32 TimeoutMs = DefaultTimeoutMs;
		int32 PollMs    = DefaultPollMs;

		// The resolved target. Weak so a mid-wait GC doesn't leave us
		// holding a dangling pointer — the accessor's NullObject error
		// path then swallows + keeps polling, same as the blocking
		// implementation did when GetValue returned NullObject.
		TWeakObjectPtr<UObject> Target;

		// Running state, updated each poll.
		double StartSeconds = 0.0;
		double LastPollSeconds = -1.0; // -1 means "never polled yet"
		int32 PollCount = 0;
		bool bMatched = false;
		TSharedPtr<FJsonValue> FinalValueJson;
		FUeMcpPropertyValue LastValue;
		bool bHaveAnyRead = false;

		// Cancel reference — captured from the factory's parameter; stable
		// for the life of the request.
		FUeMcpCancelToken* Cancel = nullptr;
	};

	/**
	 * Build the "final" response payload for wait_until completion. Shared
	 * between the natural-completion, timeout-as-non-match, and cancel
	 * paths because the shape is the same in all three.
	 */
	static TSharedRef<FJsonObject> BuildWaitUntilResult(
		const FWaitUntilState& S, bool bCancelled)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		const double ElapsedMs =
			(FPlatformTime::Seconds() - S.StartSeconds) * 1000.0;

		Data->SetBoolField(TEXT("matched"), S.bMatched);
		Data->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);
		Data->SetNumberField(TEXT("poll_count"), S.PollCount);
		Data->SetStringField(TEXT("op"), OpToWire(S.Op));
		Data->SetStringField(TEXT("property_path"), S.PropertyPath);
		Data->SetStringField(TEXT("object_ref"), S.ObjectRef);
		if (bCancelled)
		{
			Data->SetBoolField(TEXT("cancelled"), true);
		}
		if (S.FinalValueJson.IsValid())
		{
			Data->SetField(TEXT("final_value"), S.FinalValueJson);
		}
		else
		{
			Data->SetField(TEXT("final_value"), MakeShared<FJsonValueNull>());
		}
		if (S.bHaveAnyRead)
		{
			Data->SetStringField(TEXT("final_type"),
				[&]() -> const TCHAR*
				{
					switch (S.LastValue.Type)
					{
						case EUeMcpValueType::Null:      return TEXT("null");
						case EUeMcpValueType::Bool:      return TEXT("bool");
						case EUeMcpValueType::Int64:     return TEXT("int64");
						case EUeMcpValueType::Double:    return TEXT("double");
						case EUeMcpValueType::String:    return TEXT("string");
						case EUeMcpValueType::Name:      return TEXT("name");
						case EUeMcpValueType::Enum:      return TEXT("enum");
						case EUeMcpValueType::ObjectRef: return TEXT("object_ref");
						case EUeMcpValueType::Array:     return TEXT("array");
						case EUeMcpValueType::Map:       return TEXT("map");
						case EUeMcpValueType::Struct:    return TEXT("struct");
					}
					return TEXT("unknown");
				}());
		}
		return Data;
	}

	/**
	 * Build a "fail immediately" step closure: returns the given payload
	 * on first invocation and signals Failed. Used when factory-time
	 * validation wants to surface an error through the pending channel
	 * rather than via a synthesized InternalError.
	 */
	static FUeMcpPendingStep MakeImmediateFailStep(TSharedRef<FJsonObject> Payload)
	{
		return [Payload](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			Out = Payload;
			return EUeMcpStep::Failed;
		};
	}

	/**
	 * `state.wait_until` factory — runs once on first service of the
	 * request. Parses args, resolves the target object, returns a step
	 * closure that the executor invokes once per tick until Done/Failed.
	 */
	static FUeMcpPendingStep BuildWaitUntilStep(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		TSharedRef<FWaitUntilState> S = MakeShared<FWaitUntilState>();
		S->Cancel = &Cancel;
		S->StartSeconds = FPlatformTime::Seconds();

		// --- World resolution ---
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage));
		}

		// --- Required string args ---
		if (!Args->TryGetStringField(TEXT("object_ref"), S->ObjectRef)
			|| S->ObjectRef.IsEmpty())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`object_ref` is required and must be a non-empty string")));
		}
		if (!Args->TryGetStringField(TEXT("property_path"), S->PropertyPath)
			|| S->PropertyPath.IsEmpty())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`property_path` is required and must be a non-empty string")));
		}
		FString OpString;
		if (!Args->TryGetStringField(TEXT("op"), OpString) || OpString.IsEmpty())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`op` is required (one of eq|neq|lt|le|gt|ge|truthy|falsy)")));
		}
		if (!ParseOp(OpString, S->Op))
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("`op` must be one of eq|neq|lt|le|gt|ge|truthy|falsy (got '%s')"),
						*OpString)));
		}

		// `value` is required for everything except truthy/falsy.
		S->Expected = Args->TryGetField(TEXT("value"));
		if (!IsUnaryOp(S->Op) && !S->Expected.IsValid())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("`value` is required for op '%s' (only omitted for truthy/falsy)"),
						*OpString)));
		}

		// Optional numeric tolerance.
		{
			double Raw = 0.0;
			if (Args->TryGetNumberField(TEXT("tolerance"), Raw))
			{
				if (Raw < 0.0)
				{
					return MakeImmediateFailStep(
						UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
							TEXT("`tolerance` must be a non-negative number")));
				}
				S->Tolerance = Raw;
				S->bHaveTolerance = true;
			}
		}

		// Timeout + poll bounds.
		{
			FString Err;
			if (!GetOptionalIntInRange(
					Args, TEXT("timeout_ms"), DefaultTimeoutMs, 0, MaxTimeoutMs,
					S->TimeoutMs, Err))
			{
				return MakeImmediateFailStep(
					UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), Err));
			}
			if (!GetOptionalIntInRange(
					Args, TEXT("poll_ms"), DefaultPollMs, MinPollMs, MaxPollMs,
					S->PollMs, Err))
			{
				return MakeImmediateFailStep(
					UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), Err));
			}
		}

		// Resolve target object once. A stale/unresolvable ref is a
		// permanent failure — no point polling against a null target.
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(S->ObjectRef, World.World);
		if (!Resolved.IsOk())
		{
			return MakeImmediateFailStep(Resolved.ErrorInfo.ToSharedRef());
		}
		S->Target = Resolved.Object;

		// Per-tick step. Captures `S` by value (shared ref) so the state
		// lives for the life of the request regardless of whether the
		// factory or any intermediate owner goes away.
		return [S](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			check(IsInGameThread());

			const double NowSeconds = FPlatformTime::Seconds();
			const double ElapsedSeconds = NowSeconds - S->StartSeconds;
			const double TimeoutSeconds = static_cast<double>(S->TimeoutMs) / 1000.0;

			// Honour `poll_ms` as a throttle. If the prior poll was less
			// than `poll_ms` ago, don't read — just hand control back to
			// the engine for another tick. The very first entry (LastPoll
			// == -1) always polls so timeout_ms=0 still yields at least
			// one read.
			const double PollIntervalSeconds =
				static_cast<double>(S->PollMs) / 1000.0;
			if (S->LastPollSeconds >= 0.0
				&& (NowSeconds - S->LastPollSeconds) < PollIntervalSeconds)
			{
				// Still need to honour timeout even while throttled.
				if (ElapsedSeconds >= TimeoutSeconds)
				{
					Out = BuildWaitUntilResult(*S, /*bCancelled*/ false);
					return EUeMcpStep::Done;
				}
				return EUeMcpStep::Continue;
			}

			S->LastPollSeconds = NowSeconds;
			++S->PollCount;

			// Poll the target. TWeakObjectPtr::Get() returns nullptr if
			// the actor was GC'd between ticks; the accessor's NullObject
			// error then falls into the "transient — keep polling" branch
			// below, matching the pre-pending behaviour.
			UObject* TargetObj = S->Target.Get();

			FUeMcpPropertyValue Value;
			FUeMcpAccessorErrorInfo ReadErr;
			const bool bOk = FUeMcpPropertyAccessor::GetValue(
				TargetObj, S->PropertyPath, Value, ReadErr);
			if (bOk)
			{
				S->bHaveAnyRead = true;
				S->LastValue = Value;
				S->FinalValueJson = Value.Json;

				bool bMatch = false;
				FString TypeErr;
				if (!EvaluateOp(S->Op, Value.Json, S->Expected,
						S->bHaveTolerance, S->Tolerance, bMatch, TypeErr))
				{
					// Type error is permanent.
					TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(
						TEXT("TYPE_MISMATCH"), TypeErr);
					TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
					Detail->SetStringField(TEXT("op"), OpToWire(S->Op));
					if (Value.Json.IsValid())
					{
						Detail->SetField(TEXT("actual"), Value.Json);
					}
					if (S->Expected.IsValid())
					{
						Detail->SetField(TEXT("expected"), S->Expected);
					}
					Err->SetObjectField(TEXT("detail"), Detail);
					Out = Err;
					return EUeMcpStep::Failed;
				}
				if (bMatch)
				{
					S->bMatched = true;
					Out = BuildWaitUntilResult(*S, /*bCancelled*/ false);
					return EUeMcpStep::Done;
				}
			}
			else
			{
				// Permanent read errors stop polling immediately. Only
				// NullObject / InternalError are treated as transient
				// (e.g. mid-PIE-teardown, mid-GC of the target actor).
				if (ReadErr.Code != EUeMcpAccessorError::NullObject
					&& ReadErr.Code != EUeMcpAccessorError::InternalError)
				{
					Out = BuildErrorFromAccessor(ReadErr);
					return EUeMcpStep::Failed;
				}
			}

			// Timeout check after the poll so timeout_ms=0 still yields at
			// least one read attempt.
			if (ElapsedSeconds >= TimeoutSeconds)
			{
				Out = BuildWaitUntilResult(*S, /*bCancelled*/ false);
				return EUeMcpStep::Done;
			}

			// Cancel: executor honours this between ticks regardless, but
			// publishing our own body-shaped response (with matched=false
			// + cancelled=true) is tidier than the executor's generic
			// CANCELLED envelope.
			if (S->Cancel != nullptr && S->Cancel->IsCancellationRequested())
			{
				Out = BuildWaitUntilResult(*S, /*bCancelled*/ true);
				return EUeMcpStep::Done;
			}

			return EUeMcpStep::Continue;
		};
	}

	// --- F2b: test.assert_property (v2 pivot Gap 2) ---
	// Coexists with the wait_until handler above. Uses its own EAssertOp
	// enum and ParseOp/OpToWire overloads — C++ overload resolution keeps
	// them from colliding with wait_until's EOp helpers.
	/** Dispatch timeout — single game-thread read + comparison, cheap. */
	static constexpr double AssertPropertyTimeoutSeconds = 10.0;

	/**
	 * The operator enum kept inside the private namespace so it doesn't
	 * pollute the translation unit's global surface for Agent 2's sibling
	 * handler (which may re-declare its own in its own block).
	 */
	enum class EAssertOp : uint8
	{
		Invalid,
		Eq,
		Neq,
		Lt,
		Le,
		Gt,
		Ge,
		Truthy,
		Falsy,
	};

	static const TCHAR* OpToWire(EAssertOp Op)
	{
		switch (Op)
		{
			case EAssertOp::Eq:     return TEXT("eq");
			case EAssertOp::Neq:    return TEXT("neq");
			case EAssertOp::Lt:     return TEXT("lt");
			case EAssertOp::Le:     return TEXT("le");
			case EAssertOp::Gt:     return TEXT("gt");
			case EAssertOp::Ge:     return TEXT("ge");
			case EAssertOp::Truthy: return TEXT("truthy");
			case EAssertOp::Falsy:  return TEXT("falsy");
			default:                return TEXT("invalid");
		}
	}

	static EAssertOp ParseOp(const FString& Token)
	{
		if (Token.Equals(TEXT("eq"),     ESearchCase::IgnoreCase)) return EAssertOp::Eq;
		if (Token.Equals(TEXT("neq"),    ESearchCase::IgnoreCase)) return EAssertOp::Neq;
		if (Token.Equals(TEXT("lt"),     ESearchCase::IgnoreCase)) return EAssertOp::Lt;
		if (Token.Equals(TEXT("le"),     ESearchCase::IgnoreCase)) return EAssertOp::Le;
		if (Token.Equals(TEXT("gt"),     ESearchCase::IgnoreCase)) return EAssertOp::Gt;
		if (Token.Equals(TEXT("ge"),     ESearchCase::IgnoreCase)) return EAssertOp::Ge;
		if (Token.Equals(TEXT("truthy"), ESearchCase::IgnoreCase)) return EAssertOp::Truthy;
		if (Token.Equals(TEXT("falsy"),  ESearchCase::IgnoreCase)) return EAssertOp::Falsy;
		return EAssertOp::Invalid;
	}

	static bool IsNumericOp(EAssertOp Op)
	{
		return Op == EAssertOp::Lt || Op == EAssertOp::Le
			|| Op == EAssertOp::Gt || Op == EAssertOp::Ge;
	}

	static bool RequiresValue(EAssertOp Op)
	{
		return Op != EAssertOp::Truthy && Op != EAssertOp::Falsy;
	}

	/**
	 * Best-effort conversion of a JSON value to a double. Accepts
	 * FJsonValueNumber and numeric strings. Non-numeric FJsonValues
	 * (bool, string-but-not-numeric, arrays, objects) return false so
	 * the caller can surface a TYPE_MISMATCH.
	 */
	static bool JsonAsDouble(const TSharedPtr<FJsonValue>& V, double& Out)
	{
		if (!V.IsValid()) return false;
		if (V->Type == EJson::Number)
		{
			Out = V->AsNumber();
			return true;
		}
		if (V->Type == EJson::String)
		{
			const FString S = V->AsString();
			if (S.IsNumeric())
			{
				Out = FCString::Atod(*S);
				return true;
			}
		}
		return false;
	}

	/** Truthiness per JS-ish rules used across the test surface. */
	static bool JsonIsTruthy(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) return false;
		switch (V->Type)
		{
			case EJson::Null:   return false;
			case EJson::Boolean: return V->AsBool();
			case EJson::Number:  return V->AsNumber() != 0.0;
			case EJson::String:  return !V->AsString().IsEmpty();
			case EJson::Array:   return V->AsArray().Num() > 0;
			case EJson::Object:  return V->AsObject().IsValid() && V->AsObject()->Values.Num() > 0;
			default:             return false;
		}
	}

	/**
	 * Deep equality between two FJsonValue trees. Mirrors the behaviour a
	 * caller Python layer would get from json.loads equality — type-strict
	 * for primitives (a string "3" does NOT equal a number 3), order-
	 * sensitive for arrays, key-set-and-value-equal for objects.
	 */
	static bool JsonEquals(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		if (!A.IsValid() && !B.IsValid()) return true;
		if (!A.IsValid() || !B.IsValid()) return false;
		if (A->Type != B->Type) return false;
		switch (A->Type)
		{
			case EJson::Null:    return true;
			case EJson::Boolean: return A->AsBool() == B->AsBool();
			case EJson::Number:  return A->AsNumber() == B->AsNumber();
			case EJson::String:  return A->AsString().Equals(B->AsString());
			case EJson::Array:
			{
				const TArray<TSharedPtr<FJsonValue>>& AA = A->AsArray();
				const TArray<TSharedPtr<FJsonValue>>& BB = B->AsArray();
				if (AA.Num() != BB.Num()) return false;
				for (int32 i = 0; i < AA.Num(); ++i)
				{
					if (!JsonEquals(AA[i], BB[i])) return false;
				}
				return true;
			}
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> AO = A->AsObject();
				const TSharedPtr<FJsonObject> BO = B->AsObject();
				if (!AO.IsValid() || !BO.IsValid()) return AO == BO;
				if (AO->Values.Num() != BO->Values.Num()) return false;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : AO->Values)
				{
					const TSharedPtr<FJsonValue>* Other = BO->Values.Find(Pair.Key);
					if (Other == nullptr) return false;
					if (!JsonEquals(Pair.Value, *Other)) return false;
				}
				return true;
			}
			default: return false;
		}
	}

	/** One-line text form for diagnostics (short, not pretty-print). */
	static FString JsonToShort(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) return TEXT("null");
		switch (V->Type)
		{
			case EJson::Null:    return TEXT("null");
			case EJson::Boolean: return V->AsBool() ? TEXT("true") : TEXT("false");
			case EJson::Number:
			{
				const double N = V->AsNumber();
				if (FMath::IsFinite(N) && FMath::Fractional(N) == 0.0
					&& FMath::Abs(N) < 1e15)
				{
					return FString::Printf(TEXT("%lld"), static_cast<int64>(N));
				}
				return FString::Printf(TEXT("%g"), N);
			}
			case EJson::String:  return FString::Printf(TEXT("\"%s\""), *V->AsString());
			case EJson::Array:   return FString::Printf(TEXT("[<%d>]"), V->AsArray().Num());
			case EJson::Object:
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				return FString::Printf(TEXT("{<%d>}"), O.IsValid() ? O->Values.Num() : 0);
			}
			default: return TEXT("?");
		}
	}

	/** Locate an `AFunctionalTest` by actor label or name in `World`. */
	static AFunctionalTest* FindFunctionalTestByLabel(UWorld* World, const FString& Label)
	{
		if (World == nullptr || Label.IsEmpty()) return nullptr;
		for (TActorIterator<AFunctionalTest> It(World); It; ++It)
		{
			AFunctionalTest* Ft = *It;
			if (Ft == nullptr) continue;
			if (Ft->GetActorLabel().Equals(Label, ESearchCase::IgnoreCase)
				|| Ft->GetName().Equals(Label, ESearchCase::IgnoreCase))
			{
				return Ft;
			}
		}
		return nullptr;
	}

	/**
	 * Run the comparison. `bWithinTolerance` is set when a numeric
	 * comparison's passed-margin is <= tolerance (for reporting); it is
	 * only meaningful when Tolerance > 0 and both sides are numeric.
	 */
	static bool Compare(
		EAssertOp Op,
		const TSharedPtr<FJsonValue>& Actual,
		const TSharedPtr<FJsonValue>& Expected,
		double Tolerance,
		bool& bOutWithinTolerance,
		double& bOutDelta,
		bool& bOutDeltaValid)
	{
		bOutWithinTolerance = false;
		bOutDelta = 0.0;
		bOutDeltaValid = false;

		if (Op == EAssertOp::Truthy) return JsonIsTruthy(Actual);
		if (Op == EAssertOp::Falsy)  return !JsonIsTruthy(Actual);

		if (Op == EAssertOp::Eq || Op == EAssertOp::Neq)
		{
			// Numeric tolerance path: if both sides parse as doubles and
			// Tolerance > 0, compare within tolerance rather than exact.
			double A = 0.0, E = 0.0;
			if (Tolerance > 0.0
				&& JsonAsDouble(Actual, A)
				&& JsonAsDouble(Expected, E))
			{
				const double D = FMath::Abs(A - E);
				bOutDelta = A - E;
				bOutDeltaValid = true;
				const bool bEqual = D <= Tolerance;
				bOutWithinTolerance = bEqual;
				return Op == EAssertOp::Eq ? bEqual : !bEqual;
			}
			// Exact structural equality fallback.
			const bool bEqual = JsonEquals(Actual, Expected);
			return Op == EAssertOp::Eq ? bEqual : !bEqual;
		}

		// Numeric ordering operators — require both sides numeric.
		double A = 0.0, E = 0.0;
		if (!JsonAsDouble(Actual, A) || !JsonAsDouble(Expected, E))
		{
			return false; // TYPE_MISMATCH territory — handled by caller.
		}
		bOutDelta = A - E;
		bOutDeltaValid = true;
		switch (Op)
		{
			case EAssertOp::Lt: return A <  E;
			case EAssertOp::Le: return A <= E;
			case EAssertOp::Gt: return A >  E;
			case EAssertOp::Ge: return A >= E;
			default:            return false;
		}
	}

	/** `test.assert_property` body. */
	static TSharedRef<FJsonObject> HandleTestAssertProperty(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		// 1) Validate + parse args.
		FString ObjectId;
		if (!Args->TryGetStringField(TEXT("object"), ObjectId) || ObjectId.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`object` is required and must be a non-empty string"));
		}
		FString Path;
		if (!Args->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`path` is required and must be a non-empty string"));
		}
		FString OpToken;
		if (!Args->TryGetStringField(TEXT("op"), OpToken) || OpToken.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`op` is required — one of eq|neq|lt|le|gt|ge|truthy|falsy"));
		}
		const EAssertOp Op = ParseOp(OpToken);
		if (Op == EAssertOp::Invalid)
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`op` must be eq|neq|lt|le|gt|ge|truthy|falsy; got '%s'"),
					*OpToken));
		}

		// `value` is required for all ops except truthy/falsy. May be any
		// JSON type; we pull it out as an FJsonValue (preserves shape).
		TSharedPtr<FJsonValue> Expected = Args->TryGetField(TEXT("value"));
		if (RequiresValue(Op) && !Expected.IsValid())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`value` is required for op='%s'"),
					*OpToken));
		}

		double Tolerance = 0.0;
		(void)Args->TryGetNumberField(TEXT("tolerance"), Tolerance);
		if (Tolerance < 0.0)
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`tolerance` must be >= 0; got %g"), Tolerance));
		}

		FString Message;
		Args->TryGetStringField(TEXT("message"), Message);

		FString FunctionalTestLabel;
		Args->TryGetStringField(TEXT("functional_test"), FunctionalTestLabel);

		// 2) Resolve world + object via shared infra.
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ObjectId, World.World);
		if (!Resolved.IsOk())
		{
			return Resolved.ErrorInfo.ToSharedRef();
		}

		// 3) Read the property.
		FUeMcpPropertyValue Value;
		FUeMcpAccessorErrorInfo AccErr;
		if (!FUeMcpPropertyAccessor::GetValue(
				Resolved.Object, Path, Value, AccErr))
		{
			// Surface the accessor's error verbatim; it's already one of
			// the documented codes. Mapping goes through the shared
			// `UeMcp::AccessorErrorToCode` (issue #62); no local switch.
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), UeMcp::AccessorErrorToCode(AccErr.Code));
			Out->SetStringField(TEXT("message"), AccErr.Message);
			if (AccErr.Detail.IsValid())
			{
				Out->SetObjectField(TEXT("detail"), AccErr.Detail);
			}
			return Out;
		}

		// 4) Compare.
		bool bWithinTolerance = false;
		double Delta = 0.0;
		bool bDeltaValid = false;
		const TSharedPtr<FJsonValue>& ActualJson = Value.Json;

		// Ordering ops require numeric actual; fail early with TYPE_MISMATCH
		// rather than silently returning passed=false so the caller's test
		// distinguishes "assertion is ill-formed" from "state didn't match".
		if (IsNumericOp(Op))
		{
			double AProbe = 0.0, EProbe = 0.0;
			if (!JsonAsDouble(ActualJson, AProbe))
			{
				return UeMcp::MakeInlineError(
					TEXT("TYPE_MISMATCH"),
					FString::Printf(
						TEXT("op='%s' requires numeric property; actual value type is non-numeric"),
						*OpToken));
			}
			if (!JsonAsDouble(Expected, EProbe))
			{
				return UeMcp::MakeInlineError(
					TEXT("TYPE_MISMATCH"),
					FString::Printf(
						TEXT("op='%s' requires numeric `value`; got a non-numeric JSON value"),
						*OpToken));
			}
		}

		const bool bPassed = Compare(
			Op, ActualJson, Expected, Tolerance,
			bWithinTolerance, Delta, bDeltaValid);

		// 5) Build diff_summary.
		FString DiffSummary;
		const FString ActualShort   = JsonToShort(ActualJson);
		const FString ExpectedShort = RequiresValue(Op) ? JsonToShort(Expected) : TEXT("-");
		if (bPassed)
		{
			if (RequiresValue(Op))
			{
				DiffSummary = FString::Printf(
					TEXT("actual=%s matches expected=%s (op=%s)"),
					*ActualShort, *ExpectedShort, OpToWire(Op));
			}
			else
			{
				DiffSummary = FString::Printf(
					TEXT("actual=%s is %s"),
					*ActualShort, OpToWire(Op));
			}
			if (Tolerance > 0.0 && bDeltaValid)
			{
				DiffSummary += FString::Printf(
					TEXT(" tolerance=%g delta=%g"), Tolerance, Delta);
			}
		}
		else
		{
			if (RequiresValue(Op))
			{
				DiffSummary = FString::Printf(
					TEXT("actual=%s expected=%s op=%s"),
					*ActualShort, *ExpectedShort, OpToWire(Op));
			}
			else
			{
				DiffSummary = FString::Printf(
					TEXT("actual=%s expected %s"),
					*ActualShort, OpToWire(Op));
			}
			if (bDeltaValid)
			{
				DiffSummary += FString::Printf(TEXT(" delta=%g"), Delta);
			}
			if (Tolerance > 0.0)
			{
				DiffSummary += FString::Printf(TEXT(" tolerance=%g"), Tolerance);
			}
			if (!Message.IsEmpty())
			{
				DiffSummary += FString::Printf(TEXT(" message=\"%s\""), *Message);
			}
		}

		// 6) FunctionalTest routing (optional).
		bool bAttached = false;
		if (!FunctionalTestLabel.IsEmpty())
		{
			AFunctionalTest* Ft = FindFunctionalTestByLabel(
				World.World, FunctionalTestLabel);
			if (Ft == nullptr)
			{
				return UeMcp::MakeInlineError(
					TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("AFunctionalTest with label '%s' not found in resolved world"),
						*FunctionalTestLabel));
			}
			const FString FullMsg = Message.IsEmpty()
				? DiffSummary
				: FString::Printf(TEXT("%s — %s"), *Message, *DiffSummary);
			if (bPassed)
			{
				Ft->AddInfo(FullMsg);
			}
			else
			{
				Ft->AddError(FullMsg);
			}
			bAttached = true;
		}

		// 7) Log category LogUeMcpTest — structured trace for `log.tail` users.
		if (bPassed)
		{
			UE_LOG(LogUeMcpTest, Log,
				TEXT("assert_property PASS: object=%s path=%s op=%s %s"),
				*ObjectId, *Path, OpToWire(Op), *DiffSummary);
		}
		else
		{
			UE_LOG(LogUeMcpTest, Error,
				TEXT("assert_property FAIL: object=%s path=%s op=%s %s"),
				*ObjectId, *Path, OpToWire(Op), *DiffSummary);
		}

		// 8) Build response.
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("passed"), bPassed);
		if (ActualJson.IsValid())
		{
			Data->SetField(TEXT("actual"), ActualJson);
		}
		else
		{
			Data->SetField(TEXT("actual"), MakeShared<FJsonValueNull>());
		}
		if (RequiresValue(Op) && Expected.IsValid())
		{
			Data->SetField(TEXT("expected"), Expected);
		}
		else
		{
			Data->SetField(TEXT("expected"), MakeShared<FJsonValueNull>());
		}
		Data->SetStringField(TEXT("op"), OpToWire(Op));
		Data->SetStringField(TEXT("diff_summary"), DiffSummary);
		Data->SetBoolField(TEXT("attached_to_functional_test"), bAttached);
		if (Tolerance > 0.0)
		{
			Data->SetNumberField(TEXT("tolerance"), Tolerance);
		}
		if (!Message.IsEmpty())
		{
			Data->SetStringField(TEXT("message"), Message);
		}
		Data->SetStringField(TEXT("path"), Path);
		Data->SetStringField(TEXT("object"), ObjectId);
		return Data;
	}
}

namespace UeMcpAssertionHandlersPrivate
{
	// -----------------------------------------------------------------------
	// `state.wait_until_actor_count` — sibling to `state.wait_until` for the
	// "wait until N actors of class X exist in world Y" predicate. Issue #26
	// (hard-sleep waits in fixture/battle tests). Today battle scripts pad
	// 5-6 wall-seconds before reading actor.list to count surviving targets;
	// this is the bounded-wait replacement.
	//
	// Wire shape:
	//   {
	//     "class_filter": "BP_Target_C" | "/Game/.../BP_Target.BP_Target_C",
	//     "op": "le" | "lt" | "ge" | "gt" | "eq" | "neq",
	//     "target": 1,
	//     "name_filter": "...",   // optional substring, mirrors actor.list
	//     "timeout_ms": 8000,
	//     "poll_ms": 50,
	//     "world": "auto"|"pie"|"editor"
	//   }
	//
	// Response:
	//   {
	//     "matched": bool, "elapsed_ms": double, "poll_count": int,
	//     "op": str, "target": int, "final_count": int,
	//     "class_filter": str, "name_filter"?: str,
	//     "resolved_scope": "pie"|"editor", "cancelled"?: bool
	//   }
	//
	// Truthy/falsy are rejected — they don't make sense for a numeric count.
	// Class match mirrors actor.list (case-insensitive short-name OR
	// case-insensitive class path). Optional name_filter does substring
	// match on actor labels.
	//
	// Hard ceilings reuse the wait_until clamps (MaxTimeoutMs = 2 min,
	// MaxPollMs = 5 s). Per-tick read cost is one `TActorIterator` walk
	// over the world's actors — cheap; the gameplay-loop work that's
	// happening alongside is the dominant cost.
	struct FWaitUntilActorCountState
	{
		FString ClassFilterRaw;
		FString ClassFilterLower;
		FString NameFilterRaw;
		FString NameFilterLower;
		EOp Op = EOp::Eq;
		int32 Target = 0;
		int32 TimeoutMs = DefaultTimeoutMs;
		int32 PollMs    = DefaultPollMs;

		// Weak so a mid-wait world tear-down (e.g. PIE stop) ends the
		// poll cleanly with matched=false rather than crashing.
		TWeakObjectPtr<UWorld> World;
		FString ResolvedScope;

		double StartSeconds = 0.0;
		double LastPollSeconds = -1.0;
		int32 PollCount = 0;
		int32 LastCount = 0;
		bool bMatched = false;
		bool bHaveAnyRead = false;

		FUeMcpCancelToken* Cancel = nullptr;
	};

	/** Build the response payload — shared for natural-completion, timeout,
	 *  and cancel paths so the wire shape is identical regardless. */
	static TSharedRef<FJsonObject> BuildActorCountResult(
		const FWaitUntilActorCountState& S, bool bCancelled)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		const double ElapsedMs =
			(FPlatformTime::Seconds() - S.StartSeconds) * 1000.0;

		Data->SetBoolField(TEXT("matched"), S.bMatched);
		Data->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);
		Data->SetNumberField(TEXT("poll_count"), S.PollCount);
		Data->SetStringField(TEXT("op"), OpToWire(S.Op));
		Data->SetNumberField(TEXT("target"), S.Target);
		Data->SetNumberField(TEXT("final_count"),
			S.bHaveAnyRead ? S.LastCount : 0);
		Data->SetStringField(TEXT("class_filter"), S.ClassFilterRaw);
		if (!S.NameFilterRaw.IsEmpty())
		{
			Data->SetStringField(TEXT("name_filter"), S.NameFilterRaw);
		}
		if (!S.ResolvedScope.IsEmpty())
		{
			Data->SetStringField(TEXT("resolved_scope"), S.ResolvedScope);
		}
		if (bCancelled)
		{
			Data->SetBoolField(TEXT("cancelled"), true);
		}
		return Data;
	}

	/** Single-pass actor count under the resolved class + name filters. */
	static int32 CountMatchingActors(
		UWorld* World,
		const FString& LowerClassFilter,
		const FString& LowerNameFilter)
	{
		if (World == nullptr || LowerClassFilter.IsEmpty())
		{
			return 0;
		}
		int32 Count = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (Actor == nullptr) { continue; }

			UClass* Cls = Actor->GetClass();
			if (Cls == nullptr) { continue; }

			const bool bShortMatch = Cls->GetName().ToLower() == LowerClassFilter;
			const bool bPathMatch  = Cls->GetPathName().ToLower() == LowerClassFilter;
			if (!bShortMatch && !bPathMatch)
			{
				continue;
			}

			if (!LowerNameFilter.IsEmpty())
			{
				if (!Actor->GetActorLabel().ToLower().Contains(LowerNameFilter))
				{
					continue;
				}
			}

			++Count;
		}
		return Count;
	}

	/** Compare an integer count against a target via one of the documented
	 *  ordering / equality ops. Truthy/Falsy are rejected at parse time. */
	static bool CompareCount(EOp Op, int32 Count, int32 Target)
	{
		switch (Op)
		{
			case EOp::Eq:  return Count == Target;
			case EOp::Neq: return Count != Target;
			case EOp::Lt:  return Count <  Target;
			case EOp::Le:  return Count <= Target;
			case EOp::Gt:  return Count >  Target;
			case EOp::Ge:  return Count >= Target;
			default:       return false; // Truthy/Falsy rejected at parse.
		}
	}

	/**
	 * `state.wait_until_actor_count` factory — runs once on first service.
	 * Validates args, resolves the world, returns a step closure that
	 * counts on each tick until the comparison holds or the timeout elapses.
	 */
	static FUeMcpPendingStep BuildWaitUntilActorCountStep(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		TSharedRef<FWaitUntilActorCountState> S =
			MakeShared<FWaitUntilActorCountState>();
		S->Cancel = &Cancel;
		S->StartSeconds = FPlatformTime::Seconds();

		// World resolution shares the world-resolver helpers with every
		// other world-aware tool. Failure is permanent — no point polling
		// against a missing world.
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage));
		}
		S->World = World.World;
		S->ResolvedScope = UeMcp::WorldScopeToString(World.ResolvedScope);

		// class_filter is required: counting "all actors in the world"
		// would dominate the bounded-wait usefulness with engine-spawned
		// helpers (controllers, GameMode, post-process volumes, …).
		if (!Args->TryGetStringField(TEXT("class_filter"), S->ClassFilterRaw)
			|| S->ClassFilterRaw.IsEmpty())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`class_filter` is required and must be a non-empty string")));
		}
		S->ClassFilterLower = S->ClassFilterRaw.ToLower();

		// name_filter is optional substring on labels, mirroring actor.list.
		Args->TryGetStringField(TEXT("name_filter"), S->NameFilterRaw);
		S->NameFilterLower = S->NameFilterRaw.ToLower();

		// op: numeric subset. truthy/falsy rejected (a count is always a
		// number; truthy/falsy is misleading on the wire and would
		// double-document the `eq 0` / `neq 0` semantics).
		FString OpString;
		if (!Args->TryGetStringField(TEXT("op"), OpString) || OpString.IsEmpty())
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`op` is required (one of eq|neq|lt|le|gt|ge)")));
		}
		if (!ParseOp(OpString, S->Op))
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("`op` must be one of eq|neq|lt|le|gt|ge (got '%s')"),
						*OpString)));
		}
		if (IsUnaryOp(S->Op))
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`op` truthy/falsy not supported for actor counts; use eq 0 / neq 0")));
		}

		// target: required, non-negative integer. Negative counts can't
		// happen, so a negative target means the user is confused.
		double TargetRaw = 0.0;
		if (!Args->TryGetNumberField(TEXT("target"), TargetRaw))
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					TEXT("`target` is required and must be a non-negative integer")));
		}
		const int32 TargetInt = static_cast<int32>(TargetRaw);
		if (TargetInt < 0)
		{
			return MakeImmediateFailStep(
				UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					FString::Printf(TEXT("`target` must be >= 0 (got %d)"), TargetInt)));
		}
		S->Target = TargetInt;

		// Timeout / poll bounds — reuse wait_until's clamps.
		{
			FString Err;
			if (!GetOptionalIntInRange(
					Args, TEXT("timeout_ms"), DefaultTimeoutMs, 0, MaxTimeoutMs,
					S->TimeoutMs, Err))
			{
				return MakeImmediateFailStep(
					UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), Err));
			}
			if (!GetOptionalIntInRange(
					Args, TEXT("poll_ms"), DefaultPollMs, MinPollMs, MaxPollMs,
					S->PollMs, Err))
			{
				return MakeImmediateFailStep(
					UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"), Err));
			}
		}

		// Per-tick step. Captures the shared state by ref so the lifetime
		// outlives the factory. Mirrors `BuildWaitUntilStep` pattern.
		return [S](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
		{
			check(IsInGameThread());

			const double NowSeconds = FPlatformTime::Seconds();
			const double ElapsedSeconds = NowSeconds - S->StartSeconds;
			const double TimeoutSeconds = static_cast<double>(S->TimeoutMs) / 1000.0;

			const double PollIntervalSeconds =
				static_cast<double>(S->PollMs) / 1000.0;
			if (S->LastPollSeconds >= 0.0
				&& (NowSeconds - S->LastPollSeconds) < PollIntervalSeconds)
			{
				if (ElapsedSeconds >= TimeoutSeconds)
				{
					Out = BuildActorCountResult(*S, /*bCancelled*/ false);
					return EUeMcpStep::Done;
				}
				return EUeMcpStep::Continue;
			}

			S->LastPollSeconds = NowSeconds;
			++S->PollCount;

			// Tear-down resilience: if the world disappeared mid-wait
			// (e.g. PIE stopped), end with the last seen count and
			// matched=false. Same pattern as wait_until's NullObject branch.
			UWorld* WorldPtr = S->World.Get();
			if (WorldPtr == nullptr)
			{
				Out = BuildActorCountResult(*S, /*bCancelled*/ false);
				return EUeMcpStep::Done;
			}

			S->LastCount = CountMatchingActors(
				WorldPtr, S->ClassFilterLower, S->NameFilterLower);
			S->bHaveAnyRead = true;

			if (CompareCount(S->Op, S->LastCount, S->Target))
			{
				S->bMatched = true;
				Out = BuildActorCountResult(*S, /*bCancelled*/ false);
				return EUeMcpStep::Done;
			}

			if (ElapsedSeconds >= TimeoutSeconds)
			{
				Out = BuildActorCountResult(*S, /*bCancelled*/ false);
				return EUeMcpStep::Done;
			}

			if (S->Cancel != nullptr && S->Cancel->IsCancellationRequested())
			{
				Out = BuildActorCountResult(*S, /*bCancelled*/ true);
				return EUeMcpStep::Done;
			}

			return EUeMcpStep::Continue;
		};
	}
}

void UeMcp::RegisterAssertionHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpAssertionHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("state.wait_until"));
		// Dispatcher timeout must be >= caller-supplied `timeout_ms` +
		// slack. The handler's own clamp is 120s; give the executor 150s
		// so the step's internal timeout always wins the race.
		Reg.DefaultTimeoutSeconds = 150.0;
		Reg.bMutating = false;
		Reg.PendingHandler = FUeMcpToolPendingHandler::CreateStatic(&BuildWaitUntilStep);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("state.wait_until_actor_count"));
		// Same 150s ceiling as wait_until — handler clamps caller's
		// timeout_ms at 120s, executor gets 30s slack.
		Reg.DefaultTimeoutSeconds = 150.0;
		Reg.bMutating = false;
		Reg.PendingHandler = FUeMcpToolPendingHandler::CreateStatic(&BuildWaitUntilActorCountStep);
		Dispatcher.RegisterTool(Reg);
	}

	// F2b — test.assert_property: single game-thread read + comparison,
	// cheap. AssertPropertyTimeoutSeconds (10s) is defined in the anon
	// namespace above.
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("test.assert_property"));
		Reg.DefaultTimeoutSeconds = AssertPropertyTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleTestAssertProperty);
		Dispatcher.RegisterTool(Reg);
	}
}
