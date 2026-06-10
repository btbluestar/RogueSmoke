// RogueUIStatics.cpp

#include "UI/RogueUIStatics.h"
#include "CommonActivatableWidget.h"
#include "Widgets/CommonActivatableWidgetContainer.h"

UCommonActivatableWidget* URogueUIStatics::PushWidgetToStack(UCommonActivatableWidgetStack* Stack,
                                                             TSubclassOf<UCommonActivatableWidget> WidgetClass)
{
	if (Stack == nullptr || WidgetClass.Get() == nullptr)
	{
		return nullptr;
	}
	return Stack->AddWidget<UCommonActivatableWidget>(WidgetClass);
}
