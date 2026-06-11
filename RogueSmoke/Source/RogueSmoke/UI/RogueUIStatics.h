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

	/**
	 * Apply Widget's desired input config directly through its action router.
	 *
	 * Why this exists: FActivatableTreeRoot::ApplyLeafmostNodeConfig has an editor-builds-only
	 * guard (IsViewportWindowInFocusPath) that SILENTLY drops the one-shot config application
	 * when the slate user's focus is dangling — which is reliably the case at the exact frames
	 * a fallback root (our Game-layer HUD host) becomes the active root: right after a map
	 * travel destroyed the focused widget, or right after a click destroyed the clicked menu
	 * button. The router never retries, so the previous config (cursor, no capture) lingers
	 * over gameplay. This shim feeds the exact same config through the router's public
	 * SetActiveUIInputConfig — the arbitration state stays consistent; only the flaky guard is
	 * bypassed. Menus opening still apply through the normal CommonUI path.
	 */
	UFUNCTION(BlueprintCallable, Category = "UI")
	static void ApplyDesiredInputConfig(UCommonActivatableWidget* Widget);
};
