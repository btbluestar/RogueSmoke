// RogueGameStatics.h
// Game-flow statics AngelScript needs but the fork doesn't bind. Currently just the pause:
// UGameplayStatics::SetGamePaused has no AS binding, and the upgrade loop pauses the raid for
// everyone while picks are open (UpgradeLoop concept, 2026-06-11).

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RogueGameStatics.generated.h"

UCLASS()
class ROGUESMOKE_API URogueGameStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Pause/unpause the whole game. On a listen server the WorldSettings pauser replicates, so
	 * remote clients pause too. UI (Slate), player-controller input, and networking keep running
	 * while paused — which is what lets everyone make their upgrade pick.
	 */
	UFUNCTION(BlueprintCallable, Category="Game", meta=(WorldContext="WorldContextObject"))
	static bool SetRaidPaused(UObject* WorldContextObject, bool bPaused);
};
