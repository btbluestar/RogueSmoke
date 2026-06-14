// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"

namespace UeMcpDispatcherTestsPrivate
{
	/**
	 * Automation tests run on the game thread, but the dispatcher's ticker
	 * also runs on the game thread — so we cannot simply `Future.Get()` or
	 * we'd deadlock the ticker. Instead, we spawn the Dispatch call on a
	 * worker thread (matching the production transport pattern) and here
	 * on the game thread we manually tick the core ticker while polling
	 * the future's readiness. That lets the dispatcher's own ticker run,
	 * which is what actually drains the work into the handler.
	 */
	static TSharedRef<FJsonObject> DispatchOffThreadAndPump(
		FUeMcpDispatcher& Dispatcher,
		const FGuid& RequestId,
		const FName& ToolName,
		const TSharedRef<FJsonObject>& Args,
		double MaxWaitSeconds = 10.0)
	{
		check(IsInGameThread());

		TSharedRef<TPromise<TSharedRef<FJsonObject>>, ESPMode::ThreadSafe> Promise =
			MakeShared<TPromise<TSharedRef<FJsonObject>>, ESPMode::ThreadSafe>();
		TFuture<TSharedRef<FJsonObject>> Future = Promise->GetFuture();

		Async(EAsyncExecution::Thread,
			[&Dispatcher, RequestId, ToolName, Args, Promise]()
			{
				TSharedRef<FJsonObject> Response = Dispatcher.Dispatch(RequestId, ToolName, Args);
				Promise->SetValue(Response);
			});

		const double Deadline = FPlatformTime::Seconds() + MaxWaitSeconds;
		while (!Future.IsReady())
		{
			FTSTicker::GetCoreTicker().Tick(0.016f);
			FPlatformProcess::Sleep(0.005f);
			if (FPlatformTime::Seconds() > Deadline)
			{
				// Safety escape: return an empty error object rather than
				// blocking the automation harness forever. The test will
				// fail the subsequent assertions.
				TSharedRef<FJsonObject> Empty = MakeShared<FJsonObject>();
				Empty->SetStringField(TEXT("id"), RequestId.ToString(EGuidFormats::Digits));
				Empty->SetBoolField(TEXT("ok"), false);
				Empty->SetStringField(TEXT("error"), TEXT("TEST_HARNESS_DEADLINE"));
				Empty->SetStringField(TEXT("message"),
					TEXT("Test harness pump-loop exceeded max wait"));
				return Empty;
			}
		}

		return Future.Get();
	}

	/** Pump the core ticker for a bounded period while polling a predicate. */
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
}

/*
 * Ping test — basic happy-path round trip through dispatcher and executor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpDispatcherPingTest,
	"unreal-test-mcp.Runtime.Dispatcher.Ping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpDispatcherPingTest::RunTest(const FString& /*Parameters*/)
{
	FUeMcpDispatcher Dispatcher;

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<FJsonObject> Response = UeMcpDispatcherTestsPrivate::DispatchOffThreadAndPump(
		Dispatcher, Id, FName(TEXT("ping")), Args);

	bool bOk = false;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestTrue(TEXT("ok == true"), bOk);

	const TSharedPtr<FJsonObject>* Data = nullptr;
	TestTrue(TEXT("response has data"), Response->TryGetObjectField(TEXT("data"), Data));
	if (Data != nullptr && Data->IsValid())
	{
		bool bPong = false;
		TestTrue(TEXT("data has pong"), (*Data)->TryGetBoolField(TEXT("pong"), bPong));
		TestTrue(TEXT("pong == true"), bPong);

		double UptimeMs = -1.0;
		TestTrue(TEXT("data has server_uptime_ms"),
			(*Data)->TryGetNumberField(TEXT("server_uptime_ms"), UptimeMs));
		TestTrue(TEXT("server_uptime_ms >= 0"), UptimeMs >= 0.0);
	}

	return true;
}

