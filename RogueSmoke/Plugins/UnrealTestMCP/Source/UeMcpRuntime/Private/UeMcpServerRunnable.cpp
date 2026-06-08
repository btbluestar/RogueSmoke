// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpServerRunnable.h"

#include "Common/TcpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "IPAddress.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "HAL/PlatformTime.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpToolCallLog.h"

namespace UeMcpServerRunnablePrivate
{
	/** Upper-snake error codes matching the taxonomy in 07_V0_PLAN.md §2.7. */
	static const FString ErrCodeInvalidPayload = TEXT("INVALID_PAYLOAD");

	/** Accept-poll interval. Keeps Stop() reactive without burning a core. */
	static constexpr float AcceptPollSeconds = 0.05f;

	/**
	 * Build a response that does not echo an id — used when the request
	 * could not be parsed (the id is the first thing we could not read).
	 */
	static TSharedRef<FJsonObject> MakeUnIdentifiedError(
		const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}

	static TSharedRef<FJsonObject> MakeIdentifiedError(
		const FString& Id, const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("id"), Id);
		Out->SetBoolField(TEXT("ok"), false);
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}

	/**
	 * Serialize a JSON object to a compact UTF-8 `TArray<uint8>` with a
	 * trailing newline (the framing separator).
	 */
	static TArray<uint8> SerializeToFramedUtf8(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		Out.AppendChar(TEXT('\n'));

		FTCHARToUTF8 Utf8(*Out);
		TArray<uint8> Bytes;
		Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		return Bytes;
	}

	/**
	 * Parse a UUID-looking id string into an FGuid. Accepts both the
	 * `Digits` (32-char hex) and canonical (8-4-4-4-12) encodings, as
	 * well as the Python UUID4 `str(uuid)` form.
	 */
	static bool ParseGuidLenient(const FString& In, FGuid& OutGuid)
	{
		if (FGuid::Parse(In, OutGuid))
		{
			return true;
		}
		// Strip hyphens and retry as `Digits`.
		FString Stripped = In.Replace(TEXT("-"), TEXT(""));
		return FGuid::ParseExact(Stripped, EGuidFormats::Digits, OutGuid);
	}
}

/**
 * Per-connection worker. Owns one client socket and serves it on its own
 * `FRunnableThread` until the peer disconnects or shutdown is requested.
 *
 * The worker borrows (does NOT own) its parent runnable to call back into
 * `ServeClientLoop` and the shared `bStopRequested` flag — the parent
 * outlives every worker because `RequestShutdownAndWait()` joins all
 * workers before the parent is destroyed.
 */
class FUeMcpServerRunnable::FConnectionWorker : public FRunnable
{
public:
	FConnectionWorker(FUeMcpServerRunnable& InParent,
	                  FSocket* InClientSocket,
	                  const FString& InPeerDesc)
		: Parent(InParent)
		, ClientSocket(InClientSocket)
		, PeerDesc(InPeerDesc)
	{
	}

	virtual ~FConnectionWorker() override
	{
		// The parent's join path is the only caller of the destructor and
		// it always Stop()s + WaitForCompletion()s first, so the serve loop
		// has already returned by the time we get here.
		if (Thread != nullptr)
		{
			Thread->WaitForCompletion();
			delete Thread;
			Thread = nullptr;
		}

		if (ClientSocket != nullptr)
		{
			ClientSocket->Close();
			if (ISocketSubsystem* SocketSubsystem =
					ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				SocketSubsystem->DestroySocket(ClientSocket);
			}
			ClientSocket = nullptr;
		}
	}

	FConnectionWorker(const FConnectionWorker&) = delete;
	FConnectionWorker& operator=(const FConnectionWorker&) = delete;

	bool StartThread()
	{
		Thread = FRunnableThread::Create(
			this, TEXT("UeMcpConnWorker"), 0, TPri_Normal);
		return Thread != nullptr;
	}

