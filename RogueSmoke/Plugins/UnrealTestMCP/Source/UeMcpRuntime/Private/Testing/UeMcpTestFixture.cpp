// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "Testing/UeMcpTestFixture.h"

#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Math/UnrealMathUtility.h"

AUeMcpTestFixture::AUeMcpTestFixture()
{
	// AFunctionalTest itself enables ticking and sets up the test timer,
	// so we don't flip PrimaryActorTick here — just rely on the base.
}

AActor* AUeMcpTestFixture::SpawnFixtureActor(TSubclassOf<AActor> Class, const FTransform& Transform)
{
	if (!*Class)
	{
		AddError(TEXT("SpawnFixtureActor: Class is null."));
		return nullptr;
	}

	UWorld* const World = GetWorld();
	if (!World)
	{
		AddError(TEXT("SpawnFixtureActor: no UWorld — are you calling this before the fixture is in a level?"));
		return nullptr;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// Naming kept default (anonymous) so multiple fixture actors of the same
	// class don't clash when a test spawns several.
	Params.Owner = this;

	AActor* const Spawned = World->SpawnActor<AActor>(Class, Transform, Params);
	if (!Spawned)
	{
		AddError(FString::Printf(TEXT("SpawnFixtureActor: SpawnActor returned null for class '%s'."),
			*GetNameSafe(*Class)));
		return nullptr;
	}

	SpawnedActors.Add(Spawned);
	return Spawned;
}

bool AUeMcpTestFixture::AssertEqualInt(int32 Actual, int32 Expected, const FString& Message)
{
	const bool bPass = (Actual == Expected);
	if (bPass)
	{
		AddInfo(FString::Printf(TEXT("%s — int equal: %d"), *Message, Actual));
	}
	else
	{
		AddError(FString::Printf(TEXT("%s — int NOT equal: got %d, expected %d"), *Message, Actual, Expected));
	}
	return bPass;
}

bool AUeMcpTestFixture::AssertEqualFloat(float Actual, float Expected, float Tolerance, const FString& Message)
{
	const bool bPass = FMath::IsNearlyEqual(Actual, Expected, Tolerance);
	if (bPass)
	{
		AddInfo(FString::Printf(TEXT("%s — float equal: %f (~= %f ±%f)"),
			*Message, Actual, Expected, Tolerance));
	}
	else
	{
		AddError(FString::Printf(TEXT("%s — float NOT equal: got %f, expected %f (±%f)"),
			*Message, Actual, Expected, Tolerance));
	}
	return bPass;
}

bool AUeMcpTestFixture::AssertTrue(bool Condition, const FString& Message)
{
	if (Condition)
	{
		AddInfo(FString::Printf(TEXT("%s — true"), *Message));
	}
	else
	{
		AddError(FString::Printf(TEXT("%s — expected true, got false"), *Message));
	}
	return Condition;
}

void AUeMcpTestFixture::LatchUntil(TFunction<bool()> Predicate, float TimeoutSeconds,
								   TFunction<void()> OnSuccess, TFunction<void()> OnTimeout,
								   float TickInterval)
{
	if (!Predicate)
	{
		AddError(TEXT("LatchUntil: Predicate is empty."));
		return;
	}

	// Shared state for the ticker closure. We can't mutate TArray<FDelegateHandle>
	// from the closure safely (the fixture could EndPlay mid-tick), so the
	// closure keeps its own local slot and writes back through the weak self.
	TWeakObjectPtr<AUeMcpTestFixture> WeakSelf(this);
	TSharedRef<float, ESPMode::ThreadSafe> Elapsed = MakeShared<float, ESPMode::ThreadSafe>(0.0f);
	TSharedRef<FTSTicker::FDelegateHandle, ESPMode::ThreadSafe> HandleSlot =
		MakeShared<FTSTicker::FDelegateHandle, ESPMode::ThreadSafe>();

	auto Unregister = [WeakSelf, HandleSlot]()
	{
		if (AUeMcpTestFixture* Self = WeakSelf.Get())
		{
			Self->ActiveLatches.RemoveSingleSwap(*HandleSlot);
		}
		// The ticker system removes the handle itself when the delegate
		// returns false, so we don't need to call RemoveTicker here.
	};

	FTickerDelegate Tick = FTickerDelegate::CreateLambda(
		[Predicate, TimeoutSeconds, OnSuccess, OnTimeout, Elapsed, WeakSelf, Unregister]
		(float DeltaTime) -> bool
		{
			// Fixture torn down out from under us — just stop ticking.
			if (!WeakSelf.IsValid())
			{
				return false;
			}

			if (Predicate())
			{
				Unregister();
				if (OnSuccess) { OnSuccess(); }
				return false; // one-shot
			}

			*Elapsed += DeltaTime;
			if (*Elapsed >= TimeoutSeconds)
			{
				Unregister();
				if (OnTimeout) { OnTimeout(); }
				return false; // one-shot
			}

			return true; // keep ticking
		});

	const FTSTicker::FDelegateHandle Handle =
		FTSTicker::GetCoreTicker().AddTicker(Tick, TickInterval);
	*HandleSlot = Handle;
	ActiveLatches.Add(Handle);
}

void AUeMcpTestFixture::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Unregister any live latches before tearing down spawned actors — the
	// latch closures may inspect those actors, and we don't want a dangling
	// tick racing with Destroy().
	for (const FTSTicker::FDelegateHandle& Handle : ActiveLatches)
	{
		if (Handle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(Handle);
		}
	}
	ActiveLatches.Reset();

	for (const TWeakObjectPtr<AActor>& Weak : SpawnedActors)
	{
		if (AActor* Actor = Weak.Get())
		{
			if (IsValid(Actor) && !Actor->IsActorBeingDestroyed())
			{
				Actor->Destroy();
			}
		}
	}
	SpawnedActors.Reset();

	Super::EndPlay(EndPlayReason);
}