/*
 * Unknown-tool test — dispatcher must return a structured UNKNOWN_TOOL
 * error, not crash or return a success. The lookup happens synchronously
 * on the transport thread, so no game-thread hop is needed; we still
 * dispatch off-thread to match the real call path.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpDispatcherUnknownToolTest,
	"unreal-test-mcp.Runtime.Dispatcher.UnknownTool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpDispatcherUnknownToolTest::RunTest(const FString& /*Parameters*/)
{
	FUeMcpDispatcher Dispatcher;

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<FJsonObject> Response = UeMcpDispatcherTestsPrivate::DispatchOffThreadAndPump(
		Dispatcher, Id, FName(TEXT("does_not_exist")), Args);

	bool bOk = true;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestFalse(TEXT("ok == false"), bOk);

	FString ErrorCode;
	TestTrue(TEXT("response has error"), Response->TryGetStringField(TEXT("error"), ErrorCode));
	TestEqual(TEXT("error code is UNKNOWN_TOOL"), ErrorCode, FString(TEXT("UNKNOWN_TOOL")));

	return true;
}

/*
 * Timeout test — handler that sleeps longer than the timeout must produce
 * a TIMEOUT error, and the executor must eventually reap the in-flight
 * entry so the dispatcher's NumInFlight() returns to zero.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpDispatcherTimeoutTest,
	"unreal-test-mcp.Runtime.Dispatcher.Timeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpDispatcherTimeoutTest::RunTest(const FString& /*Parameters*/)
{
	FUeMcpDispatcher Dispatcher;

	FUeMcpToolRegistration SlowReg;
	SlowReg.ToolName = FName(TEXT("slow_test_tool"));
	SlowReg.DefaultTimeoutSeconds = 0.5;
	SlowReg.bMutating = false;
	SlowReg.Handler.BindLambda(
		[](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& Cancel) -> TSharedRef<FJsonObject>
		{
			// Sleep for ~2s total, polling cancel so we don't wedge the
			// ticker if the handler survives past the caller's timeout.
			const double Deadline = FPlatformTime::Seconds() + 2.0;
			while (FPlatformTime::Seconds() < Deadline)
			{
				if (Cancel.IsCancellationRequested())
				{
					break;
				}
				FPlatformProcess::Sleep(0.02f);
			}
			return MakeShared<FJsonObject>();
		});
	Dispatcher.RegisterTool(SlowReg);

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<FJsonObject> Response = UeMcpDispatcherTestsPrivate::DispatchOffThreadAndPump(
		Dispatcher, Id, SlowReg.ToolName, Args, /*MaxWaitSeconds*/ 5.0);

	bool bOk = true;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestFalse(TEXT("ok == false"), bOk);

	FString ErrorCode;
	TestTrue(TEXT("response has error"), Response->TryGetStringField(TEXT("error"), ErrorCode));
	TestEqual(TEXT("error code is TIMEOUT"), ErrorCode, FString(TEXT("TIMEOUT")));

	// The caller's ExecuteOnGameThread marks the request abandoned on
	// timeout; the next ticker pass removes it from the InFlight map.
	// Pump the ticker a bit and confirm the count drained.
	const bool bDrained = UeMcpDispatcherTestsPrivate::PumpUntil(
		[&Dispatcher]() { return Dispatcher.NumInFlight() == 0; },
		/*MaxWaitSeconds*/ 3.0);
	TestTrue(TEXT("NumInFlight drained to 0"), bDrained);

	return true;
}