	/** True once the serve loop has returned (worker is reapable). */
	bool IsFinished() const { return bFinished.Load(); }

	/**
	 * Detach the client socket without closing it. Used only on the
	 * thread-create-failure path so the caller (which still owns cleanup
	 * on failure) can close+destroy it exactly once.
	 */
	void ReleaseSocket() { ClientSocket = nullptr; }

	// FRunnable
	virtual uint32 Run() override
	{
		Parent.ServeClientLoop(ClientSocket);

		// Decrement the live counter and log BEFORE flagging finished, so
		// the freed slot is visible to the cap check the instant the
		// accept thread could reap us. `bFinished` is the very last write:
		// once it is set, JoinFinishedWorkers may destroy this worker, and
		// the destructor's WaitForCompletion blocks until this function
		// returns — so all reads of `Parent` here happen-before the join.
		const int32 Live = --Parent.ConnectedClients;
		UE_LOG(LogUeMcpRuntime, Log,
			TEXT("FUeMcpServerRunnable: client %s disconnected (live=%d)"),
			*PeerDesc, Live);
		bFinished.Store(true);
		return 0;
	}

	virtual void Stop() override
	{
		// Cooperative: ServeClientLoop polls Parent.bStopRequested. Closing
		// the client socket here would race the serve loop's Recv; instead
		// we rely on the parent having already set bStopRequested, and the
		// 10ms Wait() inside ReadFramedMessages bounds the exit latency.
	}

private:
	FUeMcpServerRunnable& Parent;
	FSocket* ClientSocket = nullptr;
	FString PeerDesc;
	FRunnableThread* Thread = nullptr;
	TAtomic<bool> bFinished { false };
};

FUeMcpServerRunnable::FUeMcpServerRunnable(FUeMcpDispatcher& InDispatcher,
                                           const FString& InHost,
                                           int32 InPort,
                                           int32 InMaxMessageBytes)
	: Dispatcher(InDispatcher)
	, Host(InHost)
	, Port(InPort)
	, MaxMessageBytes(InMaxMessageBytes > 0 ? InMaxMessageBytes : (4 * 1024 * 1024))
{
}

FUeMcpServerRunnable::~FUeMcpServerRunnable()
{
	RequestShutdownAndWait();
}

