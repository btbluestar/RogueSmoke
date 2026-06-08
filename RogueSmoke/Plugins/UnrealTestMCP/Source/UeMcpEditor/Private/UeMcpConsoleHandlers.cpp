// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// `console.exec` — drive a console command at the editor or PIE world and
// capture whatever the command writes back to its FOutputDevice.
//
// World resolution defaults to `"editor"` (not `"auto"`) so a console
// command never silently targets PlayWorld just because PIE happens to
// be running — explicit `"pie"` or `"auto"` opts in.

#include "UeMcpConsoleHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/OutputDevice.h"

#include "UeMcpDispatcher.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpConsoleHandlersPrivate
{
	static constexpr double DispatcherTimeoutSeconds = 10.0;

	/**
	 * `FOutputDevice` that captures every Serialize() line into a
	 * TArray<FString>. Stack-scoped to one Exec call. The CanBeUsedOn*
	 * overrides let console commands that hand the device to a worker
	 * thread (rare but legal) write back without crashing.
	 */
	class FConsoleCaptureDevice : public FOutputDevice
	{
	public:
		TArray<FString> Lines;

		virtual void Serialize(
			const TCHAR* V, ELogVerbosity::Type /*Verbosity*/, const FName& /*Category*/) override
		{
			if (V != nullptr)
			{
				Lines.Add(FString(V));
			}
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
	};

	/**
	 * Parse the `world` arg into a strict EWorldScope. Default is Editor
	 * (not Auto) — see file header. Returns false + populates OutError
	 * on an unrecognised value.
	 */
	static bool ParseStrictWorldScope(
		const TSharedRef<FJsonObject>& Args,
		UeMcp::EWorldScope& OutScope,
		TSharedPtr<FJsonObject>& OutError)
	{
		FString Raw;
		if (!Args->TryGetStringField(TEXT("world"), Raw) || Raw.IsEmpty())
		{
			OutScope = UeMcp::EWorldScope::Editor;
			return true;
		}
		const FString Lower = Raw.ToLower();
		if (Lower == TEXT("editor")) { OutScope = UeMcp::EWorldScope::Editor; return true; }
		if (Lower == TEXT("pie"))    { OutScope = UeMcp::EWorldScope::PIE;    return true; }
		if (Lower == TEXT("auto"))   { OutScope = UeMcp::EWorldScope::Auto;   return true; }

		OutError = UeMcp::MakeInlineError(
			TEXT("SCHEMA_ERROR"),
			FString::Printf(
				TEXT("`world` must be one of 'editor', 'pie', 'auto' (got '%s')"),
				*Raw));
		return false;
	}

	/** `console.exec` body. */
	static TSharedRef<FJsonObject> HandleConsoleExec(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Command;
		if (!Args->TryGetStringField(TEXT("command"), Command) || Command.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`command` is required and must be a non-empty string"));
		}

		UeMcp::EWorldScope Scope = UeMcp::EWorldScope::Editor;
		TSharedPtr<FJsonObject> ScopeError;
		if (!ParseStrictWorldScope(Args, Scope, ScopeError))
		{
			return ScopeError.ToSharedRef();
		}
		const UeMcp::FWorldResolution World = UeMcp::ResolveWorld(Scope);
		if (!World.IsOk())
		{
			// PIE-required scope yields NOT_IN_PIE; sharpen the message so
			// callers see the tool name in the failure log.
			if (World.ErrorCode == TEXT("NOT_IN_PIE"))
			{
				return UeMcp::MakeInlineError(
					TEXT("NOT_IN_PIE"),
					TEXT("console.exec requires an active PIE session when world='pie'"));
			}
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		// GEngine is what hosts Exec — guarded for the teardown race.
		if (GEngine == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEngine is null"));
		}

		FConsoleCaptureDevice Capture;
		const double StartSeconds = FPlatformTime::Seconds();
		const bool bExecOk = GEngine->Exec(World.World, *Command, Capture);
		const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("success"), bExecOk);
		Data->SetStringField(TEXT("command"), Command);
		Data->SetStringField(TEXT("world"),
			World.bIsPIE ? TEXT("pie") : TEXT("editor"));
		Data->SetNumberField(TEXT("elapsed_ms"), ElapsedMs);

		TArray<TSharedPtr<FJsonValue>> OutputArr;
		OutputArr.Reserve(Capture.Lines.Num());
		for (const FString& Line : Capture.Lines)
		{
			OutputArr.Add(MakeShared<FJsonValueString>(Line));
		}
		Data->SetArrayField(TEXT("output"), OutputArr);

		return Data;
	}
}

void UeMcp::RegisterConsoleHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpConsoleHandlersPrivate;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("console.exec"));
	Reg.DefaultTimeoutSeconds = DispatcherTimeoutSeconds;
	// Mutating: a console command can change anything from r.* cvars to
	// world state. Mark accordingly so the surface metadata is honest.
	Reg.bMutating = true;
	Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleConsoleExec);
	Dispatcher.RegisterTool(Reg);
}