/*
 * Cancel test — handler polls IsCancellationRequested in a loop; after
 * 100ms we Cancel(Id) and expect a CANCELLED error.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpDispatcherCancelTest,
	"unreal-test-mcp.Runtime.Dispatcher.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpDispatcherCancelTest::RunTest(const FString& /*Parameters*/)
{
	FUeMcpDispatcher Dispatcher;

	FUeMcpToolRegistration CancelReg;
	CancelReg.ToolName = FName(TEXT("cancellable_test_tool"));
	CancelReg.DefaultTimeoutSeconds = 5.0;
	CancelReg.bMutating = false;
	CancelReg.Handler.BindLambda(
		[](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& Cancel) -> TSharedRef<FJsonObject>
		{
			const double Deadline = FPlatformTime::Seconds() + 1.0;
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
	Dispatcher.RegisterTool(CancelReg);

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<TPromise<TSharedRef<FJsonObject>>, ESPMode::ThreadSafe> Promise =
		MakeShared<TPromise<TSharedRef<FJsonObject>>, ESPMode::ThreadSafe>();
	TFuture<TSharedRef<FJsonObject>> Future = Promise->GetFuture();

	Async(EAsyncExecution::Thread,
		[&Dispatcher, Id, ToolName = CancelReg.ToolName, Args, Promise]()
		{
			TSharedRef<FJsonObject> Response = Dispatcher.Dispatch(Id, ToolName, Args);
			Promise->SetValue(Response);
		});

	// The handler runs synchronously inside the game-thread ticker, so a
	// Cancel() call from the game thread would be too late — the handler
	// would have already completed by the time we got control back. Issue
	// the Cancel from a second worker thread after a short delay so it can
	// race the handler's polling loop.
	Async(EAsyncExecution::Thread, [&Dispatcher, Id]()
	{
		FPlatformProcess::Sleep(0.1f);
		Dispatcher.Cancel(Id);
	});

	// Keep pumping the ticker (which runs the handler) until the future
	// completes or we hit a safety deadline.
	const double ResponseDeadline = FPlatformTime::Seconds() + 5.0;
	while (!Future.IsReady() && FPlatformTime::Seconds() < ResponseDeadline)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		FPlatformProcess::Sleep(0.005f);
	}

	TestTrue(TEXT("future became ready"), Future.IsReady());
	if (!Future.IsReady())
	{
		return false;
	}

	TSharedRef<FJsonObject> Response = Future.Get();

	bool bOk = true;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestFalse(TEXT("ok == false"), bOk);

	FString ErrorCode;
	TestTrue(TEXT("response has error"), Response->TryGetStringField(TEXT("error"), ErrorCode));
	TestEqual(TEXT("error code is CANCELLED"), ErrorCode, FString(TEXT("CANCELLED")));

	return true;
}

/*
 * Pending-handler happy path — step returns Continue twice, then Done.
 * The executor must invoke the step across three separate ticks (each
 * of which yields to the game thread) and publish the final payload
 * only on the tick that returned Done.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpDispatcherPendingHappyPathTest,
	"unreal-test-mcp.Runtime.Dispatcher.PendingHappyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpDispatcherPendingHappyPathTest::RunTest(const FString& /*Parameters*/)
{
	FUeMcpDispatcher Dispatcher;

	// Step invocations happen on the game thread via the core ticker; no
	// atomic needed, a shared int32 is enough. Expected = 3 (Continue,
	// Continue, Done).
	TSharedRef<int32, ESPMode::ThreadSafe> Calls =
		MakeShared<int32, ESPMode::ThreadSafe>(0);

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("pending_happy"));
	Reg.DefaultTimeoutSeconds = 5.0;
	Reg.bMutating = false;
	Reg.PendingHandler.BindLambda(
		[Calls](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/) -> FUeMcpPendingStep
		{
			return [Calls](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
			{
				const int32 N = ++(*Calls);
				if (N < 3)
				{
					return EUeMcpStep::Continue;
				}
				Out->SetNumberField(TEXT("call_count"), static_cast<double>(N));
				Out->SetBoolField(TEXT("completed"), true);
				return EUeMcpStep::Done;
			};
		});
	Dispatcher.RegisterTool(Reg);

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<FJsonObject> Response = UeMcpDispatcherTestsPrivate::DispatchOffThreadAndPump(
		Dispatcher, Id, Reg.ToolName, Args);

	bool bOk = false;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestTrue(TEXT("ok == true"), bOk);

	const TSharedPtr<FJsonObject>* Data = nullptr;
	TestTrue(TEXT("response has data"), Response->TryGetObjectField(TEXT("data"), Data));
	if (Data != nullptr && Data->IsValid())
	{
		double CallCount = 0.0;
		TestTrue(TEXT("data has call_count"),
			(*Data)->TryGetNumberField(TEXT("call_count"), CallCount));
		TestEqual(TEXT("call_count == 3"), static_cast<int32>(CallCount), 3);

		bool bCompleted = false;
		TestTrue(TEXT("data has completed"),
			(*Data)->TryGetBoolField(TEXT("completed"), bCompleted));
		TestTrue(TEXT("completed == true"), bCompleted);
	}

	return true;
}

