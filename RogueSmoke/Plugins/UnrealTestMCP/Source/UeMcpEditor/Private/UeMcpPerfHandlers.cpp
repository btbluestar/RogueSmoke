// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Perf observability handlers. See header for shape rationale.
//
// Engine-API notes (relevant for the next session):
//   * `GAverageFPS` / `GAverageMS` are ENGINE_API floats updated by
//     `UEngine::UpdateTimeAndHandleMaxTickRate` once per frame; both
//     measure the most recent rolling average the editor displays in
//     the FPS counter widget. Source: Engine/UnrealEngine.cpp:736.
//   * `GGameThreadTime`, `GRenderThreadTime` (RENDERCORE_API uint32) +
//     `GGameThreadTimeCriticalPath`, `GRenderThreadTimeCriticalPath`
//     are CPU-cycle counters set once per frame in `FViewport::Draw`.
//     We convert via `FPlatformTime::ToMilliseconds` for wire-friendly
//     ms floats. Source: RenderCore/Public/RenderTimer.h:107-129.
//   * GPU time: `RHIGetGPUFrameCycles()` (DynamicRHI.h:1301). The bare
//     `GGPUFrameTime` global is deprecated in UE 5.6+. Returns 0 when
//     no GPU profiling is wired (commonly the case for a headless
//     editor with no viewport). We surface 0.0 ms in that case rather
//     than synthesising an error — the field is documented as
//     best-effort.
//   * `FPlatformMemory::GetStats()` returns `FPlatformMemoryStats`
//     (which extends `FPlatformMemoryConstants` so total + page-size
//     constants come along free). All byte fields are `uint64` so we
//     emit them as `bytes` (numeric) in the response.
//
// Both handlers are read-only sync — single game-thread hop, build a
// JSON object, return. No pending-handler dance, no polling.

#include "UeMcpPerfHandlers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformMemory.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "RenderTimer.h"
#include "RHI.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"

// Engine-side globals (declared `extern` here per the standard pattern
// the rest of the engine uses — direct includes of the source-private
// .cpp are not an option). Both are ENGINE_API floats, defined in
// Runtime/Engine/Private/UnrealEngine.cpp.
extern ENGINE_API float GAverageFPS;
extern ENGINE_API float GAverageMS;

namespace UeMcpPerfHandlersPrivate
{
	/** Both handlers are cheap reads — generous timeout, never mutating. */
	static constexpr double DefaultTimeoutSeconds = 5.0;

	/**
	 * Convert a uint32 cycle count to milliseconds. Wraps the engine
	 * helper so the handler body stays declarative; cheap inline call.
	 * Returns 0.0 on a zero input — the engine reports zero before any
	 * frame has been rendered (e.g. immediately after editor launch on
	 * the first invocation).
	 */
	static double CyclesToMs(uint32 Cycles)
	{
		if (Cycles == 0)
		{
			return 0.0;
		}
		// Use ToMilliseconds64 + uint64 widening so we don't lose any
		// precision the float helper would. The numeric span we emit is
		// always in the single-digit-ms range so float would be fine,
		// but doubles are the wire convention for ms across the rest of
		// the handler surface.
		return FPlatformTime::ToMilliseconds64(static_cast<uint64>(Cycles));
	}

