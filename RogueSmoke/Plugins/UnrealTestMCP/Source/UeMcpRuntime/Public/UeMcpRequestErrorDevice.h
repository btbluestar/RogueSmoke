// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDevice.h"

/**
 * `FOutputDevice` that attaches to `GLog` for the duration of a single
 * MCP request and captures `Error` / `Warning` lines into buffers the
 * dispatcher can drain and surface on the response.
 *
 * Shape motivated by the ChiR24 `FMcpRequestErrorDevice` pattern (MIT).
 * The important policy is that captured lines are NOT suppressed from
 * the normal log pipeline — `FOutputDevice::Serialize` runs alongside
 * the regular sinks, so anyone tailing the editor log still sees every
 * line. We just make a structured copy for the response.
 *
 * `Attach` / `Detach` must be called from the game thread; `Serialize`
 * is invoked from whichever thread emitted the log line and is
 * synchronized internally with a `FCriticalSection`.
 */
class UEMCPRUNTIME_API FUeMcpRequestErrorDevice : public FOutputDevice
{
public:
	FUeMcpRequestErrorDevice();
	virtual ~FUeMcpRequestErrorDevice() override;

	/** Register this device with `GLog`. Game-thread only. */
	void Attach();

	/** Unregister from `GLog`. Game-thread only. Safe to call twice. */
	void Detach();

	/**
	 * Move accumulated lines into the out params and clear internal buffers.
	 * Call after `Detach()` so no further `Serialize` calls race the drain.
	 */
	void Drain(TArray<FString>& OutWarnings, TArray<FString>& OutErrors);

	// FOutputDevice
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity,
		const FName& Category) override;

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

private:
	FCriticalSection BufferLock;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	bool bAttached = false;
};
