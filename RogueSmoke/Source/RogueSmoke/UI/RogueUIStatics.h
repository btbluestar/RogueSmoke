// RogueUIStatics.h
// Tiny CommonUI shim for AngelScript. UCommonActivatableWidgetContainerBase::BP_AddWidget is
// declared private (BlueprintCallable reaches it through the BP VM, but the AngelScript
// reflection rightly skips private members), and the public AddWidget<T>/AddWidgetInstance API
// is a non-UFUNCTION template. This library exposes the push as a plain BlueprintCallable so
// the script-side URogueUILayout can drive layer stacks.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "RogueUIStatics.generated.h"

class UCommonActivatableWidget;
class UCommonActivatableWidgetStack;

UCLASS()
class ROGUESMOKE_API URogueUIStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Push WidgetClass onto a CommonUI activatable stack; returns the instance the stack created. */
	UFUNCTION(BlueprintCallable, Category = "UI")
	static UCommonActivatableWidget* PushWidgetToStack(UCommonActivatableWidgetStack* Stack,
	                                                   TSubclassOf<UCommonActivatableWidget> WidgetClass);
};
