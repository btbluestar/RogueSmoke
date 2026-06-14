// RogueUIStatics.cpp

#include "UI/RogueUIStatics.h"
#include "CommonActivatableWidget.h"
#include "Input/CommonUIActionRouterBase.h"
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

void URogueUIStatics::ApplyDesiredInputConfig(UCommonActivatableWidget* Widget)
{
	if (Widget == nullptr)
	{
		return;
	}

	UCommonUIActionRouterBase* Router = UCommonUIActionRouterBase::Get(*Widget);
	if (Router == nullptr)
	{
		return;
	}

	const TOptional<FUIInputConfig> Config = Widget->GetDesiredInputConfig();
	if (Config.IsSet())
	{
		Router->SetActiveUIInputConfig(Config.GetValue(), Widget);
	}
}