bool FUeMcpServerRunnable::Start()
{
	// Bind the listen socket synchronously on the caller thread so Start()
	// can return a meaningful success/failure before any worker exists.
	// FRunnable::Init is left as a harmless no-op: when FRunnableThread
	// calls it on the worker, the socket is already up.
	if (!Init())
	{
		return false;
	}

	Thread = FRunnableThread::Create(
		this, TEXT("UeMcpServerRunnable"), 0, TPri_Normal);

	if (Thread == nullptr)
	{
		UE_LOG(LogUeMcpRuntime, Error,
			TEXT("FUeMcpServerRunnable::Start: FRunnableThread::Create failed"));
		if (ListenSocket != nullptr)
		{
			ListenSocket->Close();
			if (ISocketSubsystem* SocketSubsystem =
					ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
			{
				SocketSubsystem->DestroySocket(ListenSocket);
			}
			ListenSocket = nullptr;
		}
		return false;
	}

	return true;
}

bool FUeMcpServerRunnable::Init()
{
	// Idempotent: if Start() already bound the listen socket, skip re-bind.
	if (ListenSocket != nullptr)
	{
		return true;
	}

	using namespace UeMcpServerRunnablePrivate;

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr)
	{
		UE_LOG(LogUeMcpRuntime, Error,
			TEXT("FUeMcpServerRunnable::Init: no socket subsystem available"));
		return false;
	}

	// Loopback enforcement. We never trust the configured host string — if
	// it resolves to anything other than loopback, log a warning and pin
	// to 127.0.0.1 regardless.
	FIPv4Address BindAddress = FIPv4Address(127, 0, 0, 1);
	if (!Host.IsEmpty() && Host != TEXT("127.0.0.1") && Host != TEXT("::1"))
	{
		UE_LOG(LogUeMcpRuntime, Warning,
			TEXT("FUeMcpServerRunnable::Init: configured host '%s' is not loopback; pinning to 127.0.0.1"),
			*Host);
	}

	FIPv4Endpoint Endpoint(BindAddress, static_cast<uint16>(Port));

	ListenSocket = FTcpSocketBuilder(TEXT("UeMcpListenSocket"))
		.AsReusable()
		.AsNonBlocking()
		.BoundToEndpoint(Endpoint)
		.Listening(8)
		.WithSendBufferSize(64 * 1024)
		.WithReceiveBufferSize(64 * 1024);

	if (ListenSocket == nullptr)
	{
		UE_LOG(LogUeMcpRuntime, Error,
			TEXT("FUeMcpServerRunnable::Init: failed to bind %s"),
			*Endpoint.ToString());
		return false;
	}

	// Verify the bound endpoint actually landed on loopback. Belt-and-
	// braces against any future refactor that might let non-loopback
	// slip through the builder.
	{
		TSharedRef<FInternetAddr> BoundAddr = SocketSubsystem->CreateInternetAddr();
		ListenSocket->GetAddress(*BoundAddr);
		BoundPort = static_cast<int32>(ListenSocket->GetPortNo());

		FString BoundStr = BoundAddr->ToString(/*bAppendPort*/ false);
		if (BoundStr != TEXT("127.0.0.1") && BoundStr != TEXT("::1"))
		{
			UE_LOG(LogUeMcpRuntime, Error,
				TEXT("FUeMcpServerRunnable::Init: bound endpoint %s is not loopback; refusing to start"),
				*BoundStr);
			ListenSocket->Close();
			SocketSubsystem->DestroySocket(ListenSocket);
			ListenSocket = nullptr;
			return false;
		}

		UE_LOG(LogUeMcpRuntime, Log,
			TEXT("FUeMcpServerRunnable::Init: listening on %s:%d"),
			*BoundStr, BoundPort);
	}

	return true;
}

uint32 FUeMcpServerRunnable::Run()
{
	using namespace UeMcpServerRunnablePrivate;

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SocketSubsystem == nullptr || ListenSocket == nullptr)
	{
		return 1;
	}

	while (!bStopRequested.Load())
	{
		bool bPending = false;
		if (!ListenSocket->HasPendingConnection(bPending))
		{
			// Socket dead — likely the listen socket was closed by Stop().
			break;
		}

		if (!bPending)
		{
			FPlatformProcess::Sleep(AcceptPollSeconds);
			continue;
		}

		TSharedRef<FInternetAddr> PeerAddr = SocketSubsystem->CreateInternetAddr();
		FSocket* ClientSocket = ListenSocket->Accept(*PeerAddr, TEXT("UeMcpClientSocket"));
		if (ClientSocket == nullptr)
		{
			continue;
		}

		const FString PeerDesc = PeerAddr->ToString(/*bAppendPort*/ true);

		// Reap finished workers opportunistically so the array doesn't grow
		// for the whole editor session, and so a finished connection frees
		// a slot for the cap check below.
		JoinFinishedWorkers();

		// Connection cap (issue #57 mitigation): the v0 reject-second-client
		// policy is gone — we now serve up to MaxConnections concurrently.
		// Beyond the cap we still close immediately so the OS accept queue
		// never silently backs up into CLOSE_WAIT.
		if (ConnectedClients.Load() >= MaxConnections)
		{
			UE_LOG(LogUeMcpRuntime, Warning,
				TEXT("FUeMcpServerRunnable::Run: connection cap (%d) reached; "
				     "rejecting %s"),
				MaxConnections, *PeerDesc);
			ClientSocket->Close();
			SocketSubsystem->DestroySocket(ClientSocket);
			continue;
		}

		ClientSocket->SetNonBlocking(true);
		ClientSocket->SetNoDelay(true);

		if (!SpawnConnectionWorker(ClientSocket, PeerDesc))
		{
			// SpawnConnectionWorker logs the reason; it does not take
			// ownership on failure, so we destroy the socket here.
			ClientSocket->Close();
			SocketSubsystem->DestroySocket(ClientSocket);
			continue;
		}

		UE_LOG(LogUeMcpRuntime, Log,
			TEXT("FUeMcpServerRunnable::Run: client connected from %s (live=%d)"),
			*PeerDesc, ConnectedClients.Load());
	}

	return 0;
}

