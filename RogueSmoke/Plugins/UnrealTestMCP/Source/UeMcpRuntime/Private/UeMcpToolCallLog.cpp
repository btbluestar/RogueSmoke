// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpToolCallLog.h"

#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"

namespace UeMcpToolCallLogPrivate
{
	/** Default params-size cap (chars). Chosen so typical structured
	 *  args fit inline as full JSON; only outlier huge payloads (very
	 *  large script bodies) get null'd out. */
	static constexpr int32 DefaultArgsMaxChars = 4000;

	/** Server name emitted in every event. Matches the FastMCP server
	 *  name in `server.py`; cross-server pattern mining keys on this. */
	static constexpr const TCHAR* ServerName = TEXT("unreal-test-mcp");

	/** Legacy single-file destination. Migrated under archived/ once on
	 *  first init so new daily files don't sit next to old schema. */
	static constexpr const TCHAR* LegacyFileName = TEXT("mcp-tool-calls.jsonl");

	/** Parsed configuration (read once at first use, never mutated). */
	struct FConfig
	{
		bool bEnabled = true;
		int32 ArgsMaxChars = DefaultArgsMaxChars;
		/** When non-empty, write every event to this single file (legacy
		 *  override). Daily rotation and the server subdir are bypassed. */
		FString OverrideFilePath;
		/** Parent directory for the canonical layout. The destination for
		 *  any given write is `LogRoot/<server>/<YYYY-MM-DD>.jsonl`. */
		FString LogRoot;
		/** case_id for every event in this process. */
		FString CaseId;
	};

	/** State guarded by Lock. Resolved lazily on first LogToolCall(). */
	static FCriticalSection Lock;
	static bool bInitialised = false;
	static FConfig Cfg;

	static FString DefaultLogRoot()
	{
		return FPaths::ConvertRelativePathToFull(
			FPaths::ProjectSavedDir() / TEXT("Logs"));
	}

	/** Move legacy `<LogRoot>/mcp-tool-calls.jsonl` (if present) under
	 *  `<LogRoot>/archived/`. One-shot, idempotent: a second invocation
	 *  appends `.<n>` if a same-named archive already exists.
	 *
	 *  Runs once per process during init, regardless of whether legacy
	 *  exists — `IFileManager::FileExists` is cheap. */
	static void ArchiveLegacyIfPresent(const FString& LogRoot)
	{
		IFileManager& FM = IFileManager::Get();
		const FString LegacyPath = LogRoot / LegacyFileName;
		if (!FM.FileExists(*LegacyPath))
		{
			return;
		}

		const FString ArchivedDir = LogRoot / TEXT("archived");
		FM.MakeDirectory(*ArchivedDir, /*Tree=*/ true);

		FString Dest = ArchivedDir / LegacyFileName;
		int32 Suffix = 1;
		while (FM.FileExists(*Dest))
		{
			Dest = ArchivedDir / FString::Printf(TEXT("mcp-tool-calls.%d.jsonl"), Suffix++);
		}

		// Move (rename). On Windows this is atomic when source and dest
		// are on the same volume — they always are for files under
		// ProjectSavedDir.
		FM.Move(*Dest, *LegacyPath);
	}

