// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpLogHandler.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/CriticalSection.h"
#include "Logging/LogMacros.h"
#include "Logging/LogVerbosity.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

#include "UeMcpDispatcher.h"
#include "UeMcpEditorSubsystem.h" // LogUeMcpEditor
#include "UeMcpGameThreadExecutor.h"

namespace UeMcpLogHandlerPrivate
{
	/** Capacity of the ring buffer. 5000 lines is the §4.9 default. */
	static constexpr int32 kMaxLines = 5000;

	/** Default / max `n_lines` for `log.tail`. */
	static constexpr int32 DefaultTailLines = 100;
	static constexpr int32 MaxTailLines = kMaxLines;

	/** Default dispatcher timeout — read-only and cheap. */
	static constexpr double TailDispatcherTimeoutSeconds = 5.0;

	/** One captured log record. POD-ish so it's cheap to copy under the lock. */
	struct FLogEntry
	{
		ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
		FName Category;
		FString Message;
		int64 TimestampUnixMs = 0;
	};

	/** Map a verbosity enum value to the wire string. */
	static const TCHAR* VerbosityToWire(ELogVerbosity::Type Verbosity)
	{
		// Strip all flag bits; engine sometimes ORs in side-band info.
		const ELogVerbosity::Type Base =
			static_cast<ELogVerbosity::Type>(
				static_cast<uint8>(Verbosity) & ELogVerbosity::VerbosityMask);
		switch (Base)
		{
			case ELogVerbosity::Fatal:       return TEXT("Fatal");
			case ELogVerbosity::Error:       return TEXT("Error");
			case ELogVerbosity::Warning:     return TEXT("Warning");
			case ELogVerbosity::Display:     return TEXT("Display");
			case ELogVerbosity::Log:         return TEXT("Log");
			case ELogVerbosity::Verbose:     return TEXT("Verbose");
			case ELogVerbosity::VeryVerbose: return TEXT("VeryVerbose");
			default:                         return TEXT("Log");
		}
	}

	/**
	 * Fixed-capacity ring buffer `FOutputDevice`. Lives as a singleton for
	 * the lifetime of the editor subsystem.
	 */
	class FUeMcpLogRingBuffer final : public FOutputDevice
	{
	public:
		FUeMcpLogRingBuffer()
		{
			Entries.Reserve(kMaxLines);
		}

		virtual ~FUeMcpLogRingBuffer() override = default;

		// FOutputDevice
		virtual void Serialize(
			const TCHAR* V,
			ELogVerbosity::Type Verbosity,
			const FName& Category) override
		{
			// Ignore the "set color" pseudo-lines — SetColor / SET_* lines.
			if (V == nullptr)
			{
				return;
			}

			FLogEntry Entry;
			Entry.Verbosity = Verbosity;
			Entry.Category = Category;
			Entry.Message = V;
			Entry.TimestampUnixMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000
				+ FDateTime::UtcNow().GetMillisecond();

			FScopeLock Lock(&CriticalSection);
			if (Entries.Num() < kMaxLines)
			{
				Entries.Add(MoveTemp(Entry));
			}
			else
			{
				// Ring-slot reuse.
				Entries[WriteIndex] = MoveTemp(Entry);
			}
			WriteIndex = (WriteIndex + 1) % kMaxLines;
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		/**
		 * Snapshot the ring buffer into `OutEntries` in oldest-first
		 * order. Copies under the lock; returns fast.
		 */
		void Snapshot(TArray<FLogEntry>& OutEntries) const
		{
			FScopeLock Lock(&CriticalSection);

			const int32 N = Entries.Num();
			OutEntries.Reset(N);

			if (N < kMaxLines)
			{
				// Buffer not yet filled; oldest is at index 0.
				OutEntries.Append(Entries);
				return;
			}

			// Buffer full; oldest is at WriteIndex (the next slot to overwrite).
			OutEntries.Reserve(N);
			for (int32 Offset = 0; Offset < N; ++Offset)
			{
				const int32 Idx = (WriteIndex + Offset) % kMaxLines;
				OutEntries.Add(Entries[Idx]);
			}
		}

		int32 GetCurrentSize() const
		{
			FScopeLock Lock(&CriticalSection);
			return Entries.Num();
		}

	private:
		mutable FCriticalSection CriticalSection;
		TArray<FLogEntry> Entries;
		int32 WriteIndex = 0;
	};