bool FUeMcpServerRunnable::SpawnConnectionWorker(
	FSocket* ClientSocket, const FString& PeerDesc)
{
	TUniquePtr<FConnectionWorker> Worker =
		MakeUnique<FConnectionWorker>(*this, ClientSocket, PeerDesc);

	// Bump the live counter BEFORE the thread starts so a burst of accepts
	// can't blow past the cap in the window before Run() begins.
	ConnectedClients++;

	if (!Worker->StartThread())
	{
		ConnectedClients--;
		UE_LOG(LogUeMcpRuntime, Error,
			TEXT("FUeMcpServerRunnable::SpawnConnectionWorker: thread create "
			     "failed for %s"),
			*PeerDesc);
		// The worker's dtor would close+destroy the socket; we don't want
		// that here (caller owns cleanup on failure), so release it.
		Worker->ReleaseSocket();
		return false;
	}

	FScopeLock Lock(&WorkersLock);
	Workers.Add(MoveTemp(Worker));
	return true;
}

void FUeMcpServerRunnable::JoinFinishedWorkers()
{
	FScopeLock Lock(&WorkersLock);
	for (int32 i = Workers.Num() - 1; i >= 0; --i)
	{
		if (Workers[i].IsValid() && Workers[i]->IsFinished())
		{
			// Destructor joins the (already-returned) thread and tears
			// down the socket.
			Workers.RemoveAt(i, EAllowShrinking::No);
		}
	}
}

void FUeMcpServerRunnable::JoinAllWorkers()
{
	TArray<TUniquePtr<FConnectionWorker>> Draining;
	{
		FScopeLock Lock(&WorkersLock);
		Draining = MoveTemp(Workers);
		Workers.Reset();
	}

	// Destroying each worker joins its thread (WaitForCompletion in the
	// FConnectionWorker dtor) and closes its socket. We do this outside
	// WorkersLock so a slow worker exit can't stall the accept thread if
	// it happens to call JoinFinishedWorkers concurrently — though by the
	// time JoinAllWorkers runs the accept thread is already joined.
	Draining.Reset();
}

void FUeMcpServerRunnable::Stop()
{
	bStopRequested.Store(true);

	// Close the listen socket so any blocked Accept / HasPendingConnection
	// unblocks immediately. Every connection worker's ServeClientLoop polls
	// bStopRequested and returns within its 10ms read-wait window, after
	// which RequestShutdownAndWait joins them.
	if (ListenSocket != nullptr)
	{
		ListenSocket->Close();
	}
}

void FUeMcpServerRunnable::Exit()
{
	// Called by the FRunnable contract after Run() returns. Nothing to do —
	// RequestShutdownAndWait owns the socket destroy path.
}

void FUeMcpServerRunnable::RequestShutdownAndWait()
{
	if (!bStopRequested.Load())
	{
		Stop();
	}

	// Join the accept thread first. Once it has returned, no NEW connection
	// worker can be spawned, so the worker set is frozen and safe to drain.
	if (Thread != nullptr)
	{
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}

	// Join + destroy every connection worker. Each worker's ServeClientLoop
	// has already seen bStopRequested (set by Stop()) and exited its loop,
	// or is about to within one 10ms read-wait; the FConnectionWorker
	// destructor blocks on WaitForCompletion so this is a hard join — no
	// leaked transport threads on editor exit.
	JoinAllWorkers();

	if (ListenSocket != nullptr)
	{
		if (ISocketSubsystem* SocketSubsystem =
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM))
		{
			SocketSubsystem->DestroySocket(ListenSocket);
		}
		ListenSocket = nullptr;
	}
}