	/** Resolve env vars and compute the configuration. Called once. */
	static void InitialiseUnderLock()
	{
		if (bInitialised)
		{
			return;
		}

		// Kill-switch.
		const FString Disable =
			FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_TOOL_CALL_LOG"));
		if (Disable.Equals(TEXT("off"), ESearchCase::IgnoreCase) ||
			Disable == TEXT("0") ||
			Disable.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			Cfg.bEnabled = false;
		}

		// Args cap.
		const FString ArgsMaxEnv =
			FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_TOOL_CALL_LOG_ARGS_MAX"));
		if (!ArgsMaxEnv.IsEmpty())
		{
			const int32 Parsed = FCString::Atoi(*ArgsMaxEnv);
			if (Parsed >= 0)
			{
				Cfg.ArgsMaxChars = Parsed;
			}
		}

		// Log root (canonical layout uses `<root>/<server>/<date>.jsonl`).
		const FString RootEnv =
			FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_LOG_ROOT"));
		Cfg.LogRoot = !RootEnv.IsEmpty()
			? FPaths::ConvertRelativePathToFull(RootEnv)
			: DefaultLogRoot();

		// Legacy single-file override. Bypasses rotation and the server
		// subdir; useful for CI / tests pointing at a fixed temp path.
		const FString OverridePath =
			FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_TOOL_CALL_LOG_PATH"));
		if (!OverridePath.IsEmpty())
		{
			Cfg.OverrideFilePath = FPaths::ConvertRelativePathToFull(OverridePath);
		}

		// case_id: env-pinned (cross-server correlation) or per-process
		// UUID (single-server fallback). Spec §"Case ID propagation".
		const FString CaseIdEnv =
			FPlatformMisc::GetEnvironmentVariable(TEXT("UE_MCP_CASE_ID"));
		Cfg.CaseId = !CaseIdEnv.IsEmpty()
			? CaseIdEnv
			: FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();

		// Migrate the legacy single-file ledger out of the way of the new
		// daily layout — one-shot. Skipped when the override path is set,
		// since a CI run pointing at a temp path may legitimately have
		// `mcp-tool-calls.jsonl` files we shouldn't touch.
		if (Cfg.OverrideFilePath.IsEmpty())
		{
			ArchiveLegacyIfPresent(Cfg.LogRoot);
		}

		bInitialised = true;
	}

	/**
	 * Escape a string for inclusion in a JSON string literal. UE's JSON
	 * writers escape as a side effect of serialisation; we write the
	 * line ourselves (avoids the ~200-byte allocation overhead of a
	 * `TJsonWriter` per call), so we do the escape ourselves — only the
	 * five forms that are actually required by RFC 8259.
	 */
	static FString JsonEscape(const FString& In)
	{
		FString Out;
		Out.Reserve(In.Len() + 8);
		for (TCHAR C : In)
		{
			switch (C)
			{
			case TEXT('"'):  Out.AppendChars(TEXT("\\\""), 2); break;
			case TEXT('\\'): Out.AppendChars(TEXT("\\\\"), 2); break;
			case TEXT('\n'): Out.AppendChars(TEXT("\\n"), 2); break;
			case TEXT('\r'): Out.AppendChars(TEXT("\\r"), 2); break;
			case TEXT('\t'): Out.AppendChars(TEXT("\\t"), 2); break;
			default:
				if (C < 0x20)
				{
					Out.Appendf(TEXT("\\u%04x"), static_cast<uint32>(C));
				}
				else
				{
					Out.AppendChar(C);
				}
				break;
			}
		}
		return Out;
	}

	/** Resolve the destination file for `Now`. When the legacy override
	 *  is set, returns it verbatim; otherwise `<root>/<server>/<date>.jsonl`. */
	static FString ResolveDestination(const FDateTime& Now)
	{
		if (!Cfg.OverrideFilePath.IsEmpty())
		{
			return Cfg.OverrideFilePath;
		}
		const FString DateStr = Now.ToString(TEXT("%Y-%m-%d"));
		return Cfg.LogRoot / ServerName / (DateStr + TEXT(".jsonl"));
	}

	/** Map a UE error code to the canonical `status` enum. Codes whose
	 *  upper-case form contains the literal `TIMEOUT` substring fold to
	 *  `"timeout"`; everything else with `bOk==false` is `"error"`. */
	static const TCHAR* ResolveStatus(bool bOk, const FString& ErrorCode)
	{
		if (bOk)
		{
			return TEXT("success");
		}
		const FString Upper = ErrorCode.ToUpper();
		if (Upper.Contains(TEXT("TIMEOUT")))
		{
			return TEXT("timeout");
		}
		return TEXT("error");
	}
}