	/** Module-scoped singleton. Owned via raw pointer because
	 *  `GLog->AddOutputDevice` takes a non-owning pointer and deletion
	 *  must happen after removal. */
	static FUeMcpLogRingBuffer* GRingBuffer = nullptr;

	/** Build an inline error payload. */
	static TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}

	/** Serialize one entry to wire JSON. Called outside the lock. */
	static TSharedRef<FJsonObject> EntryToJson(const FLogEntry& Entry)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("verbosity"), VerbosityToWire(Entry.Verbosity));
		Obj->SetStringField(TEXT("category"), Entry.Category.ToString());
		Obj->SetStringField(TEXT("message"), Entry.Message);
		Obj->SetNumberField(TEXT("timestamp_unix_ms"),
			static_cast<double>(Entry.TimestampUnixMs));
		return Obj;
	}

	// -------------------------------------------------------------------------
	// Windowed capture infrastructure (Gap 4)
	// -------------------------------------------------------------------------

	/**
	 * Wire-level verbosity string -> ELogVerbosity::Type.
	 * Returns ELogVerbosity::All (=0) for unrecognised strings so that all
	 * lines pass the ">= min_verbosity" filter (i.e. "accept everything").
	 *
	 * UE stores verbosity as uint8 where lower numeric value == higher
	 * severity:  Fatal=1, Error=2, Warning=3, Display=4, Log=5, Verbose=6,
	 * VeryVerbose=7, All=MAX_uint8.
	 * A line passes the filter when  line.Verbosity <= MinVerbosity  (more
	 * severe or equally severe).
	 */
	static ELogVerbosity::Type WireToVerbosity(const FString& Wire)
	{
		if (Wire.Equals(TEXT("error"),       ESearchCase::IgnoreCase)) return ELogVerbosity::Error;
		if (Wire.Equals(TEXT("warning"),     ESearchCase::IgnoreCase)) return ELogVerbosity::Warning;
		if (Wire.Equals(TEXT("display"),     ESearchCase::IgnoreCase)) return ELogVerbosity::Display;
		if (Wire.Equals(TEXT("log"),         ESearchCase::IgnoreCase)) return ELogVerbosity::Log;
		if (Wire.Equals(TEXT("verbose"),     ESearchCase::IgnoreCase)) return ELogVerbosity::Verbose;
		if (Wire.Equals(TEXT("veryverbose"), ESearchCase::IgnoreCase)) return ELogVerbosity::VeryVerbose;
		// Default: accept every verbosity level.
		return ELogVerbosity::All;
	}

	/** One active windowed capture session. */
	struct FCaptureSession
	{
		/** Lower-cased category set; empty == accept all. */
		TArray<FString> Categories;

		/** Only record lines at this severity or higher (numerically <=). */
		ELogVerbosity::Type MinVerbosity = ELogVerbosity::Warning;

		/** Captured lines (guarded by its own lock). */
		TArray<FLogEntry> Lines;
		mutable FCriticalSection LinesLock;

		/** Wall-clock start for duration_ms computation. */
		double StartSeconds = 0.0;
	};

	/**
	 * Map of all live capture sessions, keyed by UUID string.
	 * The outer lock must be held when inserting / removing; each session's
	 * own `LinesLock` guards `Lines` (fine-grained: Serialize doesn't need
	 * to hold the outer lock while appending).
	 */
	static FCriticalSection GCaptureMapLock;
	static TMap<FString, TSharedPtr<FCaptureSession>> GCaptureSessions;

	/**
	 * FOutputDevice sink for live windowed captures.
	 *
	 * There is ONE instance registered with GLog (like the ring buffer).
	 * It fans incoming lines out to all live sessions that want them.
	 * Created and destroyed alongside the ring buffer.
	 */
	class FUeMcpCaptureDevice final : public FOutputDevice
	{
	public:
		FUeMcpCaptureDevice() = default;
		virtual ~FUeMcpCaptureDevice() override = default;

		virtual void Serialize(
			const TCHAR* V,
			ELogVerbosity::Type Verbosity,
			const FName& Category) override
		{
			if (V == nullptr)
			{
				return;
			}

			// Build the entry once — shared across all matching sessions.
			FLogEntry Entry;
			Entry.Verbosity = Verbosity;
			Entry.Category = Category;
			Entry.Message = V;
			Entry.TimestampUnixMs = FDateTime::UtcNow().ToUnixTimestamp() * 1000
				+ FDateTime::UtcNow().GetMillisecond();

			const FString CategoryLower = Category.ToString().ToLower();

			// Snapshot the live-session list under the outer lock, then fan out.
			TArray<TSharedPtr<FCaptureSession>> Snapshot;
			{
				FScopeLock MapLock(&GCaptureMapLock);
				GCaptureSessions.GenerateValueArray(Snapshot);
			}

			for (const TSharedPtr<FCaptureSession>& Session : Snapshot)
			{
				if (!Session.IsValid())
				{
					continue;
				}

				// Verbosity filter: line must be at least as severe as the cap.
				// Lower numeric value == higher severity (Fatal=1, Error=2 …).
				const uint8 LineV = static_cast<uint8>(Verbosity) & ELogVerbosity::VerbosityMask;
				const uint8 MinV  = static_cast<uint8>(Session->MinVerbosity) & ELogVerbosity::VerbosityMask;
				if (LineV > MinV)
				{
					continue;
				}

				// Category filter (case-insensitive exact match).
				if (Session->Categories.Num() > 0)
				{
					bool bMatch = false;
					for (const FString& Cat : Session->Categories)
					{
						if (Cat == CategoryLower)
						{
							bMatch = true;
							break;
						}
					}
					if (!bMatch)
					{
						continue;
					}
				}

				FScopeLock LineLock(&Session->LinesLock);
				Session->Lines.Add(Entry);
			}
		}

		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
	};

	static FUeMcpCaptureDevice* GCaptureDevice = nullptr;

	// -------------------------------------------------------------------------
	// log.capture_begin handler
	// -------------------------------------------------------------------------

	static TSharedRef<FJsonObject> HandleLogCaptureBegin(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		// Parse categories (optional string array).
		TArray<FString> Categories;
		{
			const TArray<TSharedPtr<FJsonValue>>* CatArray = nullptr;
			if (Args->TryGetArrayField(TEXT("categories"), CatArray) && CatArray != nullptr)
			{
				for (const TSharedPtr<FJsonValue>& Val : *CatArray)
				{
					FString Cat;
					if (Val.IsValid() && Val->TryGetString(Cat) && !Cat.IsEmpty())
					{
						Categories.Add(Cat.ToLower());
					}
				}
			}
		}

		// Parse min_verbosity (optional string, default "warning").
		FString MinVerbosityStr = TEXT("warning");
		Args->TryGetStringField(TEXT("min_verbosity"), MinVerbosityStr);
		const ELogVerbosity::Type MinVerbosity = WireToVerbosity(MinVerbosityStr);

		// Validate: the wire schema restricts to a known set. Reject anything
		// that mapped to All (unknown) unless it was explicitly omitted.
		if (MinVerbosity == ELogVerbosity::All
			&& !MinVerbosityStr.IsEmpty()
			&& !MinVerbosityStr.Equals(TEXT("warning"), ESearchCase::IgnoreCase))
		{
			TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
			Err->SetStringField(TEXT("error"), TEXT("INVALID_PAYLOAD"));
			Err->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Unknown min_verbosity value '%s'. "
					"Valid values: error | warning | display | log | verbose | veryverbose."),
					*MinVerbosityStr));
			return Err;
		}

		// Build and register the session.
		TSharedPtr<FCaptureSession> Session = MakeShared<FCaptureSession>();
		Session->Categories = MoveTemp(Categories);
		Session->MinVerbosity =
			(MinVerbosity == ELogVerbosity::All) ? ELogVerbosity::Warning : MinVerbosity;
		Session->StartSeconds = FPlatformTime::Seconds();

		const FString CaptureId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
		{
			FScopeLock MapLock(&GCaptureMapLock);
			GCaptureSessions.Add(CaptureId, Session);
		}

		UE_LOG(LogUeMcpEditor, Verbose, TEXT("log.capture_begin: id=%s"), *CaptureId);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("capture_id"), CaptureId);
		return Out;
	}

	// -------------------------------------------------------------------------
	// log.capture_end handler
	// -------------------------------------------------------------------------

	static TSharedRef<FJsonObject> HandleLogCaptureEnd(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		FString CaptureId;
		if (!Args->TryGetStringField(TEXT("capture_id"), CaptureId) || CaptureId.IsEmpty())
		{
			TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
			Err->SetStringField(TEXT("error"), TEXT("INVALID_PAYLOAD"));
			Err->SetStringField(TEXT("message"), TEXT("`capture_id` is required."));
			return Err;
		}

		TSharedPtr<FCaptureSession> Session;
		{
			FScopeLock MapLock(&GCaptureMapLock);
			TSharedPtr<FCaptureSession>* Found = GCaptureSessions.Find(CaptureId);
			if (Found == nullptr)
			{
				TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
				Err->SetStringField(TEXT("error"), TEXT("NOT_FOUND"));
				Err->SetStringField(TEXT("message"),
					FString::Printf(TEXT("No active capture with id '%s'."), *CaptureId));
				return Err;
			}
			Session = *Found;
			GCaptureSessions.Remove(CaptureId);
		}

		const double EndSeconds = FPlatformTime::Seconds();
		const int32 DurationMs =
			static_cast<int32>((EndSeconds - Session->StartSeconds) * 1000.0);

		// Snapshot the lines under the session lock before we release it.
		TArray<FLogEntry> CapturedLines;
		{
			FScopeLock LineLock(&Session->LinesLock);
			CapturedLines = Session->Lines;
		}

		// Build count_by_verbosity map.
		int32 CountError      = 0;
		int32 CountWarning    = 0;
		int32 CountDisplay    = 0;
		int32 CountLog        = 0;
		int32 CountVerbose    = 0;
		int32 CountVeryVerbose = 0;

		TArray<TSharedPtr<FJsonValue>> LinesJson;
		LinesJson.Reserve(CapturedLines.Num());

		for (const FLogEntry& Entry : CapturedLines)
		{
			LinesJson.Add(MakeShared<FJsonValueObject>(EntryToJson(Entry)));

			const ELogVerbosity::Type V =
				static_cast<ELogVerbosity::Type>(
					static_cast<uint8>(Entry.Verbosity) & ELogVerbosity::VerbosityMask);
			switch (V)
			{
				case ELogVerbosity::Fatal:
				case ELogVerbosity::Error:       ++CountError;       break;
				case ELogVerbosity::Warning:     ++CountWarning;     break;
				case ELogVerbosity::Display:     ++CountDisplay;     break;
				case ELogVerbosity::Log:         ++CountLog;         break;
				case ELogVerbosity::Verbose:     ++CountVerbose;     break;
				case ELogVerbosity::VeryVerbose: ++CountVeryVerbose; break;
				default: break;
			}
		}

		TSharedRef<FJsonObject> CountByVerbosity = MakeShared<FJsonObject>();
		CountByVerbosity->SetNumberField(TEXT("error"),       CountError);
		CountByVerbosity->SetNumberField(TEXT("warning"),     CountWarning);
		CountByVerbosity->SetNumberField(TEXT("display"),     CountDisplay);
		CountByVerbosity->SetNumberField(TEXT("log"),         CountLog);
		CountByVerbosity->SetNumberField(TEXT("verbose"),     CountVerbose);
		CountByVerbosity->SetNumberField(TEXT("veryverbose"), CountVeryVerbose);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetArrayField(TEXT("lines"), LinesJson);
		Out->SetObjectField(TEXT("count_by_verbosity"), CountByVerbosity);
		Out->SetNumberField(TEXT("duration_ms"), DurationMs);

		UE_LOG(LogUeMcpEditor, Verbose,
			TEXT("log.capture_end: id=%s lines=%d duration_ms=%d"),
			*CaptureId, CapturedLines.Num(), DurationMs);

		return Out;
	}

	/** `log.tail` body. */
	static TSharedRef<FJsonObject> HandleLogTail(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());
		UE_LOG(LogUeMcpEditor, VeryVerbose, TEXT("log.tail dispatch"));

		// --- Parse args ---
		int32 NLines = DefaultTailLines;
		{
			int32 Parsed = DefaultTailLines;
			if (Args->TryGetNumberField(TEXT("n_lines"), Parsed))
			{
				NLines = Parsed;
			}
		}
		NLines = FMath::Clamp(NLines, 1, MaxTailLines);

		FString Filter;
		Args->TryGetStringField(TEXT("filter"), Filter);
		const FString FilterLower = Filter.ToLower();

		FString CategoryFilter;
		Args->TryGetStringField(TEXT("category"), CategoryFilter);

		int64 SinceUnixMs = 0;
		bool bHaveSince = false;
		{
			double Parsed = 0.0;
			if (Args->TryGetNumberField(TEXT("since_unix_ms"), Parsed))
			{
				SinceUnixMs = static_cast<int64>(Parsed);
				bHaveSince = true;
			}
		}

		// --- Snapshot under the lock; filter outside ---
		TArray<FLogEntry> Snapshot;
		int32 CurrentSize = 0;
		if (GRingBuffer != nullptr)
		{
			GRingBuffer->Snapshot(Snapshot);
			CurrentSize = Snapshot.Num();
		}

		TArray<FLogEntry> Matched;
		Matched.Reserve(FMath::Min(Snapshot.Num(), NLines));

		for (const FLogEntry& Entry : Snapshot)
		{
			if (bHaveSince && Entry.TimestampUnixMs < SinceUnixMs)
			{
				continue;
			}
			if (!CategoryFilter.IsEmpty()
				&& !Entry.Category.ToString().Equals(CategoryFilter, ESearchCase::CaseSensitive))
			{
				continue;
			}
			if (!FilterLower.IsEmpty())
			{
				if (!Entry.Message.ToLower().Contains(FilterLower))
				{
					continue;
				}
			}
			Matched.Add(Entry);
		}

		const int32 TotalMatched = Matched.Num();
		const bool bTruncated = TotalMatched > NLines;

		// Keep the newest NLines entries (Matched is oldest-first).
		int32 StartIdx = 0;
		if (bTruncated)
		{
			StartIdx = TotalMatched - NLines;
		}

		TArray<TSharedPtr<FJsonValue>> Lines;
		Lines.Reserve(TotalMatched - StartIdx);
		for (int32 i = StartIdx; i < TotalMatched; ++i)
		{
			Lines.Add(MakeShared<FJsonValueObject>(EntryToJson(Matched[i])));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("lines"), Lines);
		Data->SetNumberField(TEXT("total_matched"), TotalMatched);
		Data->SetBoolField(TEXT("truncated"), bTruncated);
		Data->SetNumberField(TEXT("buffer_capacity"), kMaxLines);
		Data->SetNumberField(TEXT("buffer_current_size"), CurrentSize);
		return Data;
	}
}

