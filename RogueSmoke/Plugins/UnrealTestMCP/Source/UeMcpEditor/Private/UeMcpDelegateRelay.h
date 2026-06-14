// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Tiny UObject whose only purpose is to expose a UFUNCTION-tagged thunk
// the dynamic-multicast delegate machinery can bind to.
//
// Why a dedicated UObject and not a plain lambda: every
// `DECLARE_DYNAMIC_MULTICAST_DELEGATE_*` requires a `UFUNCTION()` on a
// `UObject` subclass — `FScriptDelegate::BindUFunction(UObject*, FName)`
// is the only API and `FName` resolves through `UObject::FindFunction`.
// A plain TFunction can't be bound. This relay is the smallest UObject
// that satisfies that contract.
//
// One relay instance is allocated per outstanding `event.wait_for_delegate`
// request. The relay holds a shared flag the request's pending step
// closure polls each tick; when the multicast fires, the relay flips the
// flag and the step closure returns Done on its next service.
//
// Signature mismatch tolerance: the relay's UFUNCTION takes zero
// arguments. Multicast delegates carry their own signature (e.g.
// `FActorDestroyedSignature` is `OneParam(AActor*)`); the engine's
// dynamic-multicast machinery dispatches by name via `ProcessEvent`, so
// a mismatched parameter list silently no-ops the args (the bound function
// doesn't reach in to read parameter memory). This is the same pattern
// the engine's K2 nodes use when binding a no-arg event to a delegate
// with parameters. We do NOT capture the args today — extending to
// capture is a future v1 once we have a generic args→JSON marshaller for
// arbitrary delegate signatures.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Atomic.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"

#include "UeMcpDelegateRelay.generated.h"

/**
 * Shared state between the relay UObject and the request's pending step
 * closure. Atomic-bool because the multicast can fire from any thread
 * the engine chooses (typically game thread, but we make no assumption).
 *
 * Lifetime: created by the factory, owned by both the relay UObject and
 * the captured step state. The state struct's destructor tears the bind
 * down; the relay self-clears its strong ref to this state when the
 * AddToRoot is released.
 */
struct FUeMcpDelegateRelayState
{
	TAtomic<bool> bFired { false };
};

UCLASS()
class UUeMcpDelegateRelay : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * The thunk bound to the multicast. Flips the shared flag, ignores
	 * any args (the binding is name-based; mismatched param lists are
	 * accepted by the dynamic-multicast machinery — the parameter slots
	 * just don't get read).
	 */
	UFUNCTION()
	void OnDelegateFired();

	/** Hook the shared state up after construction. */
	void Init(TSharedRef<FUeMcpDelegateRelayState> InState)
	{
		State = InState;
	}

private:
	TSharedPtr<FUeMcpDelegateRelayState> State;
};
