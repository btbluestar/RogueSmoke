// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"

#include "UeMcpDispatcher.h"
#include "UeMcpTestHandlers.h"

namespace UeMcpTestHandlersTestsPrivate
{
	/**
	 * Mirror of the dispatcher-tests pump helper: dispatch on a worker
	 * thread and tick the core ticker on the game thread while waiting.
	 * The `tests.list` handler spins the core ticker too, so we must
	 * make sure its ticker drains from this helper the same way the
	 * production transport would.
	 */
	static TSharedRef<FJsonObject> DispatchOffThreadAndPump(
		FUeMcpDispatcher& Dispatcher,
		const FGuid& RequestId,
		const FName& ToolName,
		const TSharedRef<FJsonObject>& Args,
		double MaxWaitSeconds = 15.0)
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
				TSharedRef<FJsonObject> Empty = MakeShared<FJsonObject>();
				Empty->SetStringField(TEXT("id"), RequestId.ToString(EGuidFormats::Digits));
				Empty->SetBoolField(TEXT("ok"), false);
				Empty->SetStringField(TEXT("error"), TEXT("TEST_HARNESS_DEADLINE"));
				return Empty;
			}
		}
		return Future.Get();
	}
}

/*
 * `tests.list` shape test — does not assert on specific test names,
 * because the set of registered engine tests varies across editor
 * configurations. We only validate:
 *   - The handler registers cleanly on a fresh dispatcher.
 *   - The response carries `ok: true`.
 *   - `data.tests` is an array.
 *   - `data.total_matched` is a number.
 *   - `data.controller_state` is a string.
 *   - `data.truncated` is a bool.
 *
 * A richer L2 roundtrip test is layered on the Python harness by Agent C.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUeMcpTestsListReturnsShapeTest,
	"unreal-test-mcp.Editor.TestHandlers.TestsListShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUeMcpTestsListReturnsShapeTest::RunTest(const FString& /*Parameters*/)
{
	using namespace UeMcpTestHandlersTestsPrivate;

	FUeMcpDispatcher Dispatcher;
	UeMcp::RegisterTestHandlers(Dispatcher);

	TestTrue(TEXT("tests.list is registered"),
		Dispatcher.HasTool(FName(TEXT("tests.list"))));
	TestTrue(TEXT("tests.refresh is registered"),
		Dispatcher.HasTool(FName(TEXT("tests.refresh"))));

	const FGuid Id = FGuid::NewGuid();
	TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();

	TSharedRef<FJsonObject> Response =
		DispatchOffThreadAndPump(Dispatcher, Id, FName(TEXT("tests.list")), Args);

	bool bOk = false;
	TestTrue(TEXT("response has ok"), Response->TryGetBoolField(TEXT("ok"), bOk));
	TestTrue(TEXT("ok == true"), bOk);

	const TSharedPtr<FJsonObject>* Data = nullptr;
	TestTrue(TEXT("response has data object"),
		Response->TryGetObjectField(TEXT("data"), Data));
	if (Data == nullptr || !Data->IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Tests = nullptr;
	TestTrue(TEXT("data.tests is an array"),
		(*Data)->TryGetArrayField(TEXT("tests"), Tests));

	double TotalMatched = -1.0;
	TestTrue(TEXT("data.total_matched is a number"),
		(*Data)->TryGetNumberField(TEXT("total_matched"), TotalMatched));
	TestTrue(TEXT("total_matched is non-negative"), TotalMatched >= 0.0);

	bool bTruncated = true;
	TestTrue(TEXT("data.truncated is a bool"),
		(*Data)->TryGetBoolField(TEXT("truncated"), bTruncated));

	FString ControllerState;
	TestTrue(TEXT("data.controller_state is a string"),
		(*Data)->TryGetStringField(TEXT("controller_state"), ControllerState));
	TestTrue(TEXT("controller_state is non-empty"), !ControllerState.IsEmpty());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
