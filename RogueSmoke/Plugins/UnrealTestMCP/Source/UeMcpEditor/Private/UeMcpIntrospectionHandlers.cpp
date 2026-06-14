// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpIntrospectionHandlers.h"

#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpPropertyAccessor.h"
#include "UeMcpPropertyValue.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpIntrospectionHandlersPrivate
{
	/** Default dispatcher timeouts. Most are cheap reads. */
	static constexpr double SummaryTimeoutSeconds   = 10.0;
	static constexpr double PropertiesTimeoutSeconds = 20.0;
	static constexpr double OutlineTimeoutSeconds    = 20.0;
	static constexpr double CdoTimeoutSeconds        = 15.0;

	// Accessor-error -> wire-code now goes through the shared
	// `UeMcp::AccessorErrorToCode` (issue #62); the formerly-duplicated
	// local mapper was removed.

	/**
	 * Render an `FEdGraphPinType` to a short human-readable string for
	 * `blueprint.outline`'s variable listing. We don't want to ship full
	 * `EdGraphSchema_K2::TypeToText` — it's verbose and localized. A flat
	 * `"int"` / `"Vector"` / `"Actor*"` / `"array<FTransform>"` form is
	 * what agents actually parse.
	 *
	 * Returns "unknown" for pin types we don't explicitly handle; listing
	 * a placeholder keeps the outline shape stable.
	 */
	static FString RenderPinType(const FEdGraphPinType& Pin)
	{
		auto CategoryToString = [](const FName& Cat, const UObject* Sub) -> FString
		{
			// Exhaustive match on the K2 well-known category names.
			if (Cat == UEdGraphSchema_K2::PC_Boolean)   return TEXT("bool");
			if (Cat == UEdGraphSchema_K2::PC_Byte)      return TEXT("byte");
			if (Cat == UEdGraphSchema_K2::PC_Int)       return TEXT("int");
			if (Cat == UEdGraphSchema_K2::PC_Int64)     return TEXT("int64");
			if (Cat == UEdGraphSchema_K2::PC_Float)     return TEXT("float");
			if (Cat == UEdGraphSchema_K2::PC_Double)    return TEXT("double");
			if (Cat == UEdGraphSchema_K2::PC_Real)      return TEXT("real");
			if (Cat == UEdGraphSchema_K2::PC_String)    return TEXT("string");
			if (Cat == UEdGraphSchema_K2::PC_Name)      return TEXT("name");
			if (Cat == UEdGraphSchema_K2::PC_Text)      return TEXT("text");
			if (Cat == UEdGraphSchema_K2::PC_Struct)
			{
				return Sub ? FString::Printf(TEXT("struct<%s>"), *Sub->GetName())
				           : TEXT("struct");
			}
			if (Cat == UEdGraphSchema_K2::PC_Object)
			{
				return Sub ? FString::Printf(TEXT("%s*"), *Sub->GetName())
				           : TEXT("object*");
			}
			if (Cat == UEdGraphSchema_K2::PC_Class)
			{
				return Sub ? FString::Printf(TEXT("class<%s>"), *Sub->GetName())
				           : TEXT("class");
			}
			if (Cat == UEdGraphSchema_K2::PC_Interface)
			{
				return Sub ? FString::Printf(TEXT("interface<%s>"), *Sub->GetName())
				           : TEXT("interface");
			}
			return Cat.IsNone() ? TEXT("unknown") : Cat.ToString();
		};

		const UObject* Sub = Pin.PinSubCategoryObject.Get();
		const FString Base = CategoryToString(Pin.PinCategory, Sub);

		// Container wrapper.
		switch (Pin.ContainerType)
		{
			case EPinContainerType::Array: return FString::Printf(TEXT("array<%s>"), *Base);
			case EPinContainerType::Set:   return FString::Printf(TEXT("set<%s>"),   *Base);
			case EPinContainerType::Map:
			{
				// TerminalType describes the value half.
				const UObject* ValSub = Pin.PinValueType.TerminalSubCategoryObject.Get();
				const FString Val = CategoryToString(
					Pin.PinValueType.TerminalCategory, ValSub);
				return FString::Printf(TEXT("map<%s,%s>"), *Base, *Val);
			}
			default: break;
		}
		return Base;
	}

	/**
	 * Count properties on `Obj` whose current value differs from the CDO
	 * for its class. `Identical` uses `PPF_DeepCompareInstances` so
	 * nested structs and subobjects are compared element-wise / deeply
	 * rather than by pointer equality — matches the engine's own
	 * "overridden defaults" semantics (see
	 * `ActorComponent.cpp:3146`).
	 *
	 * Returns -1 if we couldn't get a CDO (should never happen on a
	 * valid UObject); the handler maps that to a JSON null.
	 */
	static int32 CountOverriddenDefaults(UObject* Obj)
	{
		if (Obj == nullptr)
		{
			return -1;
		}
		UClass* Cls = Obj->GetClass();
		if (Cls == nullptr)
		{
			return -1;
		}
		UObject* Cdo = Cls->GetDefaultObject();
		if (Cdo == nullptr)
		{
			return -1;
		}
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Cls); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop == nullptr)
			{
				continue;
			}
			// Skip transient properties — comparing them against the CDO
			// is meaningless by definition (they're not persisted).
			if (Prop->HasAnyPropertyFlags(CPF_Transient))
			{
				continue;
			}
			const void* Self = Prop->ContainerPtrToValuePtr<void>(Obj);
			const void* Def  = Prop->ContainerPtrToValuePtr<void>(Cdo);
			if (!Prop->Identical(Self, Def, PPF_DeepCompareInstances))
			{
				Count++;
			}
		}
		return Count;
	}

	/**
	 * Build a compact component entry for `actor.summary`'s component
	 * list. Includes name, class, root-flag, and the parent's name for
	 * scene components. Non-scene components have empty `attached_to`.
	 */
	static TSharedRef<FJsonObject> BuildComponentEntry(
		AActor* Owner, UActorComponent* Component)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Component ? Component->GetName() : FString());
		Entry->SetStringField(TEXT("class"),
			(Component && Component->GetClass()) ? Component->GetClass()->GetName() : FString());

		bool bIsRoot = false;
		FString AttachedTo;
		if (USceneComponent* SC = Cast<USceneComponent>(Component))
		{
			if (Owner && Owner->GetRootComponent() == SC)
			{
				bIsRoot = true;
			}
			if (USceneComponent* Parent = SC->GetAttachParent())
			{
				AttachedTo = Parent->GetName();
			}
		}
		Entry->SetBoolField(TEXT("is_root"), bIsRoot);
		Entry->SetStringField(TEXT("attached_to"), AttachedTo);
		return Entry;
	}

	/**
	 * Resolve an "actor path" argument to a live `AActor*` within the
	 * resolved world. Unlike the generic `ResolveObject`, this one always
	 * returns an actor (rejects assets). Matches the shape callers of
	 * `actor.summary` / `actor.properties` expect.
	 */
	static AActor* ResolveActorArg(
		const FString& ArgValue, UWorld* World,
		TSharedPtr<FJsonObject>& OutErr)
	{
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ArgValue, World);
		if (!Resolved.IsOk())
		{
			OutErr = Resolved.ErrorInfo;
			return nullptr;
		}
		AActor* Actor = Cast<AActor>(Resolved.Object);
		if (Actor == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("'%s' resolved to a non-actor object (%s)"),
					*ArgValue,
					*Resolved.Object->GetClass()->GetName()));
			return nullptr;
		}
		return Actor;
	}

	/** `actor.summary` body. */
	static TSharedRef<FJsonObject> HandleActorSummary(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		FString ActorPath;
		if (!Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`actor_path` is required and must be a non-empty string"));
		}

		TSharedPtr<FJsonObject> ErrJson;
		AActor* Actor = ResolveActorArg(ActorPath, World.World, ErrJson);
		if (Actor == nullptr)
		{
			return ErrJson.ToSharedRef();
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Data->SetStringField(TEXT("name"),  Actor->GetName());
		UClass* Cls = Actor->GetClass();
		Data->SetStringField(TEXT("class"),      Cls ? Cls->GetName()     : FString());
		Data->SetStringField(TEXT("class_path"), Cls ? Cls->GetPathName() : FString());
		Data->SetStringField(TEXT("path"),       Actor->GetPathName());

		// Enumerate all components — native + SCS + dynamically-added.
		// GetComponents<UActorComponent> covers the union.
		TInlineComponentArray<UActorComponent*> Components(Actor);
		TArray<TSharedPtr<FJsonValue>> CompValues;
		CompValues.Reserve(Components.Num());
		for (UActorComponent* C : Components)
		{
			CompValues.Add(MakeShared<FJsonValueObject>(BuildComponentEntry(Actor, C)));
		}
		Data->SetArrayField(TEXT("components"), CompValues);
		Data->SetNumberField(TEXT("component_count"), Components.Num());

		const int32 Overridden = CountOverriddenDefaults(Actor);
		if (Overridden < 0)
		{
			Data->SetField(TEXT("num_overridden_defaults"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Data->SetNumberField(TEXT("num_overridden_defaults"), Overridden);
		}

		Data->SetStringField(TEXT("resolved_scope"),
			UeMcp::WorldScopeToString(World.ResolvedScope));
		return Data;
	}

	/**
	 * Read the top-level UPROPERTY snapshot for an actor — one accessor
	 * call per declared property on the actor's class. Does NOT recurse
	 * into nested structs by default; that job belongs to
	 * `list_property_paths` + repeated `get_property` calls.
	 *
	 * The output is a JSON object keyed by property name.
	 */
	static void ReadTopLevelSnapshot(
		UObject* Root,
		bool bIncludeDefaults,
		TSharedRef<FJsonObject>& OutProperties)
	{
		if (Root == nullptr)
		{
			return;
		}
		UClass* Cls = Root->GetClass();
		if (Cls == nullptr)
		{
			return;
		}

		for (TFieldIterator<FProperty> It(Cls); It; ++It)
		{
			FProperty* Prop = *It;
			if (Prop == nullptr)
			{
				continue;
			}
			// Skip transient + deprecated — agents usually don't want them.
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
			{
				continue;
			}
			// When bIncludeDefaults is false, skip properties that match
			// the CDO value. Saves tokens on actors where most fields are
			// untouched from class defaults.
			if (!bIncludeDefaults)
			{
				UObject* Cdo = Cls->GetDefaultObject();
				if (Cdo != nullptr)
				{
					const void* Self = Prop->ContainerPtrToValuePtr<void>(Root);
					const void* Def  = Prop->ContainerPtrToValuePtr<void>(Cdo);
					if (Prop->Identical(Self, Def, PPF_DeepCompareInstances))
					{
						continue;
					}
				}
			}

			FUeMcpPropertyValue Value;
			FUeMcpAccessorErrorInfo Err;
			const FString Name = Prop->GetName();
			if (FUeMcpPropertyAccessor::GetValue(Root, Name, Value, Err)
				&& Value.Json.IsValid())
			{
				OutProperties->SetField(Name, Value.Json);
			}
			// Silent on read failure — a flaky property on a single
			// field should not torpedo the whole snapshot. The caller
			// requests individual paths for precision.
		}
	}

	/** `actor.properties` body. */
	static TSharedRef<FJsonObject> HandleActorProperties(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		const UeMcp::FWorldResolution World = UeMcp::ResolveWorldFromArgs(Args);
		if (!World.IsOk())
		{
			return UeMcp::MakeInlineError(World.ErrorCode, World.ErrorMessage);
		}

		FString ActorPath;
		if (!Args->TryGetStringField(TEXT("actor_path"), ActorPath) || ActorPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`actor_path` is required and must be a non-empty string"));
		}

		TSharedPtr<FJsonObject> ErrJson;
		AActor* Actor = ResolveActorArg(ActorPath, World.World, ErrJson);
		if (Actor == nullptr)
		{
			return ErrJson.ToSharedRef();
		}

		bool bIncludeDefaults = false;
		Args->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);

		TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
		TArray<FString> RequestedPaths;
		TArray<FString> MissingPaths;

		// If `paths` is supplied, bulk-read each path. Else fall back to
		// the top-level-only snapshot.
		const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
		const bool bHasPaths =
			Args->TryGetArrayField(TEXT("paths"), PathsArr) && PathsArr != nullptr;

		if (bHasPaths)
		{
			RequestedPaths.Reserve(PathsArr->Num());
			for (const TSharedPtr<FJsonValue>& Entry : *PathsArr)
			{
				FString PathStr;
				if (Entry.IsValid() && Entry->TryGetString(PathStr) && !PathStr.IsEmpty())
				{
					RequestedPaths.Add(PathStr);
				}
			}
			for (const FString& P : RequestedPaths)
			{
				FUeMcpPropertyValue Value;
				FUeMcpAccessorErrorInfo Err;
				if (FUeMcpPropertyAccessor::GetValue(Actor, P, Value, Err)
					&& Value.Json.IsValid())
				{
					Properties->SetField(P, Value.Json);
				}
				else
				{
					MissingPaths.Add(P);
				}
			}
		}
		else
		{
			ReadTopLevelSnapshot(Actor, bIncludeDefaults, Properties);
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("label"), Actor->GetActorLabel());
		Data->SetStringField(TEXT("name"),  Actor->GetName());
		UClass* Cls = Actor->GetClass();
		Data->SetStringField(TEXT("class"), Cls ? Cls->GetName() : FString());
		Data->SetObjectField(TEXT("properties"), Properties);

		if (bHasPaths)
		{
			TArray<TSharedPtr<FJsonValue>> ReqValues;
			ReqValues.Reserve(RequestedPaths.Num());
			for (const FString& P : RequestedPaths)
			{
				ReqValues.Add(MakeShared<FJsonValueString>(P));
			}
			Data->SetArrayField(TEXT("requested_paths"), ReqValues);

			TArray<TSharedPtr<FJsonValue>> MissValues;
			MissValues.Reserve(MissingPaths.Num());
			for (const FString& P : MissingPaths)
			{
				MissValues.Add(MakeShared<FJsonValueString>(P));
			}
			Data->SetArrayField(TEXT("missing_paths"), MissValues);
		}

		Data->SetStringField(TEXT("resolved_scope"),
			UeMcp::WorldScopeToString(World.ResolvedScope));
		return Data;
	}

	/**
	 * Resolve a class/BP path for `blueprint.outline` or `cdo.defaults`.
	 * Accepts either a `UBlueprint` asset path (unwraps to GeneratedClass)
	 * or a class path directly. Returns nullptr and fills OutErr on failure.
	 *
	 * Design note: we deliberately DO NOT use world-actor iteration for
	 * these tools — both operate on asset data, never on live actors.
	 * So we go straight to the soft-object-path resolver rather than
	 * threading a UWorld through.
	 */
	static UObject* ResolveAssetOrClass(
		const FString& ObjectId,
		UBlueprint*& OutBlueprint,
		UClass*& OutClass,
		TSharedPtr<FJsonObject>& OutErr)
	{
		OutBlueprint = nullptr;
		OutClass = nullptr;

		// Pass nullptr world hint — asset resolution only. The resolver
		// will skip the actor strategy when WorldHint is null.
		UeMcp::FUeMcpResolvedObject Resolved =
			UeMcp::ResolveObject(ObjectId, nullptr);
		if (!Resolved.IsOk())
		{
			OutErr = Resolved.ErrorInfo;
			return nullptr;
		}

		if (UBlueprint* BP = Cast<UBlueprint>(Resolved.Object))
		{
			OutBlueprint = BP;
			OutClass = BP->GeneratedClass;
		}
		else if (UClass* Cls = Cast<UClass>(Resolved.Object))
		{
			OutClass = Cls;
			// Look up the BP asset for this generated class, if any.
			// Native classes have no UBlueprint; BP classes do.
			if (UObject* ClassOuter = Cls->ClassGeneratedBy)
			{
				OutBlueprint = Cast<UBlueprint>(ClassOuter);
			}
		}

		if (OutClass == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("'%s' did not resolve to a class or blueprint"), *ObjectId));
			return nullptr;
		}
		return Resolved.Object;
	}

	/**
	 * Walk a `UBlueprint`'s `UbergraphPages` looking for `UK2Node_Event`
	 * nodes and collect unique event names. The K2 event node reports
	 * its function name via `GetFunctionName()`; we call that rather
	 * than inspecting `EventReference` directly so overridden and
	 * user-defined events both flow through the same path.
	 */
	static void CollectBlueprintEvents(
		UBlueprint* BP, TArray<TSharedPtr<FJsonValue>>& OutEvents)
	{
		if (BP == nullptr)
		{
			return;
		}
		TSet<FName> Seen;
		auto VisitGraph = [&](UEdGraph* Graph)
		{
			if (Graph == nullptr)
			{
				return;
			}
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
				if (EventNode == nullptr)
				{
					continue;
				}
				const FName Name = EventNode->GetFunctionName();
				if (Name.IsNone())
				{
					continue;
				}
				if (Seen.Contains(Name))
				{
					continue;
				}
				Seen.Add(Name);
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Name.ToString());
				Entry->SetBoolField(TEXT("is_override"), EventNode->bOverrideFunction != 0);
				OutEvents.Add(MakeShared<FJsonValueObject>(Entry));
			}
		};

		for (UEdGraph* Graph : BP->UbergraphPages)
		{
			VisitGraph(Graph);
		}
		for (UEdGraph* Graph : BP->EventGraphs)
		{
			VisitGraph(Graph);
		}
	}

	/**
	 * Walk `UBlueprint::FunctionGraphs` and collect function names plus a
	 * short signature string. We look for the single `UK2Node_FunctionEntry`
	 * in each graph to read parameters.
	 *
	 * Signature shape: `"(InA: int, InB: FVector) -> bool"`. Where we
	 * can't find an entry node, signature is `""` (empty) rather than
	 * omitted — stable shape is friendlier to agent parsers.
	 */
	static void CollectBlueprintFunctions(
		UBlueprint* BP, TArray<TSharedPtr<FJsonValue>>& OutFunctions)
	{
		if (BP == nullptr)
		{
			return;
		}

		for (UEdGraph* Graph : BP->FunctionGraphs)
		{
			if (Graph == nullptr)
			{
				continue;
			}
			FString Signature;
			UK2Node_FunctionEntry* Entry = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				Entry = Cast<UK2Node_FunctionEntry>(Node);
				if (Entry != nullptr)
				{
					break;
				}
			}
			if (Entry != nullptr)
			{
				// Pins on the entry node that are OUTPUTS (from the
				// entry node's perspective) are the function's INPUT
				// parameters. We walk in source order for stability.
				TArray<FString> InParts, OutParts;
				for (UEdGraphPin* Pin : Entry->Pins)
				{
					if (Pin == nullptr || Pin->bHidden
						|| Pin->PinName == UEdGraphSchema_K2::PN_Execute
						|| Pin->PinName == UEdGraphSchema_K2::PN_Then
						|| Pin->PinName == UEdGraphSchema_K2::PN_Self)
					{
						continue;
					}
					if (Pin->Direction == EGPD_Output)
					{
						InParts.Add(FString::Printf(TEXT("%s: %s"),
							*Pin->PinName.ToString(), *RenderPinType(Pin->PinType)));
					}
				}

				// Locate the (first/canonical) result node. A BP function
				// can have multiple result nodes for early-return; they
				// all share the same return-pin signature, so any one
				// tells us the shape. Result-node INPUT pins are the
				// function's RETURN values (mirror of the entry node).
				UK2Node_FunctionResult* Result = nullptr;
				for (UEdGraphNode* Node : Graph->Nodes)
				{
					Result = Cast<UK2Node_FunctionResult>(Node);
					if (Result != nullptr)
					{
						break;
					}
				}
				if (Result != nullptr)
				{
					for (UEdGraphPin* Pin : Result->Pins)
					{
						if (Pin == nullptr || Pin->bHidden
							|| Pin->PinName == UEdGraphSchema_K2::PN_Execute
							|| Pin->PinName == UEdGraphSchema_K2::PN_Then
							|| Pin->PinName == UEdGraphSchema_K2::PN_Self)
						{
							continue;
						}
						if (Pin->Direction == EGPD_Input)
						{
							OutParts.Add(FString::Printf(TEXT("%s: %s"),
								*Pin->PinName.ToString(), *RenderPinType(Pin->PinType)));
						}
					}
				}

				// Build the `-> ...` suffix:
				//   0 returns → omit entirely.
				//   1 return  → `-> Type` (drop the typically-`ReturnValue` name).
				//   N returns → `-> (a: T1, b: T2)` so all names are visible.
				FString ReturnPart;
				if (OutParts.Num() == 1)
				{
					int32 ColonIdx = INDEX_NONE;
					OutParts[0].FindChar(TEXT(':'), ColonIdx);
					const FString TypeOnly = (ColonIdx != INDEX_NONE)
						? OutParts[0].Mid(ColonIdx + 2).TrimStartAndEnd()
						: OutParts[0];
					ReturnPart = FString::Printf(TEXT(" -> %s"), *TypeOnly);
				}
				else if (OutParts.Num() > 1)
				{
					ReturnPart = FString::Printf(TEXT(" -> (%s)"),
						*FString::Join(OutParts, TEXT(", ")));
				}

				Signature = FString::Printf(TEXT("(%s)%s"),
					*FString::Join(InParts, TEXT(", ")), *ReturnPart);
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("name"), Graph->GetName());
			Out->SetStringField(TEXT("signature"), Signature);
			OutFunctions.Add(MakeShared<FJsonValueObject>(Out));
		}
	}

	/**
	 * Walk `UBlueprint::NewVariables` and emit
	 * `[{name, type, category?, flags?}]`.
	 */
	static void CollectBlueprintVariables(
		UBlueprint* BP, TArray<TSharedPtr<FJsonValue>>& OutVariables)
	{
		if (BP == nullptr)
		{
			return;
		}
		for (const FBPVariableDescription& Var : BP->NewVariables)
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Var.VarName.ToString());
			Entry->SetStringField(TEXT("type"), RenderPinType(Var.VarType));
			if (!Var.Category.IsEmpty())
			{
				Entry->SetStringField(TEXT("category"), Var.Category.ToString());
			}
			OutVariables.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	/**
	 * Walk `USimpleConstructionScript::GetAllNodes()` and emit the
	 * component tree as a flat list with parent references. Root nodes
	 * (no parent) are flagged via `is_root: true`.
	 *
	 * The SCS models component-template relationships at construction
	 * time; we don't try to surface inheritance from the parent class's
	 * SCS here — that would duplicate components. Agents asking for the
	 * full live-actor component list use `actor.summary` instead.
	 */
	static void CollectBlueprintComponents(
		UBlueprint* BP, TArray<TSharedPtr<FJsonValue>>& OutComponents)
	{
		if (BP == nullptr || BP->SimpleConstructionScript == nullptr)
		{
			return;
		}
		const TArray<USCS_Node*>& All = BP->SimpleConstructionScript->GetAllNodes();
		const TArray<USCS_Node*>& Roots = BP->SimpleConstructionScript->GetRootNodes();
		TSet<USCS_Node*> RootSet(Roots);

		for (USCS_Node* Node : All)
		{
			if (Node == nullptr)
			{
				continue;
			}
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"),
				Node->GetVariableName().ToString());
			UActorComponent* Template = Node->ComponentTemplate;
			Entry->SetStringField(TEXT("class"),
				(Template && Template->GetClass()) ? Template->GetClass()->GetName() : FString());
			Entry->SetBoolField(TEXT("is_root"), RootSet.Contains(Node));
			// ParentComponentOrVariableName is non-empty when the SCS
			// node attaches to a component inherited from a parent
			// class; otherwise the attachment is to another SCS node
			// (exposed implicitly through the tree structure which we
			// don't currently serialize in depth).
			Entry->SetStringField(TEXT("attached_to"),
				Node->ParentComponentOrVariableName.ToString());
			OutComponents.Add(MakeShared<FJsonValueObject>(Entry));
		}
	}

	/** `blueprint.outline` body. */
	static TSharedRef<FJsonObject> HandleBlueprintOutline(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("bp_path"), AssetPath) || AssetPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}

		UBlueprint* BP = nullptr;
		UClass* Cls = nullptr;
		TSharedPtr<FJsonObject> Err;
		if (!ResolveAssetOrClass(AssetPath, BP, Cls, Err))
		{
			return Err.ToSharedRef();
		}

		if (BP == nullptr)
		{
			// Native class — most outline sections are empty, but we
			// still return a stable shape so agents get a predictable
			// "this is a native class, no BP graphs to walk" answer.
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset_path"), AssetPath);
			Data->SetStringField(TEXT("class_path"),
				Cls ? Cls->GetPathName() : FString());
			Data->SetStringField(TEXT("parent_class"),
				(Cls && Cls->GetSuperClass()) ? Cls->GetSuperClass()->GetName() : FString());
			Data->SetBoolField(TEXT("is_native"), true);
			Data->SetArrayField(TEXT("functions"),  TArray<TSharedPtr<FJsonValue>>());
			Data->SetArrayField(TEXT("variables"),  TArray<TSharedPtr<FJsonValue>>());
			Data->SetArrayField(TEXT("components"), TArray<TSharedPtr<FJsonValue>>());
			Data->SetArrayField(TEXT("events"),     TArray<TSharedPtr<FJsonValue>>());
			return Data;
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Data->SetStringField(TEXT("class_path"),
			BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());
		Data->SetStringField(TEXT("parent_class"),
			BP->ParentClass ? BP->ParentClass->GetName() : FString());
		Data->SetBoolField(TEXT("is_native"), false);

		TArray<TSharedPtr<FJsonValue>> Functions;
		CollectBlueprintFunctions(BP, Functions);
		Data->SetArrayField(TEXT("functions"), Functions);

		TArray<TSharedPtr<FJsonValue>> Variables;
		CollectBlueprintVariables(BP, Variables);
		Data->SetArrayField(TEXT("variables"), Variables);

		TArray<TSharedPtr<FJsonValue>> Components;
		CollectBlueprintComponents(BP, Components);
		Data->SetArrayField(TEXT("components"), Components);

		TArray<TSharedPtr<FJsonValue>> Events;
		CollectBlueprintEvents(BP, Events);
		Data->SetArrayField(TEXT("events"), Events);

		return Data;
	}

	/** `cdo.defaults` body. */
	static TSharedRef<FJsonObject> HandleCdoDefaults(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Target;
		if (!Args->TryGetStringField(TEXT("class_path"), Target) || Target.IsEmpty())
		{
			// Accept `class_or_bp_path` as an alias for ergonomic calls.
			Args->TryGetStringField(TEXT("class_or_bp_path"), Target);
		}
		if (Target.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				TEXT("`class_path` (or `class_or_bp_path`) is required"));
		}

		UBlueprint* BP = nullptr;
		UClass* Cls = nullptr;
		TSharedPtr<FJsonObject> Err;
		if (!ResolveAssetOrClass(Target, BP, Cls, Err))
		{
			return Err.ToSharedRef();
		}

		UObject* Cdo = Cls ? Cls->GetDefaultObject() : nullptr;
		if (Cdo == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(TEXT("Class '%s' has no CDO"), *Target));
		}

		// Collect requested paths. When omitted, fall back to the
		// top-level-only snapshot — callers exploring a new class get
		// "tell me everything at depth 1" for free.
		TArray<FString> RequestedPaths;
		const TArray<TSharedPtr<FJsonValue>>* PathsArr = nullptr;
		if (Args->TryGetArrayField(TEXT("paths"), PathsArr) && PathsArr)
		{
			RequestedPaths.Reserve(PathsArr->Num());
			for (const TSharedPtr<FJsonValue>& V : *PathsArr)
			{
				FString P;
				if (V.IsValid() && V->TryGetString(P) && !P.IsEmpty())
				{
					RequestedPaths.Add(P);
				}
			}
		}

		TSharedRef<FJsonObject> Defaults = MakeShared<FJsonObject>();
		TArray<FString> MissingPaths;

		if (RequestedPaths.Num() > 0)
		{
			for (const FString& P : RequestedPaths)
			{
				FUeMcpPropertyValue Value;
				FUeMcpAccessorErrorInfo AccErr;
				if (FUeMcpPropertyAccessor::GetValue(Cdo, P, Value, AccErr)
					&& Value.Json.IsValid())
				{
					Defaults->SetField(P, Value.Json);
				}
				else
				{
					MissingPaths.Add(P);
				}
			}
		}
		else
		{
			// Top-level snapshot — include all non-default-vs-... wait,
			// this IS the CDO, so "defaults" is what we read directly.
			// Pass bIncludeDefaults=true so ReadTopLevelSnapshot doesn't
			// try to diff the CDO against itself (which would emit an
			// empty object).
			ReadTopLevelSnapshot(Cdo, /*bIncludeDefaults=*/true, Defaults);
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("class_path"),
			Cls ? Cls->GetPathName() : FString());
		Data->SetStringField(TEXT("class"),
			Cls ? Cls->GetName() : FString());
		Data->SetObjectField(TEXT("defaults"), Defaults);
		if (BP != nullptr)
		{
			Data->SetStringField(TEXT("asset_path"), BP->GetPathName());
		}

		if (RequestedPaths.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ReqValues;
			ReqValues.Reserve(RequestedPaths.Num());
			for (const FString& P : RequestedPaths)
			{
				ReqValues.Add(MakeShared<FJsonValueString>(P));
			}
			Data->SetArrayField(TEXT("requested_paths"), ReqValues);

			TArray<TSharedPtr<FJsonValue>> MissValues;
			MissValues.Reserve(MissingPaths.Num());
			for (const FString& P : MissingPaths)
			{
				MissValues.Add(MakeShared<FJsonValueString>(P));
			}
			Data->SetArrayField(TEXT("missing_paths"), MissValues);
		}
		return Data;
	}
}

void UeMcp::RegisterIntrospectionHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpIntrospectionHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.summary"));
		Reg.DefaultTimeoutSeconds = SummaryTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorSummary);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("actor.properties"));
		Reg.DefaultTimeoutSeconds = PropertiesTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleActorProperties);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.outline"));
		Reg.DefaultTimeoutSeconds = OutlineTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintOutline);
		Dispatcher.RegisterTool(Reg);
	}

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("cdo.defaults"));
		Reg.DefaultTimeoutSeconds = CdoTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleCdoDefaults);
		Dispatcher.RegisterTool(Reg);
	}
}
