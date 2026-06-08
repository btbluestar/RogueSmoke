// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Concrete UUserWidget subclass used only by the `ui._spawn_test_widget`
// smoke fixture. UUserWidget itself is `UCLASS(Abstract)`, so the
// engine refuses to instantiate it via `CreateWidget`. This shell adds
// no behaviour — it exists purely so the smoke can stand up a runtime
// widget tree without depending on a Widget Blueprint asset (which the
// minimal test project does not ship).

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UeMcpSmokeUserWidget.generated.h"

UCLASS()
class UEMCPEDITOR_API UUeMcpSmokeUserWidget : public UUserWidget
{
	GENERATED_BODY()
};