void UeMcp::LogToolCall(
	const FString& RequestId,
	const FString& ToolName,
	const FString& ParamsJsonOrEmpty,
	bool bOk,
	const FString& ErrorCode,
	double DurationMs)
{
	using namespace UeMcpToolCallLogPrivate;

	FScopeLock ScopeLock(&Lock);
	InitialiseUnderLock();

	if (!Cfg.bEnabled)
	{
		return;
	}

	const FDateTime NowUtc = FDateTime::UtcNow();
	const FString TimestampIso = NowUtc.ToIso8601(); // e.g. 2026-05-02T14:23:11.432Z
	const TCHAR* Status = ResolveStatus(bOk, ErrorCode);

	// Build params + details fields. Params are inline JSON; when over
	// the cap we emit `null` and stash the original size in details so
	// the loss is observable.
	const int32 ParamsLen = ParamsJsonOrEmpty.Len();
	const bool bHasParams = ParamsLen > 0;
	const bool bParamsTruncated =
		bHasParams && Cfg.ArgsMaxChars > 0 && ParamsLen > Cfg.ArgsMaxChars;
	const bool bParamsOmitted =
		bHasParams && Cfg.ArgsMaxChars == 0;

	FString ParamsField;
	if (!bHasParams || bParamsOmitted || bParamsTruncated)
	{
		ParamsField = TEXT(",\"params\":null");
	}
	else
	{
		// Trust the caller produced valid JSON — we serialised it from a
		// FJsonObject ourselves at the call site.
		ParamsField = FString::Printf(TEXT(",\"params\":%s"), *ParamsJsonOrEmpty);
	}

	FString ErrorClassField = TEXT(",\"error_class\":null");
	if (!bOk && !ErrorCode.IsEmpty())
	{
		ErrorClassField = FString::Printf(
			TEXT(",\"error_class\":\"%s\""), *JsonEscape(ErrorCode));
	}

	// Details: preserve request_id always; record params size when we
	// dropped or truncated the structured payload.
	FString DetailsField;
	{
		FString Inner = FString::Printf(
			TEXT("\"request_id\":\"%s\""), *JsonEscape(RequestId));
		if (bParamsTruncated || bParamsOmitted)
		{
			Inner += FString::Printf(
				TEXT(",\"params_size_chars\":%d,\"params_truncated\":true"),
				ParamsLen);
		}
		DetailsField = FString::Printf(TEXT(",\"details\":{%s}"), *Inner);
	}

	const FString Line = FString::Printf(
		TEXT("{\"ts\":\"%s\",\"case_id\":\"%s\",\"server\":\"%s\","
		     "\"tool\":\"%s\",\"status\":\"%s\",\"duration_ms\":%.3f%s%s%s}\n"),
		*TimestampIso,
		*JsonEscape(Cfg.CaseId),
		ServerName,
		*JsonEscape(ToolName),
		Status,
		DurationMs,
		*ParamsField,
		*ErrorClassField,
		*DetailsField);

	const FString Destination = ResolveDestination(NowUtc);

	IFileManager& FM = IFileManager::Get();
	TUniquePtr<FArchive> Writer(FM.CreateFileWriter(
		*Destination, FILEWRITE_Append | FILEWRITE_AllowRead | FILEWRITE_Silent));
	if (!Writer.IsValid())
	{
		// Directory may not exist yet. Create and retry once.
		FM.MakeDirectory(*FPaths::GetPath(Destination), /*Tree=*/ true);
		Writer.Reset(FM.CreateFileWriter(
			*Destination, FILEWRITE_Append | FILEWRITE_AllowRead | FILEWRITE_Silent));
		if (!Writer.IsValid())
		{
			// Give up silently — we don't want logging failures to
			// surface as tool-call failures. The rest of the system is
			// still fine; the user just loses pattern data.
			return;
		}
	}

	FTCHARToUTF8 Utf8(*Line);
	Writer->Serialize(const_cast<ANSICHAR*>(Utf8.Get()), Utf8.Length());
	Writer->Close();
}
