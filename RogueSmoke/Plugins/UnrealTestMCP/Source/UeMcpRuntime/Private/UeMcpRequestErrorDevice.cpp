// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpRequestErrorDevice.h"

#include "Misc/ScopeLock.h"

FUeMcpRequestErrorDevice::FUeMcpRequestErrorDevice()
{
	// A request error device wants to see every warning/error regardless of
	// the global log suppression level for the owning category, so Classify
	// as always-log.
}

FUeMcpRequestErrorDevice::~FUeMcpRequestErrorDevice()
{
	// Safety net: if the owner forgot to detach, do it here before GLog
	// retains a dangling pointer to us.
	Detach();
}

void FUeMcpRequestErrorDevice::Attach()
{
	check(IsInGameThread());

	if (bAttached)
	{
		return;
	}

	if (GLog != nullptr)
	{
		GLog->AddOutputDevice(this);
		bAttached = true;
	}
}

void FUeMcpRequestErrorDevice::Detach()
{
	// Detach is permitted from the game thread only — matches Attach's
	// contract. We don't assert here because the destructor path can race
	// with late module teardown in tests.
	if (!bAttached)
	{
		return;
	}

	if (GLog != nullptr)
	{
		GLog->RemoveOutputDevice(this);
	}
	bAttached = false;
}

void FUeMcpRequestErrorDevice::Drain(TArray<FString>& OutWarnings, TArray<FString>& OutErrors)
{
	FScopeLock Lock(&BufferLock);
	OutWarnings = MoveTemp(Warnings);
	OutErrors = MoveTemp(Errors);
	Warnings.Reset();
	Errors.Reset();
}

void FUeMcpRequestErrorDevice::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (V == nullptr)
	{
		return;
	}

	// Only Warning / Error are interesting. Everything else is noise.
	// `SetColor` and `BreakRecursionGuard` variants come in as non-verbosity
	// high bits — mask to compare against the pure verbosity.
	const ELogVerbosity::Type RawVerbosity =
		static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);

	if (RawVerbosity != ELogVerbosity::Warning && RawVerbosity != ELogVerbosity::Error)
	{
		return;
	}

	const FString Formatted = FString::Printf(TEXT("[%s] %s"), *Category.ToString(), V);

	FScopeLock Lock(&BufferLock);
	if (RawVerbosity == ELogVerbosity::Error)
	{
		Errors.Add(Formatted);
	}
	else
	{
		Warnings.Add(Formatted);
	}
}
