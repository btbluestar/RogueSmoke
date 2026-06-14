// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Tool-call ledger. One JSONL line per MCP request received by the plugin,
// in the canonical mcp-process-mining schema (see
// `mpc-process-mining/docs/mcp-process-mining-spec.md`):
//
//   {"ts":"2026-05-02T14:23:11.432Z","case_id":"<uuid>",
//    "server":"unreal-test-mcp","tool":"blueprint.graph.add_node",
//    "status":"success","duration_ms":17.23,
//    "params":{"bp_path":"/Game/..."},"error_class":null}
//
// Logs land under `<Project>/Saved/Logs/unreal-test-mcp/<YYYY-MM-DD>.jsonl`
// by default — daily rotation per the spec, so date-filtered scans can
// skip whole files. Append-only; survives editor restarts. Intended as
// the source of truth for tool-usage analysis — e.g. "after `add_node`
// we almost always call `set_pin_default` within 200ms" → specialise.
//
// Writes happen from the transport worker thread; UE's file-manager
// append path is safe there. Writes are one-at-a-time serialised by a
// critical section so concurrent dispatchers (if we ever add a parallel
// lane for reads) don't interleave lines. Per-call open-append-close on
// the underlying archive is a few microseconds on local disk and gives
// us crash safety for free.
//
// Migration: on first init we move any legacy
// `<Project>/Saved/Logs/mcp-tool-calls.jsonl` to
// `<Project>/Saved/Logs/archived/` so the new daily files don't mix with
// the old single-file legacy schema.
//
// Environment knobs:
//   - `UE_MCP_TOOL_CALL_LOG=off` disables writes entirely.
//   - `UE_MCP_LOG_ROOT=<abs-dir>` overrides the parent directory; the
//     `<server_name>/<YYYY-MM-DD>.jsonl` layout is appended underneath.
//   - `UE_MCP_TOOL_CALL_LOG_PATH=<abs-file>` (legacy) overrides the full
//     destination as a single growing file. When set, daily rotation and
//     the server-name subdir are bypassed — useful for tests / CI.
//   - `UE_MCP_TOOL_CALL_LOG_ARGS_MAX=N` caps the params JSON size in
//     chars (default 4000). When the source params exceed N, `params` is
//     emitted as `null` and `details` records the original size — we never
//     emit a half-truncated JSON object.
//   - `UE_MCP_CASE_ID=<id>` pins the `case_id` field; when unset, the
//     plugin generates a per-process UUID at first write so events from
//     one editor session correlate.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace UeMcp
{
	/**
	 * Record one tool-call event to the JSONL ledger in the canonical
	 * mcp-process-mining schema.
	 *
	 * Safe to call from any thread (the file handle is protected by a
	 * critical section). A no-op when the log is disabled.
	 *
	 * `ParamsJsonOrEmpty` is the serialised arguments object as compact
	 * JSON (e.g. `{"foo":"bar"}`) or empty for parameter-less calls. The
	 * logger embeds it inline as a structured object — when the size
	 * exceeds `UE_MCP_TOOL_CALL_LOG_ARGS_MAX`, `params` is set to `null`
	 * and the original size goes into `details.params_size_chars`.
	 *
	 * `ErrorCode` is the upper-snake error code when `bOk == false`;
	 * ignored otherwise. It is emitted as `error_class` in the canonical
	 * schema. Codes containing the literal substring `TIMEOUT` produce
	 * `status: "timeout"`; other failures produce `status: "error"`.
	 *
	 * `RequestId` is the per-call request id and is preserved in the
	 * `details.request_id` field for back-correlation with editor logs.
	 */
	UEMCPRUNTIME_API void LogToolCall(
		const FString& RequestId,
		const FString& ToolName,
		const FString& ParamsJsonOrEmpty,
		bool bOk,
		const FString& ErrorCode,
		double DurationMs);
}
