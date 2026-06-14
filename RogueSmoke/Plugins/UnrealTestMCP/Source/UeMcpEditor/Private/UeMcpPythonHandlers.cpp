// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpPythonHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformTime.h"
#include "IPythonScriptPlugin.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"

namespace UeMcpPythonHandlersPrivate
{
	/** Default timeout for `python_exec`. Long because Python can do anything. */
	static constexpr double DefaultTimeoutSeconds = 60.0;

	/**
	 * Emit a handler-error-shaped payload. The dispatcher doesn't currently
	 * hoist this into a top-level error response, but the code+message fields
	 * are still machine-readable inside `data` and match the taxonomy shape.
	 */
	static TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}

	/** Map `EPythonLogOutputType` to the string surface we use on the wire. */
	static const TCHAR* LogTypeToString(EPythonLogOutputType Type)
	{
		switch (Type)
		{
		case EPythonLogOutputType::Info:
			return TEXT("Info");
		case EPythonLogOutputType::Warning:
			return TEXT("Warning");
		case EPythonLogOutputType::Error:
			return TEXT("Error");
		default:
			return TEXT("Info");
		}
	}
}

void UeMcp::RegisterPythonExecHandler(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpPythonHandlersPrivate;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("python_exec"));
	Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
	// Semantically mutating — arbitrary Python can mutate arbitrary state.
	Reg.bMutating = true;
	Reg.Handler.BindLambda(
		[](const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/) -> TSharedRef<FJsonObject>
		{
			check(IsInGameThread());

			// Args validation — only `script` is required for v0.
			FString Script;
			if (!Args->TryGetStringField(TEXT("script"), Script) || Script.IsEmpty())
			{
				return MakeInlineError(
					TEXT("SCHEMA_ERROR"),
					TEXT("python_exec requires args.script (non-empty string)"));
			}

			IPythonScriptPlugin* PyPlugin = IPythonScriptPlugin::Get();
			if (PyPlugin == nullptr || !PyPlugin->IsPythonAvailable())
			{
				return MakeInlineError(
					TEXT("PLUGIN_INTERNAL_ERROR"),
					TEXT("Python Script Plugin not loaded. Enable 'Python Editor Script Plugin'."));
			}

			// Build the command. ExecuteFile mode is the documented path for
			// multi-statement scripts; Public execution scope makes the
			// resulting module globals live across subsequent calls, which
			// matches an interactive editor Python session.
			FPythonCommandEx Command;
			Command.Command = Script;
			Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
			Command.FileExecutionScope = EPythonFileExecutionScope::Public;

			const double StartSec = FPlatformTime::Seconds();
			const bool bOkPython = PyPlugin->ExecPythonCommandEx(Command);
			const double DurationMs = (FPlatformTime::Seconds() - StartSec) * 1000.0;

			// Capture a `result` global from the script's namespace, if any.
			// `ExecuteFile` doesn't return an expression value — CommandResult
			// for that mode is typically the string "None". To give callers
			// a usable return channel we run a follow-up `EvaluateStatement`
			// that evaluates `result` from __main__ (the Public execution
			// scope). UE's `EvaluateStatement` stringifies via Python's
			// `repr()`, so an int `42` comes back as "42", a float as
			// "5000.0", a list as "[1, 2, 3]", a str as "'hello'" (quoted),
			// and unset `result` as "None". Scripts that want clean string
			// output can still `print(...)` and parse `log_output.combined`.
			FString FinalCommandResult;
			if (bOkPython)
			{
				FPythonCommandEx CaptureCommand;
				CaptureCommand.Command =
					TEXT("result if 'result' in dir() else None");
				CaptureCommand.ExecutionMode =
					EPythonCommandExecutionMode::EvaluateStatement;
				CaptureCommand.FileExecutionScope =
					EPythonFileExecutionScope::Public;
				if (PyPlugin->ExecPythonCommandEx(CaptureCommand))
				{
					FinalCommandResult = CaptureCommand.CommandResult;
				}
				else
				{
					// Capture-statement itself failed; fall back to the
					// user-script's CommandResult (usually "None").
					FinalCommandResult = Command.CommandResult;
				}
			}
			else
			{
				// Script raised — preserve the traceback / error info UE
				// put in CommandResult.
				FinalCommandResult = Command.CommandResult;
			}

			// Build the response payload per the schema in 07_V0_PLAN.md §4.10.
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("ok_python"), bOkPython);
			Data->SetStringField(TEXT("command_result"), FinalCommandResult);
			Data->SetNumberField(TEXT("duration_ms"), DurationMs);

			// log_output: combined flat string + structured entries.
			{
				TSharedRef<FJsonObject> LogOutput = MakeShared<FJsonObject>();

				TArray<TSharedPtr<FJsonValue>> Entries;
				Entries.Reserve(Command.LogOutput.Num());
				FString Combined;
				Combined.Reserve(Command.LogOutput.Num() * 64);

				for (const FPythonLogOutputEntry& E : Command.LogOutput)
				{
					TSharedRef<FJsonObject> EntryObj = MakeShared<FJsonObject>();
					EntryObj->SetStringField(TEXT("type"), LogTypeToString(E.Type));
					EntryObj->SetStringField(TEXT("output"), E.Output);
					Entries.Add(MakeShared<FJsonValueObject>(EntryObj));

					Combined.Append(E.Output);
					if (!E.Output.EndsWith(TEXT("\n")))
					{
						Combined.AppendChar(TEXT('\n'));
					}
				}

				LogOutput->SetStringField(TEXT("combined"), Combined);
				LogOutput->SetArrayField(TEXT("entries"), Entries);
				Data->SetObjectField(TEXT("log_output"), LogOutput);
			}

			return Data;
		});

	Dispatcher.RegisterTool(Reg);
}