/*
 * Pending-handler timeout path — step always returns Continue, so the
 * executor's timeout must fire and the dispatcher must surface TIMEOUT.
 * NumInFlight must drain to zero after the caller times out.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpDispatcherPendingTimeoutTest,
	"unreal-test-mcp.Runtime.Dispatcher.PendingTimeout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpDispatcherPendingTimeoutTest::RunTest(const FString& /*Parameters*/)
{
	FUeMcpDispatcher Dispatcher;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("pending_forever"));
	Reg.DefaultTimeoutSeconds = 0.4; // sub-second for fast test runs
	Reg.bMutating = false;
	Reg.PendingHandler.BindLambda(
		[](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/) -> FUeMcpPendingStep
		{
			return [](TSharedRef<FJsonObject>& /*Out*/) -> EUeMcpStep
			{
				return EUeMcpStep::Continue;
			};
		});
	Dispatcher.RegisterTool(Reg);

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<FJsonObject> Response = UeMcpDispatcherTestsPrivate::DispatchOffThreadAndPump(
		Dispatcher, Id, Reg.ToolName, Args, /*MaxWaitSeconds*/ 3.0);

	bool bOk = true;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestFalse(TEXT("ok == false"), bOk);

	FString ErrorCode;
	TestTrue(TEXT("response has error"),
		Response->TryGetStringField(TEXT("error"), ErrorCode));
	TestEqual(TEXT("error code is TIMEOUT"), ErrorCode, FString(TEXT("TIMEOUT")));

	const bool bDrained = UeMcpDispatcherTestsPrivate::PumpUntil(
		[&Dispatcher]() { return Dispatcher.NumInFlight() == 0; },
		/*MaxWaitSeconds*/ 3.0);
	TestTrue(TEXT("NumInFlight drained to 0"), bDrained);

	return true;
}

/*
 * Pending-handler Failed path — step returns Failed with an inline-error
 * payload. The dispatcher must hoist `error` / `message` into the
 * top-level error shape identical to the synchronous hoist path.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpDispatcherPendingFailedTest,
	"unreal-test-mcp.Runtime.Dispatcher.PendingFailed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpDispatcherPendingFailedTest::RunTest(const FString& /*Parameters*/)
{
	FUeMcpDispatcher Dispatcher;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("pending_failed"));
	Reg.DefaultTimeoutSeconds = 5.0;
	Reg.bMutating = false;
	Reg.PendingHandler.BindLambda(
		[](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/) -> FUeMcpPendingStep
		{
			return [](TSharedRef<FJsonObject>& Out) -> EUeMcpStep
			{
				Out->SetStringField(TEXT("error"), TEXT("TEST_FAIL_CODE"));
				Out->SetStringField(TEXT("message"), TEXT("intentional test failure"));
				return EUeMcpStep::Failed;
			};
		});
	Dispatcher.RegisterTool(Reg);

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<FJsonObject> Response = UeMcpDispatcherTestsPrivate::DispatchOffThreadAndPump(
		Dispatcher, Id, Reg.ToolName, Args);

	bool bOk = true;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestFalse(TEXT("ok == false"), bOk);

	FString ErrorCode;
	TestTrue(TEXT("response has error"),
		Response->TryGetStringField(TEXT("error"), ErrorCode));
	TestEqual(TEXT("error code hoisted"), ErrorCode, FString(TEXT("TEST_FAIL_CODE")));

	FString Message;
	TestTrue(TEXT("response has message"),
		Response->TryGetStringField(TEXT("message"), Message));
	TestEqual(TEXT("message hoisted"), Message, FString(TEXT("intentional test failure")));

	return true;
}

