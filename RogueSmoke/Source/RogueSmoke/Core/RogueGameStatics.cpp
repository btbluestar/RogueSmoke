// RogueGameStatics.cpp

#include "Core/RogueGameStatics.h"
#include "Kismet/GameplayStatics.h"

bool URogueGameStatics::SetRaidPaused(UObject* WorldContextObject, bool bPaused)
{
	return UGameplayStatics::SetGamePaused(WorldContextObject, bPaused);
}