	/**
	 * `perf.frame_stats` body. Reads engine globals on the game thread,
	 * returns a JSON object with named timing fields. All values in ms
	 * (doubles) except `fps` (frames per second). Field guarantees:
	 *
	 *   fps                       : last rolling-average FPS (float)
	 *   frame_ms                  : last rolling-average frame time
	 *   game_thread_ms            : excluding idle wait
	 *   game_thread_critical_ms   : including dependent-wait time
	 *   render_thread_ms          : excluding idle wait
	 *   render_thread_critical_ms : including dependent-wait time
	 *   gpu_ms                    : last GPU frame time (0 when not
	 *                               available — headless / no viewport)
	 *
	 * Caller should treat any 0-valued field as "not yet measured" — the
	 * editor frequently returns 0 before the first frame has rendered.
	 */
	static TSharedRef<FJsonObject> HandlePerfFrameStats(
		const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

		Data->SetNumberField(TEXT("fps"), static_cast<double>(GAverageFPS));
		Data->SetNumberField(TEXT("frame_ms"), static_cast<double>(GAverageMS));

		Data->SetNumberField(TEXT("game_thread_ms"),
			CyclesToMs(GGameThreadTime));
		Data->SetNumberField(TEXT("game_thread_critical_ms"),
			CyclesToMs(GGameThreadTimeCriticalPath));
		Data->SetNumberField(TEXT("render_thread_ms"),
			CyclesToMs(GRenderThreadTime));
		Data->SetNumberField(TEXT("render_thread_critical_ms"),
			CyclesToMs(GRenderThreadTimeCriticalPath));

		// GPU time: prefer the global helper (RHIGetGPUFrameCycles)
		// because direct GGPUFrameTime is deprecated in 5.6+. Returns 0
		// when the RHI isn't tracking frame time — typical for a
		// headless / unfocused editor.
		const uint32 GpuCycles = RHIGetGPUFrameCycles();
		Data->SetNumberField(TEXT("gpu_ms"), CyclesToMs(GpuCycles));

		return Data;
	}

	/**
	 * `perf.memory_snapshot` body. Snapshots the platform memory stats
	 * on the game thread. All numeric fields in bytes (uint64 → JSON
	 * number). Wire shape:
	 *
	 *   used_physical_bytes
	 *   peak_used_physical_bytes
	 *   available_physical_bytes
	 *   total_physical_bytes
	 *   used_virtual_bytes
	 *   peak_used_virtual_bytes
	 *   available_virtual_bytes
	 *   total_virtual_bytes
	 *   total_physical_gb         (rough — engine-supplied estimate)
	 *   page_size_bytes
	 *
	 * Numeric values fit comfortably in JSON number precision (53 bits
	 * mantissa) for any practical RAM size; 64-bit RAM totals up to
	 * ~9 PB before the cast to double would lose a byte.
	 */
	static TSharedRef<FJsonObject> HandlePerfMemorySnapshot(
		const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const FPlatformMemoryStats Stats = FPlatformMemory::GetStats();

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

		Data->SetNumberField(TEXT("used_physical_bytes"),
			static_cast<double>(Stats.UsedPhysical));
		Data->SetNumberField(TEXT("peak_used_physical_bytes"),
			static_cast<double>(Stats.PeakUsedPhysical));
		Data->SetNumberField(TEXT("available_physical_bytes"),
			static_cast<double>(Stats.AvailablePhysical));
		Data->SetNumberField(TEXT("total_physical_bytes"),
			static_cast<double>(Stats.TotalPhysical));

		Data->SetNumberField(TEXT("used_virtual_bytes"),
			static_cast<double>(Stats.UsedVirtual));
		Data->SetNumberField(TEXT("peak_used_virtual_bytes"),
			static_cast<double>(Stats.PeakUsedVirtual));
		Data->SetNumberField(TEXT("available_virtual_bytes"),
			static_cast<double>(Stats.AvailableVirtual));
		Data->SetNumberField(TEXT("total_virtual_bytes"),
			static_cast<double>(Stats.TotalVirtual));

		Data->SetNumberField(TEXT("total_physical_gb"),
			static_cast<double>(Stats.TotalPhysicalGB));
		Data->SetNumberField(TEXT("page_size_bytes"),
			static_cast<double>(Stats.PageSize));

		return Data;
	}
}

void UeMcp::RegisterPerfHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpPerfHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("perf.frame_stats"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandlePerfFrameStats);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("perf.memory_snapshot"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandlePerfMemorySnapshot);
		Dispatcher.RegisterTool(Reg);
	}
}