void UeMcp::RegisterLogHandler(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpLogHandlerPrivate;

	// log.tail
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("log.tail"));
		Reg.DefaultTimeoutSeconds = TailDispatcherTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleLogTail);
		Dispatcher.RegisterTool(Reg);
	}

	// log.capture_begin — starts a windowed capture session.
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("log.capture_begin"));
		Reg.DefaultTimeoutSeconds = 5.0;
		Reg.bMutating = true; // allocates a session
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleLogCaptureBegin);
		Dispatcher.RegisterTool(Reg);
	}

	// log.capture_end — closes a capture session and returns lines.
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("log.capture_end"));
		Reg.DefaultTimeoutSeconds = 5.0;
		Reg.bMutating = true; // removes a session
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleLogCaptureEnd);
		Dispatcher.RegisterTool(Reg);
	}
}

void UeMcp::InitializeLogCapture()
{
	using namespace UeMcpLogHandlerPrivate;

	check(IsInGameThread());

	if (GRingBuffer != nullptr)
	{
		UE_LOG(LogUeMcpEditor, Warning,
			TEXT("InitializeLogCapture: ring buffer already attached; skipping"));
		return;
	}

	if (GLog == nullptr)
	{
		UE_LOG(LogUeMcpEditor, Error,
			TEXT("InitializeLogCapture: GLog is null; cannot attach ring buffer"));
		return;
	}

	GRingBuffer = new FUeMcpLogRingBuffer();
	GLog->AddOutputDevice(GRingBuffer);

	// Also attach the capture fan-out device.
	GCaptureDevice = new FUeMcpCaptureDevice();
	GLog->AddOutputDevice(GCaptureDevice);

	UE_LOG(LogUeMcpEditor, Verbose,
		TEXT("Log ring buffer and capture device attached to GLog (capacity %d)."), kMaxLines);
}

void UeMcp::ShutdownLogCapture()
{
	using namespace UeMcpLogHandlerPrivate;

	check(IsInGameThread());

	if (GRingBuffer == nullptr)
	{
		return;
	}

	if (GLog != nullptr)
	{
		GLog->RemoveOutputDevice(GRingBuffer);
		if (GCaptureDevice != nullptr)
		{
			GLog->RemoveOutputDevice(GCaptureDevice);
		}
	}

	// RemoveOutputDevice is synchronous on UE 5.7 — safe to delete now.
	delete GRingBuffer;
	GRingBuffer = nullptr;

	delete GCaptureDevice;
	GCaptureDevice = nullptr;

	// Discard any captures left open (e.g., if the editor was killed mid-test).
	{
		FScopeLock MapLock(&GCaptureMapLock);
		GCaptureSessions.Empty();
	}

	UE_LOG(LogUeMcpEditor, Verbose, TEXT("Log ring buffer and capture device detached."));
}
