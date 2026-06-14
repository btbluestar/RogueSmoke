// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"

class FUeMcpDispatcher;

/**
 * Python escape-hatch tool handler.
 *
 * Currently registers:
 *   - `python_exec` — runs arbitrary editor Python via
 *     `IPythonScriptPlugin::Get()->ExecPythonCommandEx(...)`. The outer
 *     MCP response `ok` flag is `true` whenever the plugin executed
 *     without throwing a C++-level exception; Python-side exceptions are
 *     reported as `data.ok_python == false` with the traceback in
 *     `data.command_result`.
 *
 * The "if you use this, submit feedback about a native tool" directive
 * is emitted by the Python MCP server — deliberately NOT in this
 * handler — per the contract documented in 07_V0_PLAN.md §4.10.
 */
namespace UeMcp
{
	/**
	 * Register the `python_exec` tool on the given dispatcher. The
	 * dispatcher must outlive any dispatched requests.
	 */
	UEMCPEDITOR_API void RegisterPythonExecHandler(FUeMcpDispatcher& Dispatcher);
}
