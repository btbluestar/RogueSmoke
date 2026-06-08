// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// AUeMcpTestFixture — reusable AFunctionalTest subclass so game-module
// tests can declare intent (spawn, assert, latch) without re-implementing
// teardown and polling each time. See docs/v2_pivot.md §Gap 5.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "FunctionalTest.h"
#include "GameFramework/Actor.h"
#include "Templates/SubclassOf.h"
#include "UObject/WeakObjectPtrTemplates.h"

#include "UeMcpTestFixture.generated.h"

/**
 * Base class for gameplay integration tests run under the Functional
 * Testing framework. Subclass in a game module, override StartTest(),
 * call SpawnFixtureActor / AssertXxx / LatchUntil, then FinishTest(...).
 *
 * Lifecycle contract:
 *  - SpawnFixtureActor registers the returned actor for auto-Destroy()
 *    in EndPlay, so tests don't leak into the editor world if they
 *    fail mid-run.
 *  - LatchUntil wires a ticker that is unregistered in EndPlay or when
 *    the predicate / timeout resolves, whichever comes first.
 *
 * Thread-affinity: all methods are game-thread only (AActor semantics).
 */
UCLASS(Abstract, Blueprintable)
class UEMCPRUNTIME_API AUeMcpTestFixture : public AFunctionalTest
{
	GENERATED_BODY()

public:
	AUeMcpTestFixture();

	/** Spawn Class at Transform and register for teardown. Returns nullptr on failure. */
	UFUNCTION(BlueprintCallable, Category="UeMcp|Fixture")
	AActor* SpawnFixtureActor(TSubclassOf<AActor> Class, const FTransform& Transform);

	/** int32 equality check. Logs via AddInfo on pass, AddError on fail. Returns true on pass. */
	UFUNCTION(BlueprintCallable, Category="UeMcp|Fixture")
	bool AssertEqualInt(int32 Actual, int32 Expected, const FString& Message);

	/** Float equality with tolerance. Returns true on pass. */
	UFUNCTION(BlueprintCallable, Category="UeMcp|Fixture")
	bool AssertEqualFloat(float Actual, float Expected, float Tolerance, const FString& Message);

	/**
	 * Boolean truthy check. Returns true on pass.
	 *
	 * No UFUNCTION — AFunctionalTest already exposes a BlueprintCallable
	 * `AssertTrue(bool, FString, UObject*)` and UHT won't let us overload
	 * or override it with a different signature. BP authors should call
	 * the base's AssertTrue directly; this C++ helper keeps the spec's
	 * 2-arg shape for native tests.
	 */
	bool AssertTrue(bool Condition, const FString& Message);

	// NOTE: this 2-arg overload hides AFunctionalTest::AssertTrue(bool,
	// FString, UObject*). That's intentional — the spec calls for a
	// 2-arg shape. Callers who need the ContextObject form must qualify
	// it explicitly as `AFunctionalTest::AssertTrue(...)`.

	/**
	 * Poll Predicate on the core ticker every TickInterval seconds. If it
	 * returns true, OnSuccess fires and the latch unregisters. If
	 * TimeoutSeconds elapses first, OnTimeout fires and the latch
	 * unregisters. Intended to be started from StartTest(); EndPlay also
	 * unregisters any still-active latches as a safety net.
	 *
	 * Not a UFUNCTION — TFunction<> params aren't reflection-legal. BP
	 * users should build latches via AFunctionalTest's native scheduling
	 * helpers or the existing delay nodes.
	 */
	void LatchUntil(TFunction<bool()> Predicate, float TimeoutSeconds,
					TFunction<void()> OnSuccess, TFunction<void()> OnTimeout,
					float TickInterval = 0.05f);

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** Actors spawned via SpawnFixtureActor, pending Destroy() on EndPlay. Weak so teardown is safe if GC already nuked them. */
	TArray<TWeakObjectPtr<AActor>> SpawnedActors;

	/** Handles for ticker-driven latches started by LatchUntil. */
	TArray<FTSTicker::FDelegateHandle> ActiveLatches;
};