/*
 * Pending-handler cancel path — step always returns Continue, but the
 * caller issues Cancel from a worker thread mid-flight. The dispatcher
 * must surface CANCELLED (not TIMEOUT) and NumInFlight must drain.
 *
 * Mirrors the sync FUeMcpDispatcherCancelTest: Dispatch on a worker
 * thread, pump the ticker on the game thread, fire Cancel from a third
 * thread after a short delay so it races the running step.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpDispatcherPendingCancelTest,
	"unreal-test-mcp.Runtime.Dispatcher.PendingCancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpDispatcherPendingCancelTest::RunTest(const FString& /*Parameters*/)
{
	FUeMcpDispatcher Dispatcher;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("pending_cancellable"));
	Reg.DefaultTimeoutSeconds = 5.0;
	Reg.bMutating = false;
	Reg.PendingHandler.BindLambda(
		[](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/) -> FUeMcpPendingStep
		{
			return [](TSharedRef<FJsonObject>& /*Out*/) -> EUeMcpStep
			{
				return EUeMcpStep::Continue;
			};
		});
	Dispatcher.RegisterTool(Reg);

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<TPromise<TSharedRef<FJsonObject>>, ESPMode::ThreadSafe> Promise =
		MakeShared<TPromise<TSharedRef<FJsonObject>>, ESPMode::ThreadSafe>();
	TFuture<TSharedRef<FJsonObject>> Future = Promise->GetFuture();

	Async(EAsyncExecution::Thread,
		[&Dispatcher, Id, ToolName = Reg.ToolName, Args, Promise]()
		{
			TSharedRef<FJsonObject> Response = Dispatcher.Dispatch(Id, ToolName, Args);
			Promise->SetValue(Response);
		});

	// Cancel from a separate thread after a brief delay so the step has
	// run at least once before the cancel observation.
	Async(EAsyncExecution::Thread, [&Dispatcher, Id]()
	{
		FPlatformProcess::Sleep(0.1f);
		Dispatcher.Cancel(Id);
	});

	const double ResponseDeadline = FPlatformTime::Seconds() + 5.0;
	while (!Future.IsReady() && FPlatformTime::Seconds() < ResponseDeadline)
	{
		FTSTicker::GetCoreTicker().Tick(0.016f);
		FPlatformProcess::Sleep(0.005f);
	}

	TestTrue(TEXT("future became ready"), Future.IsReady());
	if (!Future.IsReady())
	{
		return false;
	}

	TSharedRef<FJsonObject> Response = Future.Get();

	bool bOk = true;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestFalse(TEXT("ok == false"), bOk);

	FString ErrorCode;
	TestTrue(TEXT("response has error"),
		Response->TryGetStringField(TEXT("error"), ErrorCode));
	TestEqual(TEXT("error code is CANCELLED"), ErrorCode, FString(TEXT("CANCELLED")));

	const bool bDrained = UeMcpDispatcherTestsPrivate::PumpUntil(
		[&Dispatcher]() { return Dispatcher.NumInFlight() == 0; },
		/*MaxWaitSeconds*/ 3.0);
	TestTrue(TEXT("NumInFlight drained to 0"), bDrained);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
