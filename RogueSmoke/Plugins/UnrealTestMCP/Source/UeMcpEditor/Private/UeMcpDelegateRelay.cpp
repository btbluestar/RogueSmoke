// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpDelegateRelay.h"

void UUeMcpDelegateRelay::OnDelegateFired()
{
	// Be defensive: the multicast can race with state teardown if the
	// caller's request was abandoned mid-bind. A null/expired State is
	// fine — the request's response was already published.
	if (State.IsValid())
	{
		State->bFired.Store(true);
	}
}