int32 FUeMcpServerRunnable::GetConnectedClientCount() const
{
	return ConnectedClients.Load();
}

void FUeMcpServerRunnable::ServeClientLoop(FSocket* ClientSocket)
{
	using namespace UeMcpServerRunnablePrivate;

	TArray<uint8> ReadBuffer;
	ReadBuffer.Reserve(16 * 1024);

	while (!bStopRequested.Load())
	{
		TArray<FString> Messages;
		bool bHardError = false;
		const bool bAlive = ReadFramedMessages(ClientSocket, ReadBuffer, Messages, bHardError);
		if (!bAlive)
		{
			break;
		}

		if (bHardError)
		{
			// A message exceeded the size cap — we already sent an error
			// response in ReadFramedMessages; close the connection.
			break;
		}

		for (const FString& Line : Messages)
		{
			UE_LOG(LogUeMcpRuntime, Log,
				TEXT("ServeClientLoop: received message, %d chars"), Line.Len());
			TSharedRef<FJsonObject> Response = HandleMessage(Line);
			if (!SendResponse(ClientSocket, Response))
			{
				UE_LOG(LogUeMcpRuntime, Warning,
					TEXT("ServeClientLoop: send failed; dropping connection"));
				return;
			}
			UE_LOG(LogUeMcpRuntime, Log, TEXT("ServeClientLoop: response sent"));
		}
		// No extra sleep — `ReadFramedMessages` already yields via
		// `Socket->Wait` with a 10ms timeout when there is no data, so the
		// loop already paces itself and stays immediately reactive to
		// incoming bytes.
	}
}

bool FUeMcpServerRunnable::ReadFramedMessages(FSocket* Socket,
                                              TArray<uint8>& Buffer,
                                              TArray<FString>& OutMessages,
                                              bool& OutHardError)
{
	using namespace UeMcpServerRunnablePrivate;

	OutHardError = false;

	// Wait for data with a short timeout. `Wait(WaitForRead, ...)` returns
	// true when either bytes are available OR the socket transitioned to a
	// closed/error state — in both cases the subsequent `Recv` tells us
	// which via its return value.  This is the canonical UE pattern for
	// non-blocking sockets on Windows; `HasPendingData` can miss bytes
	// and can't distinguish "no data yet" from "peer closed" reliably.
	const bool bHasReadSignal = Socket->Wait(
		ESocketWaitConditions::WaitForRead,
		FTimespan::FromMilliseconds(10));

	if (bHasReadSignal)
	{
		// Try to read one chunk. Non-blocking Recv semantics on UE:
		//   bOk == false                 -> socket error (treat as closed)
		//   bOk == true, BytesRead == 0  -> orderly peer close
		//   bOk == true, BytesRead > 0   -> got data
		uint8 Scratch[8192];
		int32 BytesRead = 0;
		const bool bOk = Socket->Recv(Scratch, sizeof(Scratch), BytesRead);
		if (!bOk || BytesRead == 0)
		{
			UE_LOG(LogUeMcpRuntime, Log,
				TEXT("ReadFramedMessages: peer closed (ok=%d, bytes=%d)"),
				bOk ? 1 : 0, BytesRead);
			return false;
		}
		Buffer.Append(Scratch, BytesRead);
	}
	// else: no data and no error yet; fall through to parse whatever is
	// already in Buffer (or return with empty OutMessages so the caller
	// loops again).

	// Pop complete lines (bytes up to but not including '\n').
	while (true)
	{
		int32 NewlineIdx = INDEX_NONE;
		for (int32 i = 0; i < Buffer.Num(); ++i)
		{
			if (Buffer[i] == '\n')
			{
				NewlineIdx = i;
				break;
			}
		}

		if (NewlineIdx == INDEX_NONE)
		{
			// No complete line yet. If the buffer is bigger than our cap,
			// the sender is misbehaving — reject and drop.
			if (Buffer.Num() > MaxMessageBytes)
			{
				TSharedRef<FJsonObject> Err = MakeUnIdentifiedError(
					ErrCodeInvalidPayload,
					FString::Printf(
						TEXT("Single message exceeds max %d bytes with no line terminator"),
						MaxMessageBytes));
				SendResponse(Socket, Err);
				OutHardError = true;
				Buffer.Reset();
			}
			break;
		}

		int32 LineLen = NewlineIdx;
		// Strip optional trailing '\r' for CRLF-tolerance.
		if (LineLen > 0 && Buffer[LineLen - 1] == '\r')
		{
			LineLen -= 1;
		}

		if (LineLen > MaxMessageBytes)
		{
			TSharedRef<FJsonObject> Err = MakeUnIdentifiedError(
				ErrCodeInvalidPayload,
				FString::Printf(TEXT("Single message exceeds max %d bytes"),
					MaxMessageBytes));
			SendResponse(Socket, Err);
			OutHardError = true;
			Buffer.Reset();
			break;
		}

		{
			// Copy the UTF-8 slice into a null-terminated scratch so the
			// converter sees a clean boundary — the in-place buffer may
			// still have trailing bytes beyond this line.
			TArray<uint8> LineBytes;
			LineBytes.SetNumUninitialized(LineLen + 1);
			if (LineLen > 0)
			{
				FMemory::Memcpy(LineBytes.GetData(), Buffer.GetData(), LineLen);
			}
			LineBytes[LineLen] = 0;

			FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(LineBytes.GetData()));
			OutMessages.Add(FString(Conv.Length(), Conv.Get()));
		}

		Buffer.RemoveAt(0, NewlineIdx + 1, EAllowShrinking::No);
	}

	return true;
}

