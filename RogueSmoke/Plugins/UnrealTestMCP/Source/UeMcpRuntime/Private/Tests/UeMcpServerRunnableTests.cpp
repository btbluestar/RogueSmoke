// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "Common/TcpSocketBuilder.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "IPAddress.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include "UeMcpDispatcher.h"
#include "UeMcpServerRunnable.h"

namespace UeMcpServerRunnableTestsPrivate
{
	/** Ephemeral test port — chosen high enough to avoid common collisions. */
	static constexpr int32 TestPort = 55598;

	/** Hard test wall-clock budget: never block automation longer than this. */
	static constexpr double TestDeadlineSeconds = 6.0;

	/**
	 * Pump the core ticker on the game thread until the predicate returns
	 * true or we hit the deadline. Returns whether the predicate succeeded.
	 */
	template <typename Predicate>
	static bool PumpUntil(Predicate P, double MaxWaitSeconds)
	{
		check(IsInGameThread());
		const double Deadline = FPlatformTime::Seconds() + MaxWaitSeconds;
		while (!P())
		{
			if (FPlatformTime::Seconds() > Deadline)
			{
				return false;
			}
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FPlatformProcess::Sleep(0.005f);
		}
		return true;
	}

	/** Connect a loopback TCP client to the given port. Returns null on failure. */
	static FSocket* ConnectLoopback(int32 Port)
	{
		ISocketSubsystem* Sub = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (Sub == nullptr)
		{
			return nullptr;
		}

		FSocket* Client = FTcpSocketBuilder(TEXT("UeMcpTestClient"))
			.AsBlocking()
			.AsReusable();
		if (Client == nullptr)
		{
			return nullptr;
		}

		TSharedRef<FInternetAddr> Addr = Sub->CreateInternetAddr();
		bool bValid = false;
		Addr->SetIp(TEXT("127.0.0.1"), bValid);
		Addr->SetPort(Port);

		if (!bValid || !Client->Connect(*Addr))
		{
			Client->Close();
			Sub->DestroySocket(Client);
			return nullptr;
		}

		return Client;
	}

	/** Send a single UTF-8 line (caller supplies trailing newline). */
	static bool SendLine(FSocket* Socket, const FString& Line)
	{
		FTCHARToUTF8 Utf8(*Line);
		int32 TotalSent = 0;
		const int32 Len = Utf8.Length();
		while (TotalSent < Len)
		{
			int32 Sent = 0;
			if (!Socket->Send(reinterpret_cast<const uint8*>(Utf8.Get()) + TotalSent,
				Len - TotalSent, Sent))
			{
				return false;
			}
			if (Sent <= 0)
			{
				return false;
			}
			TotalSent += Sent;
		}
		return true;
	}

