// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "Templates/UniquePtr.h"

class FJsonObject;
class FRunnableThread;
class FSocket;
class FUeMcpDispatcher;
class ISocketSubsystem;

/**
 * TCP listener + per-connection JSON-line reader that drives an
 * `FUeMcpDispatcher`.
 *
 * Lives in the runtime module because it is pure networking and JSON —
 * no editor dependencies. The owning editor subsystem hands the
 * runnable a reference to its dispatcher; the runnable uses the
 * dispatcher for every non-meta tool call (the `cancel` tool is
 * intercepted here because it must never block on a handler).
 *
 * Lifecycle:
 *   - `Init()` binds the listen socket to `127.0.0.1:<Port>` (loopback is
 *     enforced regardless of what `Host` resolves to).
 *   - `Run()` loops accepting connections. Each accepted client socket is
 *     handed to a per-connection worker thread running `ServeClientLoop`;
 *     `Run()` returns immediately to `Accept()`. Up to `MaxConnections`
 *     (8) clients are served concurrently — the Python bridge keeps one
 *     keepalive socket open indefinitely (15 s pings) AND opens a fresh
 *     per-request socket; the old single-connection serve loop starved
 *     `Accept()` for the entire lifetime of the keepalive (issue #57).
 *     Excess connections beyond the cap are closed immediately so the OS
 *     accept queue never silently backs up.
 *   - `Stop()` / `RequestShutdownAndWait()` closes the listener, signals
 *     every worker to drop its connection, and joins ALL worker threads
 *     (accept thread + every connection worker) before returning — no
 *     leaked threads on editor exit.
 *
 * Framing: newline-delimited JSON, UTF-8, in both directions. Each
 * complete line must parse as a JSON object. Incremental reads are
 * buffered — we do not assume a single `recv` is a whole message.
 * Messages exceeding `MaxMessageBytes` are rejected with `INVALID_PAYLOAD`
 * and the connection is reset.
 */
class UEMCPRUNTIME_API FUeMcpServerRunnable : public FRunnable
{
public:
	/**
	 * Construct but do not start the runnable.
	 *
	 * @param InDispatcher  Dispatcher used for every non-meta tool call.
	 *                      Must outlive the runnable.
	 * @param InHost        Host name requested by config. If it resolves to
	 *                      anything other than 127.0.0.1 / ::1 we log a
	 *                      warning and pin to 127.0.0.1 anyway.
	 * @param InPort        TCP port to bind.
	 * @param InMaxMessageBytes  Maximum size of a single framed JSON line.
	 *                           Requests larger than this are rejected with
	 *                           `INVALID_PAYLOAD` and the socket is reset.
	 */
	FUeMcpServerRunnable(FUeMcpDispatcher& InDispatcher,
	                     const FString& InHost,
	                     int32 InPort,
	                     int32 InMaxMessageBytes);

	virtual ~FUeMcpServerRunnable();

	FUeMcpServerRunnable(const FUeMcpServerRunnable&) = delete;
	FUeMcpServerRunnable& operator=(const FUeMcpServerRunnable&) = delete;

	/**
	 * Start the worker thread. Returns true on success; false if the socket
	 * could not be bound. Safe to call once.
	 */
	bool Start();

	// FRunnable
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

	/**
	 * Cooperatively tear the listener down and block until the worker
	 * thread has exited. Safe to call from any thread other than the
	 * worker itself. Idempotent.
	 */
	void RequestShutdownAndWait();

	/**
	 * Diagnostic accessor — number of connection workers currently serving
	 * a client. 0..`MaxConnections`. Used by the multi-connection unit test.
	 */
	int32 GetConnectedClientCount() const;

	/** Hard cap on concurrently-served client connections. */
	static constexpr int32 MaxConnections = 8;

	/** Returns the actual bound port (useful when InPort == 0 for ephemeral). */
	int32 GetBoundPort() const { return BoundPort; }

private:
	FUeMcpDispatcher& Dispatcher;
	FString Host;
	int32 Port;
	int32 MaxMessageBytes;
	int32 BoundPort = 0;

	TAtomic<bool> bStopRequested { false };

	/**
	 * Live connection-worker count. Incremented when a worker is spawned,
	 * decremented when it finishes. A counter, not a 0/1 gate — the v0
	 * reject-second-client policy is gone (issue #57). Used both to enforce
	 * `MaxConnections` and as a diagnostic accessor.
	 */
	TAtomic<int32> ConnectedClients { 0 };

	FSocket* ListenSocket = nullptr;

	/** The accept thread (runs `Run`). */
	FRunnableThread* Thread = nullptr;

	/**
	 * One runnable per accepted client. Each owns its client socket and
	 * runs `ServeClientLoop` on its own `FRunnableThread`. The accept
	 * thread appends here; `JoinFinishedWorkers` reaps completed ones and
	 * `JoinAllWorkers` drains the rest at shutdown. Guarded by
	 * `WorkersLock` — touched from the accept thread and (at shutdown)
	 * the thread calling `RequestShutdownAndWait`.
	 */
	class FConnectionWorker;
	TArray<TUniquePtr<FConnectionWorker>> Workers;
	mutable FCriticalSection WorkersLock;

	/**
	 * Spawn a per-connection worker for `ClientSocket`. Takes ownership of
	 * the socket. Returns false (and the caller closes the socket) if the
	 * connection cap is hit or the worker thread can't be created.
	 */
	bool SpawnConnectionWorker(FSocket* ClientSocket, const FString& PeerDesc);

	/** Reap + destroy any workers whose serve loop has exited. Cheap; called
	 *  from the accept loop so finished connections don't accumulate. */
	void JoinFinishedWorkers();

	/** Join + destroy every worker. Called from the shutdown path only. */
	void JoinAllWorkers();

	/**
	 * Serve a single client until it disconnects or we are stopping. Runs
	 * on a per-connection worker thread; many may run concurrently. The
	 * dispatcher is documented safe for concurrent `Dispatch` from multiple
	 * transport threads (registry is `FCriticalSection`-guarded; handler
	 * bodies are serialized onto the single game thread by the executor).
	 */
	void ServeClientLoop(FSocket* ClientSocket);

	/**
	 * Read whatever is available on the socket, append to `Buffer`, and
	 * split complete newline-terminated lines into `OutMessages`. Returns
	 * false if the socket is dead or closed by the peer.
	 *
	 * A line exceeding `MaxMessageBytes` triggers a protocol error: we
	 * emit an `INVALID_PAYLOAD` response and tell the caller to drop the
	 * connection (return value stays true but `OutHardError` becomes true).
	 */
	bool ReadFramedMessages(FSocket* Socket,
	                        TArray<uint8>& Buffer,
	                        TArray<FString>& OutMessages,
	                        bool& OutHardError);

	/**
	 * Serialize `Response` to UTF-8 with a trailing `\n` and send it on
	 * `Socket`. Returns false on any send failure.
	 */
	bool SendResponse(FSocket* Socket, const TSharedRef<FJsonObject>& Response);

	/**
	 * Parse a JSON envelope and route it to either the dispatcher or the
	 * intercepted `cancel` tool. Returns the response object to send.
	 * Never throws.
	 */
	TSharedRef<FJsonObject> HandleMessage(const FString& LineJson);
};
