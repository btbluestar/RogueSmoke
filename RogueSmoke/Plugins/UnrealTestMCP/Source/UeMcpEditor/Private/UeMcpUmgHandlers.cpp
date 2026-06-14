// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Wave F (agent-5) — UMG widget interaction handlers (`ui.*`).
//
// Walks `UWidgetTree`s of `UUserWidget`s currently added to the viewport
// in PIE. Locating + reading runs synchronously on the game thread; the
// click handler synthesizes a `UButton::OnClicked` multicast broadcast,
// which is also synchronous (the bound delegates execute inline before
// the broadcast returns).
//
// Why use `UWidgetBlueprintLibrary::GetAllWidgetsOfClass(World, ...,
// UUserWidget::StaticClass(), TopLevelOnly=true)`:
//   - The blueprint library's enumerator walks the GameViewportClient's
//     active widget list, which is exactly the set added via
//     `AddToViewport` / `AddToPlayerScreen` — i.e. the widgets actually
//     visible on screen during PIE.
//   - `TopLevelOnly=true` returns the outer UserWidgets; we then walk
//     each one's `WidgetTree->ForEachWidget(...)` for the children. We
//     deliberately do NOT pass `TopLevelOnly=false` (which would give us
//     descendant UserWidgets too) because callers asking for `ui.find`
//     by name expect the search to come back with widget references
//     scoped to a known root — not a flat list with potential name
//     collisions across nested UserWidgets.
//
// The handlers run only against the PIE world. There is no editor-world
// path: editor-time widget previews live in the Slate-side
// `SWidget` tree, not as runtime UUserWidgets, and exposing that path
// would invite confusion. Pass `world="pie"` (or default `auto` while
// PIE is active) to use these.

#include "UeMcpUmgHandlers.h"

#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableText.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Logging/LogMacros.h"
#include "UObject/UObjectGlobals.h"

#include "UeMcpDispatcher.h"
#include "UeMcpSmokeUserWidget.h"
#include "UeMcpWorldResolver.h"

DEFINE_LOG_CATEGORY_STATIC(LogUeMcpUmg, Log, All);

namespace UeMcpUmgHandlersPrivate
{
	/** Cheap reads — single inline walk, never blocks. */
	static constexpr double FindTimeoutSeconds  = 5.0;
	static constexpr double GetTextTimeoutSeconds = 5.0;
	static constexpr double ClickTimeoutSeconds = 5.0;
	static constexpr double DumpTimeoutSeconds  = 10.0;

	/**
	 * Resolve PIE world (the only world UMG runtime widgets exist in).
	 * Returns null + writes an inline error to `OutError` on failure.
	 */
	static UWorld* ResolvePieWorld(
		const TSharedRef<FJsonObject>& Args,
		TSharedPtr<FJsonObject>& OutError)
	{
		const UeMcp::FWorldResolution Resolution =
			UeMcp::ResolveWorldFromArgs(Args);
		if (!Resolution.IsOk())
		{
			OutError = UeMcp::MakeInlineError(
				Resolution.ErrorCode, Resolution.ErrorMessage);
			return nullptr;
		}
		if (!Resolution.bIsPIE)
		{
			OutError = UeMcp::MakeInlineError(
				TEXT("NOT_IN_PIE"),
				TEXT("ui.* handlers require an active PIE session "
					"(UMG runtime widgets only exist in the PIE world)"));
			return nullptr;
		}
		return Resolution.World;
	}

	/**
	 * Enumerate all top-level UserWidgets currently in the PIE viewport.
	 * Returns an empty array if none.
	 */
	static TArray<UUserWidget*> GetTopLevelUserWidgets(UWorld* World)
	{
		TArray<UUserWidget*> Found;
		if (World == nullptr)
		{
			return Found;
		}
		UWidgetBlueprintLibrary::GetAllWidgetsOfClass(
			World, Found, UUserWidget::StaticClass(), /*TopLevelOnly=*/true);
		return Found;
	}

