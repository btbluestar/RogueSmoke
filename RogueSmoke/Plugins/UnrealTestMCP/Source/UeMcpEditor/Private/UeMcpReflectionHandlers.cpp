// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpReflectionHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpPropertyAccessor.h"
#include "UeMcpPropertyPath.h"
#include "UeMcpPropertyValue.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpReflectionHandlersPrivate
{
	/** Default dispatcher timeouts. Reads are cheap; writes may trigger
	 *  notify chains; list walks are bounded by the max-depth setting. */
	static constexpr double GetDefaultTimeoutSeconds     = 10.0;
	static constexpr double SetDefaultTimeoutSeconds     = 30.0;
	static constexpr double ListPathsTimeoutSeconds      = 15.0;

	/** Bounds for caller-supplied `max_depth`. Accessor's own default is 10. */
	static constexpr int32 MaxDepthLimit = 20;

	/** Wire-string for `EUeMcpValueType`. Kept stable — callers pattern
	 *  match on these tokens. */
	static const TCHAR* ValueTypeToWire(EUeMcpValueType Type)
	{
		switch (Type)
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
	}

	/**
	 * Build the inline `{error, message, detail}` root from a populated
	 * `FUeMcpAccessorErrorInfo`. The accessor attaches structured detail
	 * (actual_properties, actual_size, actual_keys, expected_type, etc.)
	 * via its `Detail` TSharedPtr; we pass it through verbatim.
	 *
	 * Accessor-error -> wire-code goes through the shared
	 * `UeMcp::AccessorErrorToCode` (issue #62); no local switch here.
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
	 * Parse the mandatory `object` + `path` args. `max_depth` and
	 * `include_synthesized` are passed through to `OutReadOptions` /
	 * `OutListOptions` when the caller supplies pointers to them.
	 *
	 * Returns nullptr on success; otherwise a populated inline-error root
	 * the handler can return directly.
	 */
	static TSharedPtr<FJsonObject> ParseCommonArgs(
		const TSharedRef<FJsonObject>& Args,
		FString& OutObjectId,
		FString* OutPath)
	{
		if (!Args->TryGetStringField(TEXT("object"), OutObjectId)
			|| OutObjectId.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`object` is required and must be a non-empty string"));
		}

		if (OutPath != nullptr)
		{
			if (!Args->TryGetStringField(TEXT("path"), *OutPath)
				|| OutPath->IsEmpty())
			{
				return UeMcp::MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					TEXT("`path` is required and must be a non-empty string"));
			}
		}

		return nullptr;
	}

	/** `get_property` body. */
	static TSharedRef<FJsonObject> HandleGetProperty(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		// World resolution first so we can search the right actor set.
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		FString ObjectId, Path;
		if (TSharedPtr<FJsonObject> Err = ParseCommonArgs(Args, ObjectId, &Path))
		{
			return Err.ToSharedRef();
		}

		// Object resolution via the shared five-strategy chain.
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ObjectId, World.World);
		if (!Resolved.IsOk())
		{
			return Resolved.ErrorInfo.ToSharedRef();
		}

		FUeMcpPropertyValue Value;
		FUeMcpAccessorErrorInfo Err;
		if (!FUeMcpPropertyAccessor::GetValue(
				Resolved.Object, Path, Value, Err))
		{
			return BuildErrorFromAccessor(Err);
		}

		// Success. `data` holds the JSON value directly (agents consume
		// it verbatim). Top-level `type` is the semantic tag from the
		// carrier — callers can use it to disambiguate (e.g. enum vs int).
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		if (Value.Json.IsValid())
		{
			Data->SetField(TEXT("data"), Value.Json);
		}
		else
		{
			Data->SetField(TEXT("data"), MakeShared<FJsonValueNull>());
		}
		Data->SetStringField(TEXT("type"), ValueTypeToWire(Value.Type));
		Data->SetStringField(TEXT("path"), Path);
		Data->SetStringField(TEXT("object"), ObjectId);

		if (Value.Type == EUeMcpValueType::Enum)
		{
			Data->SetNumberField(TEXT("enum_numeric"),
				static_cast<double>(Value.EnumNumeric));
		}
		if (Value.bTruncated)
		{
			Data->SetBoolField(TEXT("truncated"), true);
		}
		if (Resolved.bLoaded)
		{
			Data->SetBoolField(TEXT("asset_loaded_on_demand"), true);
		}
		return Data;
	}

	/** `set_property` body. */
	static TSharedRef<FJsonObject> HandleSetProperty(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		FString ObjectId, Path;
		if (TSharedPtr<FJsonObject> Err = ParseCommonArgs(Args, ObjectId, &Path))
		{
			return Err.ToSharedRef();
		}

		// `value` is required but may be any JSON type — null, bool,
		// number, string, array, object. We fish it out via TryGetField
		// because FJsonObject has no "get any field" primitive that
		// preserves the underlying FJsonValue shape cleanly.
		const TSharedPtr<FJsonValue> RawValue = Args->TryGetField(TEXT("value"));
		if (!RawValue.IsValid())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`value` is required (may be null, bool, number, string, array, or object)"));
		}

		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ObjectId, World.World);
		if (!Resolved.IsOk())
		{
			return Resolved.ErrorInfo.ToSharedRef();
		}

		// Best-effort pre-state read for the rollback hint. If the read
		// fails for any reason (property deep-reads a transient struct,
		// the value is write-only, etc.) we simply omit `previous_value`.
		// The write still proceeds; only the rollback payload shrinks.
		FUeMcpPropertyValue PreValue;
		FUeMcpAccessorErrorInfo PreErr;
		const bool bHavePreState = FUeMcpPropertyAccessor::GetValue(
			Resolved.Object, Path, PreValue, PreErr);

		FUeMcpAccessorErrorInfo WriteErr;
		if (!FUeMcpPropertyAccessor::SetValue(
				Resolved.Object, Path, RawValue, WriteErr))
		{
			return BuildErrorFromAccessor(WriteErr);
		}

		// Readback to confirm the write and report the resulting type.
		// Intentionally best-effort; a successful write with a failing
		// readback is possible (e.g. a deep-struct that reports truncated)
		// and should not be reported as a write failure.
		FUeMcpPropertyValue PostValue;
		FUeMcpAccessorErrorInfo PostErr;
		const bool bHavePostState = FUeMcpPropertyAccessor::GetValue(
			Resolved.Object, Path, PostValue, PostErr);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("set"), true);
		Data->SetStringField(TEXT("path"), Path);
		Data->SetStringField(TEXT("object"), ObjectId);
		if (bHavePostState)
		{
			Data->SetStringField(TEXT("type"), ValueTypeToWire(PostValue.Type));
		}
		if (bHavePreState && PreValue.Json.IsValid())
		{
			Data->SetField(TEXT("previous_value"), PreValue.Json);

			// Rollback hint — only emit when we captured pre-state, so
			// the replay is precise. Omit the `world` arg because world
			// at replay time may differ; the resolver honours auto anyway.
			TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
			Rollback->SetStringField(TEXT("tool"), TEXT("set_property"));
			TSharedRef<FJsonObject> RollbackArgs = MakeShared<FJsonObject>();
			RollbackArgs->SetStringField(TEXT("object"), ObjectId);
			RollbackArgs->SetStringField(TEXT("path"), Path);
			RollbackArgs->SetField(TEXT("value"), PreValue.Json);
			Rollback->SetObjectField(TEXT("args"), RollbackArgs);
			Data->SetObjectField(TEXT("rollback"), Rollback);
		}
		if (Resolved.bLoaded)
		{
			Data->SetBoolField(TEXT("asset_loaded_on_demand"), true);
		}
		return Data;
	}

	/** `list_property_paths` body. */
	static TSharedRef<FJsonObject> HandleListPropertyPaths(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		FString ObjectId;
		if (TSharedPtr<FJsonObject> Err = ParseCommonArgs(Args, ObjectId, nullptr))
		{
			return Err.ToSharedRef();
		}

		FUeMcpListPathsOptions Options;
		{
			int32 Depth = 0;
			if (Args->TryGetNumberField(TEXT("max_depth"), Depth) && Depth > 0)
			{
				Options.MaxDepth = FMath::Min(Depth, MaxDepthLimit);
			}
			bool IncludeSynth = true;
			if (Args->TryGetBoolField(TEXT("include_synthesized"), IncludeSynth))
			{
				Options.bIncludeSynthesized = IncludeSynth;
			}
		}

		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ObjectId, World.World);
		if (!Resolved.IsOk())
		{
			return Resolved.ErrorInfo.ToSharedRef();
		}

		TArray<FString> Paths;
		FUeMcpAccessorErrorInfo Err;
		if (!FUeMcpPropertyAccessor::ListPropertyPaths(
				Resolved.Object, Paths, Err, Options))
		{
			return BuildErrorFromAccessor(Err);
		}

		TArray<TSharedPtr<FJsonValue>> PathValues;
		PathValues.Reserve(Paths.Num());
		for (const FString& P : Paths)
		{
			PathValues.Add(MakeShared<FJsonValueString>(P));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("paths"), PathValues);
		Data->SetNumberField(TEXT("count"), Paths.Num());
		Data->SetNumberField(TEXT("max_depth"), Options.MaxDepth);
		Data->SetStringField(TEXT("object"), ObjectId);

		// The accessor advertises truncation through its success-ish
		// `DepthExceeded` code path (see the enum doc). If the accessor
		// fills `Err.Detail` with a `truncated: true` hint, forward it;
		// otherwise the caller assumes a complete enumeration.
		if (Err.Detail.IsValid())
		{
			bool bTrunc = false;
			if (Err.Detail->TryGetBoolField(TEXT("truncated"), bTrunc) && bTrunc)
			{
				Data->SetBoolField(TEXT("truncated"), true);
			}
		}
		return Data;
	}
}

void UeMcp::RegisterReflectionHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpReflectionHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("get_property"));
		Reg.DefaultTimeoutSeconds = GetDefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleGetProperty);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("set_property"));
		Reg.DefaultTimeoutSeconds = SetDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSetProperty);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("list_property_paths"));
		Reg.DefaultTimeoutSeconds = ListPathsTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleListPropertyPaths);
		Dispatcher.RegisterTool(Reg);
	}
}