bool FUeMcpServerRunnable::SendResponse(FSocket* Socket,
                                        const TSharedRef<FJsonObject>& Response)
{
	using namespace UeMcpServerRunnablePrivate;

	TArray<uint8> Bytes = SerializeToFramedUtf8(Response);
	int32 Sent = 0;
	int32 TotalSent = 0;
	while (TotalSent < Bytes.Num())
	{
		if (!Socket->Send(Bytes.GetData() + TotalSent, Bytes.Num() - TotalSent, Sent))
		{
			return false;
		}
		if (Sent <= 0)
		{
			// Non-blocking socket reporting would-block — sleep briefly
			// and retry. A truly wedged peer will eventually trip the
			// caller's timeout at a higher layer.
			FPlatformProcess::Sleep(0.001f);
			continue;
		}
		TotalSent += Sent;
	}

	UE_LOG(LogUeMcpRuntime, Verbose,
		TEXT("FUeMcpServerRunnable::SendResponse: sent %d bytes"),
		TotalSent);
	return true;
}

TSharedRef<FJsonObject> FUeMcpServerRunnable::HandleMessage(const FString& LineJson)
{
	using namespace UeMcpServerRunnablePrivate;

	// Parse the envelope.
	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(LineJson);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogUeMcpRuntime, Warning,
			TEXT("FUeMcpServerRunnable::HandleMessage: JSON parse failed"));
		return MakeUnIdentifiedError(ErrCodeInvalidPayload,
			TEXT("JSON parse failed at envelope"));
	}

	FString IdStr;
	if (!Root->TryGetStringField(TEXT("id"), IdStr) || IdStr.IsEmpty())
	{
		return MakeUnIdentifiedError(ErrCodeInvalidPayload,
			TEXT("Missing required 'id' string field"));
	}

	FString ToolStr;
	if (!Root->TryGetStringField(TEXT("tool"), ToolStr) || ToolStr.IsEmpty())
	{
		return MakeIdentifiedError(IdStr, ErrCodeInvalidPayload,
			TEXT("Missing required 'tool' string field"));
	}

	// args is optional — default to empty object.
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
	{
		const TSharedPtr<FJsonObject>* ArgsPtr = nullptr;
		if (Root->TryGetObjectField(TEXT("args"), ArgsPtr) &&
			ArgsPtr != nullptr && ArgsPtr->IsValid())
		{
			Args = ArgsPtr->ToSharedRef();
		}
	}

	// Intercept `cancel`. Must never route to the dispatcher — it has to
	// be lock-free and immediate so it can race a long-running handler.
	if (ToolStr == TEXT("cancel"))
	{
		FString TargetIdStr;
		if (!Args->TryGetStringField(TEXT("target_id"), TargetIdStr) ||
			TargetIdStr.IsEmpty())
		{
			return MakeIdentifiedError(IdStr, ErrCodeInvalidPayload,
				TEXT("cancel requires args.target_id (string)"));
		}

		FGuid TargetGuid;
		if (!ParseGuidLenient(TargetIdStr, TargetGuid))
		{
			return MakeIdentifiedError(IdStr, ErrCodeInvalidPayload,
				TEXT("cancel: args.target_id is not a valid GUID"));
		}

		Dispatcher.Cancel(TargetGuid);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("id"), IdStr);
		Out->SetBoolField(TEXT("ok"), true);
		Out->SetObjectField(TEXT("data"), MakeShared<FJsonObject>());

		// Log the cancel tool-call so pattern analysis sees it alongside
		// regular dispatches. Duration is ~0 — cancel is fire-and-forget.
		UeMcp::LogToolCall(IdStr, TEXT("cancel"),
			FString::Printf(TEXT("{\"target_id\":\"%s\"}"), *TargetIdStr),
			/*bOk=*/ true, FString(), /*DurationMs=*/ 0.0);
		return Out;
	}

	// Normal dispatch path. The dispatcher's id serialization uses the
	// `Digits` format; we parse the wire id leniently so it accepts both
	// UUID-canonical and digits encodings. The dispatcher re-emits the
	// id in `Digits` form in its response, which may differ from what
	// the client sent — we fix that here so the response id matches the
	// request string byte-for-byte.
	FGuid RequestGuid;
	if (!ParseGuidLenient(IdStr, RequestGuid))
	{
		// Allow non-GUID id strings — if the caller doesn't use UUIDs, we
		// still dispatch; the dispatcher key is just an FGuid, so hash the
		// string into one.
		RequestGuid = FGuid::NewGuid();
	}

	// Serialise the args object for the tool-call log preview. Doing it
	// before dispatch so we capture the INPUT shape even when the handler
	// mutates Args (none do today, but the logger shouldn't depend on
	// that invariant holding forever).
	FString ArgsJsonPreview;
	{
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> ArgsWriter =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ArgsJsonPreview);
		FJsonSerializer::Serialize(Args, ArgsWriter);
	}

	const uint64 StartCycles = FPlatformTime::Cycles64();
	TSharedRef<FJsonObject> Response =
		Dispatcher.Dispatch(RequestGuid, FName(*ToolStr), Args);
	const double DurationMs =
		FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - StartCycles);

	// Overwrite the id field with the caller's original string to keep
	// round-trip correlation independent of our internal formatting.
	Response->SetStringField(TEXT("id"), IdStr);

	// Tool-call ledger. Inspect the response we're about to return so
	// the log mirrors what the caller sees.
	bool bOk = false;
	Response->TryGetBoolField(TEXT("ok"), bOk);
	FString ErrorCode;
	if (!bOk)
	{
		Response->TryGetStringField(TEXT("error"), ErrorCode);
	}
	UeMcp::LogToolCall(IdStr, ToolStr, ArgsJsonPreview, bOk, ErrorCode, DurationMs);

	return Response;
}