	/**
	 * Block reading until a complete newline-terminated line is received
	 * or the deadline fires. Returns empty string on error / timeout.
	 */
	static FString ReadLineBlocking(FSocket* Socket, double MaxWaitSeconds)
	{
		TArray<uint8> Buffer;
		const double Deadline = FPlatformTime::Seconds() + MaxWaitSeconds;

		while (FPlatformTime::Seconds() < Deadline)
		{
			uint32 Pending = 0;
			if (Socket->HasPendingData(Pending) && Pending > 0)
			{
				TArray<uint8> Chunk;
				Chunk.SetNumUninitialized(static_cast<int32>(Pending));
				int32 Read = 0;
				if (!Socket->Recv(Chunk.GetData(), Chunk.Num(), Read) || Read <= 0)
				{
					return FString();
				}
				Buffer.Append(Chunk.GetData(), Read);

				for (int32 i = 0; i < Buffer.Num(); ++i)
				{
					if (Buffer[i] == '\n')
					{
						int32 LineLen = i;
						if (LineLen > 0 && Buffer[LineLen - 1] == '\r')
						{
							LineLen -= 1;
						}
						TArray<uint8> LineBytes;
						LineBytes.SetNumUninitialized(LineLen + 1);
						if (LineLen > 0)
						{
							FMemory::Memcpy(LineBytes.GetData(), Buffer.GetData(), LineLen);
						}
						LineBytes[LineLen] = 0;
						FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(LineBytes.GetData()));
						return FString(Conv.Length(), Conv.Get());
					}
				}
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return FString();
	}

	/** Helper: build a request line for `{id, tool, args}`. */
	static FString BuildRequestLine(
		const FString& Id, const FString& Tool, const TSharedRef<FJsonObject>& Args)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("id"), Id);
		Obj->SetStringField(TEXT("tool"), Tool);
		Obj->SetObjectField(TEXT("args"), Args);

		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		Out.AppendChar(TEXT('\n'));
		return Out;
	}

	/** Helper: parse a response line into a JSON object. */
	static TSharedPtr<FJsonObject> ParseResponse(const FString& Line)
	{
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Line);
		if (!FJsonSerializer::Deserialize(Reader, Root))
		{
			return nullptr;
		}
		return Root;
	}

	/** RAII wrapper around a server runnable bound to TestPort. */
	struct FTestServer
	{
		FUeMcpDispatcher Dispatcher;
		TUniquePtr<FUeMcpServerRunnable> Server;

		bool Start(int32 Port)
		{
			Server = MakeUnique<FUeMcpServerRunnable>(
				Dispatcher, TEXT("127.0.0.1"), Port, /*MaxMessageBytes*/ 128 * 1024);
			return Server->Start();
		}

		void Stop()
		{
			if (Server.IsValid())
			{
				Server->RequestShutdownAndWait();
				Server.Reset();
			}
		}

		~FTestServer() { Stop(); }
	};
}

/*
 * Ping round-trip — send a valid `{id, tool: "ping", args: {}}` request,
 * assert the response has `ok == true` and `data.pong == true`.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpServerRunnablePingRoundtripTest,
	"unreal-test-mcp.Runtime.ServerRunnable.PingRoundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpServerRunnablePingRoundtripTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpServerRunnableTestsPrivate;

	FTestServer Harness;
	if (!TestTrue(TEXT("server starts"), Harness.Start(TestPort)))
	{
		return false;
	}

	// Small settle so Accept() is already parked.
	FPlatformProcess::Sleep(0.05f);

	FSocket* Client = ConnectLoopback(TestPort);
	if (!TestNotNull(TEXT("client connected"), Client))
	{
		return false;
	}

	const FString ReqId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TSharedRef<FJsonObject> EmptyArgs = MakeShared<FJsonObject>();
	const FString ReqLine = BuildRequestLine(ReqId, TEXT("ping"), EmptyArgs);

	TestTrue(TEXT("send succeeded"), SendLine(Client, ReqLine));

	// Pump the ticker while the server processes the request; the test
	// harness runs on the game thread and the dispatcher's executor also
	// runs on the game thread via its own ticker.
	TSharedPtr<FJsonObject> Response;
	PumpUntil([&]()
	{
		const FString Line = ReadLineBlocking(Client, /*MaxWaitSeconds*/ 0.05);
		if (!Line.IsEmpty())
		{
			Response = ParseResponse(Line);
			return true;
		}
		return false;
	}, TestDeadlineSeconds);

	if (!TestTrue(TEXT("response received"), Response.IsValid()))
	{
		Client->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
		return false;
	}

	bool bOk = false;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestTrue(TEXT("ok == true"), bOk);

	FString EchoedId;
	TestTrue(TEXT("response echoes id"), Response->TryGetStringField(TEXT("id"), EchoedId));
	TestEqual(TEXT("id matches"), EchoedId, ReqId);

	const TSharedPtr<FJsonObject>* Data = nullptr;
	TestTrue(TEXT("response has data"), Response->TryGetObjectField(TEXT("data"), Data));
	if (Data != nullptr && Data->IsValid())
	{
		bool bPong = false;
		TestTrue(TEXT("data has pong"), (*Data)->TryGetBoolField(TEXT("pong"), bPong));
		TestTrue(TEXT("pong == true"), bPong);
	}

	Client->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
	return true;
}