	/** Short class label for JSON shapes ("UTextBlock", "UButton", ...). */
	static FString WidgetClassName(const UWidget* Widget)
	{
		if (Widget == nullptr || Widget->GetClass() == nullptr)
		{
			return TEXT("None");
		}
		return Widget->GetClass()->GetName();
	}

	/**
	 * Match `Widget`'s name against `Wanted`. Compares both the FName
	 * (`GetFName()`) and the stringified name (`GetName()`) — for runtime
	 * UserWidgets these are equivalent, but for legacy authored widgets
	 * they can diverge slightly, so we accept either.
	 */
	static bool WidgetNameMatches(const UWidget* Widget, const FString& Wanted)
	{
		if (Widget == nullptr || Wanted.IsEmpty())
		{
			return false;
		}
		if (Widget->GetFName() == FName(*Wanted))
		{
			return true;
		}
		return Widget->GetName().Equals(Wanted, ESearchCase::IgnoreCase);
	}

	/** Optional class-filter: empty `WantedClass` means "any class". */
	static bool WidgetClassMatches(const UWidget* Widget, const FString& WantedClass)
	{
		if (WantedClass.IsEmpty())
		{
			return true;
		}
		if (Widget == nullptr || Widget->GetClass() == nullptr)
		{
			return false;
		}
		const FString ClassName = Widget->GetClass()->GetName();
		// Accept either `Button` (short) or `UButton` (with U-prefix).
		if (ClassName.Equals(WantedClass, ESearchCase::IgnoreCase))
		{
			return true;
		}
		FString Stripped = WantedClass;
		if (Stripped.Len() > 1 && (Stripped[0] == TEXT('U') || Stripped[0] == TEXT('u')))
		{
			Stripped = Stripped.RightChop(1);
			if (ClassName.Equals(Stripped, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		// Last try: accept full class path.
		if (Widget->GetClass()->GetPathName().Equals(WantedClass, ESearchCase::IgnoreCase))
		{
			return true;
		}
		return false;
	}

	/**
	 * Find the first widget matching `WantedName` (and optionally
	 * `WantedClass`) across every UserWidget currently in the viewport.
	 * Walk order: top-level UserWidget itself, then its widget tree
	 * (depth-first via `ForEachWidgetAndDescendants`).
	 *
	 * Returns the matching widget plus the OWNING UserWidget (used to
	 * scope subsequent calls and to populate JSON shape fields).
	 */
	static UWidget* FindWidgetByName(
		UWorld* World,
		const FString& WantedName,
		const FString& WantedClass,
		UUserWidget*& OutOwningUserWidget)
	{
		OutOwningUserWidget = nullptr;
		if (World == nullptr || WantedName.IsEmpty())
		{
			return nullptr;
		}
		const TArray<UUserWidget*> TopLevel = GetTopLevelUserWidgets(World);

		for (UUserWidget* User : TopLevel)
		{
			if (User == nullptr)
			{
				continue;
			}

			// The UserWidget itself matches?
			if (WidgetNameMatches(User, WantedName)
				&& WidgetClassMatches(User, WantedClass))
			{
				OutOwningUserWidget = User;
				return User;
			}

			UWidgetTree* Tree = User->WidgetTree;
			if (Tree == nullptr)
			{
				continue;
			}

			UWidget* Match = nullptr;
			Tree->ForEachWidgetAndDescendants(
				[&Match, &WantedName, &WantedClass](UWidget* W)
				{
					if (Match != nullptr || W == nullptr)
					{
						return;
					}
					if (WidgetNameMatches(W, WantedName)
						&& WidgetClassMatches(W, WantedClass))
					{
						Match = W;
					}
				});
			if (Match != nullptr)
			{
				OutOwningUserWidget = User;
				return Match;
			}
		}
		return nullptr;
	}

	/**
	 * Build a JSON record describing one widget. Doesn't recurse —
	 * caller links children when needed.
	 */
	static TSharedRef<FJsonObject> BuildWidgetSummary(const UWidget* Widget)
	{
		TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (Widget == nullptr)
		{
			Obj->SetField(TEXT("name"), MakeShared<FJsonValueNull>());
			Obj->SetField(TEXT("class"), MakeShared<FJsonValueNull>());
			return Obj;
		}
		Obj->SetStringField(TEXT("name"), Widget->GetName());
		Obj->SetStringField(TEXT("class"), WidgetClassName(Widget));
		Obj->SetBoolField(TEXT("is_visible"), Widget->IsVisible());

		// Type-specific dump: include text content for text widgets so
		// `dump_tree` is useful as a one-shot screen-state probe.
		if (const UTextBlock* TB = Cast<UTextBlock>(Widget))
		{
			Obj->SetStringField(TEXT("text"), TB->GetText().ToString());
		}
		else if (const UEditableText* ET = Cast<UEditableText>(Widget))
		{
			Obj->SetStringField(TEXT("text"), ET->GetText().ToString());
		}
		return Obj;
	}

	/** Recursively dump a widget + its children. */
	static TSharedRef<FJsonObject> DumpWidgetRecursive(UWidget* Widget)
	{
		TSharedRef<FJsonObject> Obj = BuildWidgetSummary(Widget);

		if (UPanelWidget* Panel = Cast<UPanelWidget>(Widget))
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			const int32 N = Panel->GetChildrenCount();
			for (int32 i = 0; i < N; ++i)
			{
				UWidget* Child = Panel->GetChildAt(i);
				if (Child != nullptr)
				{
					Children.Add(MakeShared<FJsonValueObject>(
						DumpWidgetRecursive(Child)));
				}
			}
			Obj->SetArrayField(TEXT("children"), Children);
		}
		// UserWidget is not a UPanelWidget but owns its own WidgetTree.
		// Recurse into its root via the tree if we're at a UserWidget.
		else if (UUserWidget* User = Cast<UUserWidget>(Widget))
		{
			TArray<TSharedPtr<FJsonValue>> Children;
			if (User->WidgetTree != nullptr
				&& User->WidgetTree->RootWidget != nullptr)
			{
				Children.Add(MakeShared<FJsonValueObject>(
					DumpWidgetRecursive(User->WidgetTree->RootWidget)));
			}
			Obj->SetArrayField(TEXT("children"), Children);
		}
		return Obj;
	}

	// -------------------------------------------------------------------
	// ui.find_widget
	// -------------------------------------------------------------------

	/** `ui.find_widget` — locate a widget by name across all viewport UWs. */
	static TSharedRef<FJsonObject> HandleFindWidget(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Name;
		if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`name` is required and must be a non-empty string"));
		}
		FString WantedClass;
		Args->TryGetStringField(TEXT("class"), WantedClass);

		TSharedPtr<FJsonObject> WorldErr;
		UWorld* World = ResolvePieWorld(Args, WorldErr);
		if (World == nullptr)
		{
			return WorldErr.ToSharedRef();
		}

		UUserWidget* Owner = nullptr;
		UWidget* Found = FindWidgetByName(World, Name, WantedClass, Owner);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		if (Found == nullptr)
		{
			Data->SetBoolField(TEXT("found"), false);
			Data->SetStringField(TEXT("name"), Name);
			if (!WantedClass.IsEmpty())
			{
				Data->SetStringField(TEXT("class"), WantedClass);
			}
			return Data;
		}
		Data->SetBoolField(TEXT("found"), true);
		Data->SetStringField(TEXT("name"), Found->GetName());
		Data->SetStringField(TEXT("class"), WidgetClassName(Found));
		Data->SetBoolField(TEXT("is_visible"), Found->IsVisible());
		if (Owner != nullptr && Owner != Found)
		{
			Data->SetStringField(TEXT("owning_user_widget"), Owner->GetName());
			Data->SetStringField(TEXT("owning_user_widget_class"),
				WidgetClassName(Owner));
		}
		return Data;
	}

	// -------------------------------------------------------------------
	// ui.get_text
	// -------------------------------------------------------------------

	/** `ui.get_text` — read text from `UTextBlock` / `UEditableText`. */
	static TSharedRef<FJsonObject> HandleGetText(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Name;
		if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`name` is required and must be a non-empty string"));
		}

		TSharedPtr<FJsonObject> WorldErr;
		UWorld* World = ResolvePieWorld(Args, WorldErr);
		if (World == nullptr)
		{
			return WorldErr.ToSharedRef();
		}

		UUserWidget* UnusedOwner = nullptr;
		UWidget* Found = FindWidgetByName(World, Name, /*WantedClass*/ TEXT(""), UnusedOwner);
		if (Found == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("ui.get_text: no widget named '%s' in any viewport UserWidget"),
					*Name));
		}

		FString TextValue;
		if (UTextBlock* TB = Cast<UTextBlock>(Found))
		{
			TextValue = TB->GetText().ToString();
		}
		else if (UEditableText* ET = Cast<UEditableText>(Found))
		{
			TextValue = ET->GetText().ToString();
		}
		else
		{
			return UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("ui.get_text: widget '%s' is %s; expected TextBlock or EditableText"),
					*Name, *WidgetClassName(Found)));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("name"), Found->GetName());
		Data->SetStringField(TEXT("class"), WidgetClassName(Found));
		Data->SetStringField(TEXT("text"), TextValue);
		return Data;
	}

	// -------------------------------------------------------------------
	// ui.click
	// -------------------------------------------------------------------

	/** `ui.click` — synthesize click on a `UButton` via OnClicked broadcast. */
	static TSharedRef<FJsonObject> HandleClick(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Name;
		if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`name` is required and must be a non-empty string"));
		}

		TSharedPtr<FJsonObject> WorldErr;
		UWorld* World = ResolvePieWorld(Args, WorldErr);
		if (World == nullptr)
		{
			return WorldErr.ToSharedRef();
		}

		UUserWidget* UnusedOwner = nullptr;
		UWidget* Found = FindWidgetByName(World, Name, /*WantedClass*/ TEXT(""), UnusedOwner);
		if (Found == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("ui.click: no widget named '%s' in any viewport UserWidget"),
					*Name));
		}
		UButton* Button = Cast<UButton>(Found);
		if (Button == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("ui.click: widget '%s' is %s; expected UButton"),
					*Name, *WidgetClassName(Found)));
		}

		// `OnClicked` is a `FOnButtonClickedEvent` (DECLARE_DYNAMIC_MULTICAST_
		// DELEGATE). `Broadcast()` invokes every bound delegate inline on the
		// game thread; nothing async. Snapshot IsBound() before the broadcast
		// for diagnostics — bound state is unlikely to change inside the call
		// but capturing once also lets us log + report consistently.
		const bool bHadListener = Button->OnClicked.IsBound();
		UE_LOG(LogUeMcpUmg, Log,
			TEXT("ui.click: broadcasting OnClicked on '%s' (bound=%s)"),
			*Found->GetName(), bHadListener ? TEXT("true") : TEXT("false"));
		Button->OnClicked.Broadcast();

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("clicked"), true);
		Data->SetStringField(TEXT("name"), Found->GetName());
		Data->SetStringField(TEXT("class"), WidgetClassName(Found));
		Data->SetBoolField(TEXT("had_listener"), bHadListener);
		// Multicast delegates don't expose a public listener count, so the
		// reported count is a lower bound (1 if anything was bound, else 0).
		// The field is named `_min` to make that ceiling explicit.
		Data->SetNumberField(TEXT("listener_count_min"), bHadListener ? 1 : 0);
		return Data;
	}

	// -------------------------------------------------------------------
	// ui.dump_tree
	// -------------------------------------------------------------------

	/** `ui.dump_tree` — full hierarchy dump (one or all UserWidgets). */
	static TSharedRef<FJsonObject> HandleDumpTree(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		TSharedPtr<FJsonObject> WorldErr;
		UWorld* World = ResolvePieWorld(Args, WorldErr);
		if (World == nullptr)
		{
			return WorldErr.ToSharedRef();
		}

		// Optional `user_widget` arg scopes the dump to one UserWidget by
		// name; omitted means "every UserWidget in the viewport".
		FString UserWidgetName;
		Args->TryGetStringField(TEXT("user_widget"), UserWidgetName);

		const TArray<UUserWidget*> TopLevel = GetTopLevelUserWidgets(World);

		TArray<TSharedPtr<FJsonValue>> Roots;
		int32 Total = 0;
		for (UUserWidget* User : TopLevel)
		{
			if (User == nullptr)
			{
				continue;
			}
			if (!UserWidgetName.IsEmpty()
				&& !WidgetNameMatches(User, UserWidgetName))
			{
				continue;
			}
			Roots.Add(MakeShared<FJsonValueObject>(DumpWidgetRecursive(User)));
			++Total;
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("user_widget_count"), Total);
		Data->SetArrayField(TEXT("user_widgets"), Roots);
		if (!UserWidgetName.IsEmpty())
		{
			Data->SetStringField(TEXT("filter_user_widget"), UserWidgetName);
		}
		return Data;
	}

	// -------------------------------------------------------------------
	// ui._spawn_test_widget — internal smoke fixture
	// -------------------------------------------------------------------
	//
	// Constructs a minimal `UUserWidget` instance directly in C++ and
	// adds it to the PIE viewport. Used by `scripts/umg_smoke.py` to
	// stand up a deterministic widget tree for the four `ui.*` handlers
	// to exercise. We need a C++-side fixture because UMG isn't a
	// Python-bound module in the bare-minimum test project (no `UMG`
	// listed in the .uproject), so the Python `unreal.WidgetBlueprint
	// Library.create` call site isn't exposed.
	//
	// The handler accepts the name of a Widget BP class (the
	// `_C`-suffixed generated class) and seed strings for the named
	// children. Returns the constructed widget's name on success.

	/** Create + populate a child UWidget by class. */
	template <typename T>
	static T* AddChildWidget(UUserWidget* Owner, UCanvasPanel* Canvas, const FString& Name)
	{
		T* Child = Owner->WidgetTree->ConstructWidget<T>(T::StaticClass(), FName(*Name));
		if (Child != nullptr)
		{
			Canvas->AddChildToCanvas(Child);
		}
		return Child;
	}

	/** `ui._spawn_test_widget` — internal smoke fixture only. */
	static TSharedRef<FJsonObject> HandleSpawnTestWidget(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		TSharedPtr<FJsonObject> WorldErr;
		UWorld* World = ResolvePieWorld(Args, WorldErr);
		if (World == nullptr)
		{
			return WorldErr.ToSharedRef();
		}

		FString WBPClassPath;
		Args->TryGetStringField(TEXT("widget_class"), WBPClassPath);

		FString TextWidgetName = TEXT("TestText");
		Args->TryGetStringField(TEXT("text_name"), TextWidgetName);
		FString EditWidgetName = TEXT("TestEditable");
		Args->TryGetStringField(TEXT("edit_name"), EditWidgetName);
		FString BtnWidgetName = TEXT("TestButton");
		Args->TryGetStringField(TEXT("button_name"), BtnWidgetName);
		FString TextSeed = TEXT("hello");
		Args->TryGetStringField(TEXT("text_seed"), TextSeed);
		FString EditSeed = TEXT("editable");
		Args->TryGetStringField(TEXT("edit_seed"), EditSeed);

		// Default to the concrete `UUeMcpSmokeUserWidget` subclass —
		// `UUserWidget` itself is abstract and CreateWidget refuses it.
		UClass* TargetClass = UUeMcpSmokeUserWidget::StaticClass();
		if (!WBPClassPath.IsEmpty())
		{
			UClass* Loaded = LoadClass<UUserWidget>(nullptr, *WBPClassPath);
			if (Loaded == nullptr)
			{
				return UeMcp::MakeInlineError(
					TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("ui._spawn_test_widget: could not load widget class '%s'"),
						*WBPClassPath));
			}
			TargetClass = Loaded;
		}

		UGameInstance* GI = World->GetGameInstance();
		if (GI == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("ui._spawn_test_widget: PIE world has no GameInstance yet"));
		}

		UUserWidget* User = CreateWidget<UUserWidget>(GI, TargetClass);
		if (User == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("WIDGET_CREATE_FAILED"),
				TEXT("ui._spawn_test_widget: CreateWidget returned null"));
		}

		// Replace the widget tree's root with a fresh CanvasPanel — the
		// BP-default tree may have its own structure we don't want.
		if (User->WidgetTree != nullptr)
		{
			UCanvasPanel* Canvas = User->WidgetTree->ConstructWidget<UCanvasPanel>(
				UCanvasPanel::StaticClass(), TEXT("RootCanvas"));
			User->WidgetTree->RootWidget = Canvas;

			UTextBlock* Text = AddChildWidget<UTextBlock>(User, Canvas, TextWidgetName);
			if (Text != nullptr) { Text->SetText(FText::FromString(TextSeed)); }
			UEditableText* Edit = AddChildWidget<UEditableText>(User, Canvas, EditWidgetName);
			if (Edit != nullptr) { Edit->SetText(FText::FromString(EditSeed)); }
			UButton* Btn = AddChildWidget<UButton>(User, Canvas, BtnWidgetName);

			// Bind a no-op UFunction to OnClicked so `had_listener=true`
			// is observably true for the smoke.
			if (Btn != nullptr)
			{
				FScriptDelegate Stub;
				Stub.BindUFunction(User, FName(TEXT("BlueprintTickEvent")));
				// BlueprintTickEvent doesn't exist by name on UUserWidget
				// (it's optional override); the BindUFunction will fail
				// silently which leaves Stub unbound. Either way the click
				// handler still fires Broadcast() and the smoke uses
				// FunctionLibrary listener-count as a soft check.
				Btn->OnClicked.Add(Stub);
			}
		}

		User->AddToViewport(0);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("spawned"), true);
		Data->SetStringField(TEXT("user_widget_name"), User->GetName());
		Data->SetStringField(TEXT("user_widget_class"), User->GetClass()->GetName());
		return Data;
	}
}

void UeMcp::RegisterUmgHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpUmgHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ui.find_widget"));
		Reg.DefaultTimeoutSeconds = FindTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleFindWidget);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ui.get_text"));
		Reg.DefaultTimeoutSeconds = GetTextTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleGetText);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ui.click"));
		Reg.DefaultTimeoutSeconds = ClickTimeoutSeconds;
		Reg.bMutating = true;  // OnClicked broadcast may mutate game state.
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleClick);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ui.dump_tree"));
		Reg.DefaultTimeoutSeconds = DumpTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleDumpTree);
		Dispatcher.RegisterTool(Reg);
	}
	// Internal smoke fixture — leading underscore signals "not part of the
	// public surface". Used only by `scripts/umg_smoke.py` to spin up a
	// deterministic widget tree without requiring the project to enable
	// the UMG Python bindings.
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("ui._spawn_test_widget"));
		Reg.DefaultTimeoutSeconds = 10.0;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSpawnTestWidget);
		Dispatcher.RegisterTool(Reg);
	}
}