/*
 * Unknown tool — server must return structured UNKNOWN_TOOL.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpServerRunnableUnknownToolTest,
	"unreal-test-mcp.Runtime.ServerRunnable.UnknownTool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpServerRunnableUnknownToolTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpServerRunnableTestsPrivate;

	FTestServer Harness;
	if (!TestTrue(TEXT("server starts"), Harness.Start(TestPort)))
	{
		return false;
	}

	FPlatformProcess::Sleep(0.05f);

	FSocket* Client = ConnectLoopback(TestPort);
	if (!TestNotNull(TEXT("client connected"), Client))
	{
		return false;
	}

	const FString ReqId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TSharedRef<FJsonObject> EmptyArgs = MakeShared<FJsonObject>();
	const FString ReqLine = BuildRequestLine(ReqId, TEXT("does_not_exist"), EmptyArgs);

	TestTrue(TEXT("send succeeded"), SendLine(Client, ReqLine));

	TSharedPtr<FJsonObject> Response;
	PumpUntil([&]()
	{
		const FString Line = ReadLineBlocking(Client, /*MaxWaitSeconds*/ 0.05);
		if (!Line.IsEmpty())
		{
			Response = ParseResponse(Line);
			return true;
		}
		return false;
	}, TestDeadlineSeconds);

	TestTrue(TEXT("response received"), Response.IsValid());
	if (Response.IsValid())
	{
		bool bOk = true;
		TestTrue(TEXT("has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
		TestFalse(TEXT("ok == false"), bOk);

		FString ErrorCode;
		TestTrue(TEXT("has error"), Response->TryGetStringField(TEXT("error"), ErrorCode));
		TestEqual(TEXT("error is UNKNOWN_TOOL"), ErrorCode, FString(TEXT("UNKNOWN_TOOL")));
	}

	Client->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
	return true;
}

/*
 * Cancel round-trip — register a handler that loops on IsCancellationRequested,
 * send a request, then send a `cancel` message ~100ms later. Assert the
 * handler's response is CANCELLED and the cancel response is ok=true.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpServerRunnableCancelTest,
	"unreal-test-mcp.Runtime.ServerRunnable.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpServerRunnableCancelTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpServerRunnableTestsPrivate;

	FTestServer Harness;

	// Register a slow tool that polls cancellation.
	FUeMcpToolRegistration SlowReg;
	SlowReg.ToolName = FName(TEXT("test_cancellable"));
	SlowReg.DefaultTimeoutSeconds = 5.0;
	SlowReg.bMutating = false;
	SlowReg.Handler.BindLambda(
		[](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& Cancel) -> TSharedRef<FJsonObject>
		{
			const double Deadline = FPlatformTime::Seconds() + 2.0;
			while (FPlatformTime::Seconds() < Deadline)
			{
				if (Cancel.IsCancellationRequested())
				{
					break;
				}
				FPlatformProcess::Sleep(0.01f);
			}
			return MakeShared<FJsonObject>();
		});
	Harness.Dispatcher.RegisterTool(SlowReg);

	if (!TestTrue(TEXT("server starts"), Harness.Start(TestPort)))
	{
		return false;
	}
	FPlatformProcess::Sleep(0.05f);

	FSocket* Client = ConnectLoopback(TestPort);
	if (!TestNotNull(TEXT("client connected"), Client))
	{
		return false;
	}

	// The target id uses a stable FGuid so the server can parse it and
	// cancel the matching in-flight request.
	const FGuid TargetGuid = FGuid::NewGuid();
	const FString TargetIdStr = TargetGuid.ToString(EGuidFormats::Digits);

	TSharedRef<FJsonObject> EmptyArgs = MakeShared<FJsonObject>();
	const FString SlowLine = BuildRequestLine(TargetIdStr, TEXT("test_cancellable"), EmptyArgs);
	TestTrue(TEXT("slow send"), SendLine(Client, SlowLine));

	// Small delay, then send cancel from the same client.
	FPlatformProcess::Sleep(0.1f);

	const FString CancelId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TSharedRef<FJsonObject> CancelArgs = MakeShared<FJsonObject>();
	CancelArgs->SetStringField(TEXT("target_id"), TargetIdStr);
	const FString CancelLine = BuildRequestLine(CancelId, TEXT("cancel"), CancelArgs);
	TestTrue(TEXT("cancel send"), SendLine(Client, CancelLine));

	// We may receive the slow-tool response and the cancel response in
	// either order. Read until we've seen both ids or the deadline fires.
	TSharedPtr<FJsonObject> SlowResp;
	TSharedPtr<FJsonObject> CancelResp;

	const double Deadline = FPlatformTime::Seconds() + TestDeadlineSeconds;
	while ((!SlowResp.IsValid() || !CancelResp.IsValid()) &&
		FPlatformTime::Seconds() < Deadline)
	{
		const FString Line = ReadLineBlocking(Client, /*MaxWaitSeconds*/ 0.05);
		if (Line.IsEmpty())
		{
			FTSTicker::GetCoreTicker().Tick(0.016f);
			continue;
		}
		TSharedPtr<FJsonObject> Parsed = ParseResponse(Line);
		if (!Parsed.IsValid())
		{
			continue;
		}

		FString Id;
		Parsed->TryGetStringField(TEXT("id"), Id);
		if (Id == TargetIdStr)
		{
			SlowResp = Parsed;
		}
		else if (Id == CancelId)
		{
			CancelResp = Parsed;
		}
	}

	TestTrue(TEXT("slow response received"), SlowResp.IsValid());
	TestTrue(TEXT("cancel response received"), CancelResp.IsValid());

	if (SlowResp.IsValid())
	{
		bool bOk = true;
		SlowResp->TryGetBoolField(TEXT("ok"), bOk);
		TestFalse(TEXT("slow ok == false"), bOk);

		FString ErrorCode;
		SlowResp->TryGetStringField(TEXT("error"), ErrorCode);
		TestEqual(TEXT("slow error == CANCELLED"), ErrorCode, FString(TEXT("CANCELLED")));
	}

	if (CancelResp.IsValid())
	{
		bool bOk = false;
		CancelResp->TryGetBoolField(TEXT("ok"), bOk);
		TestTrue(TEXT("cancel ok == true"), bOk);
	}

	Client->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Client);
	return true;
}

/*
 * Issue #57 regression — concurrent connections.
 *
 * Reproduces the bug deterministically with no editor / bridge: hold a
 * long-lived idle "keepalive" connection open (mirrors the Python
 * bridge's 15s-ping socket) AND open a second connection that issues a
 * `ping`. Pre-fix, Run()'s single-connection serve loop blocked on the
 * keepalive forever, the second connect was never accept()ed, and the
 * request never got a response. Post-fix (per-connection workers), the
 * second connection is accepted and answered while the first stays open.
 *
 * Assertions:
 *   - second connection's ping gets ok=true while keepalive is open
 *   - both connections counted as connected at the same time
 *   - a SECOND request on the second connection also answers (proves the
 *     keepalive isn't merely tolerated for one round)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpServerRunnableConcurrentConnectionsTest,
	"unreal-test-mcp.Runtime.ServerRunnable.ConcurrentConnections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpServerRunnableConcurrentConnectionsTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpServerRunnableTestsPrivate;

	FTestServer Harness;
	if (!TestTrue(TEXT("server starts"), Harness.Start(TestPort)))
	{
		return false;
	}

	// Settle so the accept loop is parked on Accept().
	FPlatformProcess::Sleep(0.05f);

	// 1) Open the keepalive connection and DO NOT send anything on it.
	//    This is the socket that, pre-fix, monopolized the serve loop.
	FSocket* Keepalive = ConnectLoopback(TestPort);
	if (!TestNotNull(TEXT("keepalive connected"), Keepalive))
	{
		return false;
	}

	// Pump a few ticks so the accept thread has handed the keepalive to a
	// connection worker (ConnectedClients should reach 1).
	PumpUntil([&]()
	{
		return Harness.Server->GetConnectedClientCount() >= 1;
	}, TestDeadlineSeconds);
	TestEqual(TEXT("keepalive counted as 1 connection"),
		Harness.Server->GetConnectedClientCount(), 1);

	// 2) Open a SECOND connection while the keepalive is still open + idle,
	//    and issue a request on it.
	FSocket* Worker = ConnectLoopback(TestPort);
	if (!TestNotNull(TEXT("second client connected"), Worker))
	{
		Keepalive->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Keepalive);
		return false;
	}

	const FString ReqId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TSharedRef<FJsonObject> EmptyArgs = MakeShared<FJsonObject>();
	TestTrue(TEXT("send #1 succeeded"),
		SendLine(Worker, BuildRequestLine(ReqId, TEXT("ping"), EmptyArgs)));

	// Both connections must be live simultaneously — the core of the #57
	// regression. (Check before reading the response so we assert on the
	// concurrent state, not a post-completion snapshot.)
	PumpUntil([&]()
	{
		return Harness.Server->GetConnectedClientCount() >= 2;
	}, TestDeadlineSeconds);
	TestEqual(TEXT("keepalive + second both connected"),
		Harness.Server->GetConnectedClientCount(), 2);

	TSharedPtr<FJsonObject> Response;
	PumpUntil([&]()
	{
		const FString Line = ReadLineBlocking(Worker, /*MaxWaitSeconds*/ 0.05);
		if (!Line.IsEmpty())
		{
			Response = ParseResponse(Line);
			return true;
		}
		return false;
	}, TestDeadlineSeconds);

	TestTrue(TEXT("second connection got a response while keepalive open"),
		Response.IsValid());
	if (Response.IsValid())
	{
		bool bOk = false;
		Response->TryGetBoolField(TEXT("ok"), bOk);
		TestTrue(TEXT("ping #1 ok == true"), bOk);

		FString EchoedId;
		Response->TryGetStringField(TEXT("id"), EchoedId);
		TestEqual(TEXT("response #1 id matches"), EchoedId, ReqId);
	}

	// 3) A second request on the same worker connection must also answer —
	//    proves the keepalive isn't merely tolerated for a single round
	//    and that the keepalive is still parked, not consuming the worker.
	const FString ReqId2 = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	TestTrue(TEXT("send #2 succeeded"),
		SendLine(Worker, BuildRequestLine(ReqId2, TEXT("ping"), EmptyArgs)));

	TSharedPtr<FJsonObject> Response2;
	PumpUntil([&]()
	{
		const FString Line = ReadLineBlocking(Worker, /*MaxWaitSeconds*/ 0.05);
		if (!Line.IsEmpty())
		{
			Response2 = ParseResponse(Line);
			return true;
		}
		return false;
	}, TestDeadlineSeconds);

	TestTrue(TEXT("second request answered too"), Response2.IsValid());
	if (Response2.IsValid())
	{
		bool bOk2 = false;
		Response2->TryGetBoolField(TEXT("ok"), bOk2);
		TestTrue(TEXT("ping #2 ok == true"), bOk2);

		FString EchoedId2;
		Response2->TryGetStringField(TEXT("id"), EchoedId2);
		TestEqual(TEXT("response #2 id matches"), EchoedId2, ReqId2);
	}

	// Keepalive still open the whole time — close both. Closing the
	// keepalive (which never sent a byte) also exercises the worker's
	// orderly-peer-close path.
	Worker->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Worker);
	Keepalive->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Keepalive);

	// Drain the workers via shutdown — this also exercises the
	// join-all-workers path with a still-"connected" keepalive (its peer
	// just FIN'd) plus a finished worker, which is the highest-risk
	// shutdown ordering. Pump a few ticks first so the FINs are observed.
	PumpUntil([&]() { return false; }, 0.1);
	Harness.Stop();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
