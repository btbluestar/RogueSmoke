// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// Stage 2 of the Blueprint graph surface — AUTHORING handlers.
//
// Six mutating tools that let an agent programmatically build BP graphs:
// add a K2Node, connect/disconnect pins, set a pin's literal default,
// remove a node, and compile (with optional save-on-compile). Sibling to
// the stage-1 reader in `UeMcpBlueprintGraphHandler.cpp`; we deliberately
// duplicate its blueprint-resolver + graph-lookup patterns here rather
// than lifting a shared header (same convention as
// `UeMcpFunctionalTestHandlers.cpp:54-58`). The two files are the
// natural unit of change and drift between them is the kind of bug the
// compiler catches.
//
// Design constraints (load-bearing):
//   * Every handler runs on the game thread (`check(IsInGameThread())`).
//   * Mutations bracket: `BP->Modify()` + `Graph->Modify()` BEFORE,
//     `FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified` AFTER.
//     Anything else leaves undo/redo broken.
//   * Never spin-wait on the game thread (`docs/ue-api-gotchas.md §1`).
//   * K2Node creation goes through `FEdGraphSchemaAction_NewNode::CreateNode`
//     (i.e. the engine's node-construction path) so post-paste callbacks
//     and `AllocateDefaultPins` fire and pins materialise correctly.
//   * Error-helper naming: never `MakeError` — `TValueOrError::MakeError`
//     shadows it (`docs/ue-api-gotchas.md §7`). We use `UeMcp::MakeInlineError`.

#include "UeMcpBlueprintAuthoringHandler.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraph/EdGraphSchema.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/MemberReference.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/World.h"
#include "Editor.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "K2Node.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_EditablePinBase.h"
#include "K2Node_Event.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_MacroInstance.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "UeMcpPropertyAccessor.h"
#include "UeMcpPropertyValue.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Logging/TokenizedMessage.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpBlueprintAuthoringHandlerPrivate
{
	// ----- timeouts --------------------------------------------------

	/** Authoring ops are fast object touches; 20s mirrors the stage-1
	 *  reader and gives us plenty of headroom for anything short of
	 *  compile. */
	static constexpr double AuthoringDefaultTimeoutSeconds = 20.0;

	/** Compile is bounded by the blueprint's size and the class graph of
	 *  whatever it references. 60s is the `handler-conventions.md §5`
	 *  default for write ops; we bump higher on the theory that agents
	 *  will sometimes compile a BP that depends on a freshly-edited
	 *  parent. Saves the caller a second retry. */
	static constexpr double CompileTimeoutSeconds = 60.0;

	// ----- blueprint-resolver (duplicated from stage-1 reader) -------

	/**
	 * Resolve a user string to `(UBlueprint*, UClass*)`. We only care
	 * about BP assets here; native classes are not authorable and we
	 * reject with TYPE_MISMATCH rather than special-casing them further.
	 *
	 * This mirrors `UeMcpBlueprintGraphHandler::ResolveBlueprintAsset`
	 * deliberately. Both files sit behind the same `blueprint.graph*`
	 * agent-facing contract and duplication is cheaper than a shared
	 * helper header right now (see file header).
	 *
	 * Passes a nullptr world hint — asset resolution only. A BP path
	 * should never match an actor label.
	 */
	static UBlueprint* ResolveBlueprint(
		const FString& ObjectId, TSharedPtr<FJsonObject>& OutErr)
	{
		UeMcp::FUeMcpResolvedObject Resolved = UeMcp::ResolveObject(ObjectId, nullptr);
		if (!Resolved.IsOk())
		{
			OutErr = Resolved.ErrorInfo;
			return nullptr;
		}

		if (UBlueprint* BP = Cast<UBlueprint>(Resolved.Object))
		{
			return BP;
		}

		// A `_C` class path resolves to a UClass; walk back through
		// `ClassGeneratedBy` to the authoring UBlueprint. Matches the
		// stage-1 reader's ergonomic behaviour — callers pass whatever
		// path shape they have handy.
		if (UClass* Cls = Cast<UClass>(Resolved.Object))
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Cls->ClassGeneratedBy))
			{
				return BP;
			}
		}

		// Level Blueprint support. A level asset (`/Game/Maps/Foo`) resolves
		// to a UWorld; its level script BP is a subobject of PersistentLevel.
		// Authoring mode — pass `bDontCreate=false` so an agent can edit a
		// level that has no level BP yet. The reader counterpart uses
		// `bDontCreate=true` so read-only calls don't side-effect the asset.
		if (UWorld* World = Cast<UWorld>(Resolved.Object))
		{
			if (ULevel* Persistent = World->PersistentLevel)
			{
				if (UBlueprint* LSB = Persistent->GetLevelScriptBlueprint(
						/*bDontCreate=*/ false))
				{
					return LSB;
				}
			}
		}

		OutErr = UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
			FString::Printf(
				TEXT("'%s' did not resolve to an editable UBlueprint asset"),
				*ObjectId));
		return nullptr;
	}

	// ----- graph-by-name (duplicated from stage-1 reader) ------------

	/**
	 * Walk all four graph buckets on a UBlueprint in the same order the
	 * stage-1 reader uses and return the first matching graph by name.
	 *
	 * Keeping the bucket traversal order identical to the reader means
	 * `blueprint.graph` and `blueprint.graph.*` authoring tools agree on
	 * ties — agents never see a write land on a different graph than the
	 * one their most recent read reported. (Name collisions across
	 * buckets are an authoring mistake the editor already flags.)
	 */
	static UEdGraph* FindGraphByName(UBlueprint* BP, const FString& TargetName)
	{
		if (BP == nullptr)
		{
			return nullptr;
		}
		// UE 5.x UBlueprint arrays are TArray<TObjectPtr<UEdGraph>> — each
		// bucket iterated directly (the range-for performs the implicit
		// conversion to UEdGraph*). Taking TArray<UEdGraph*>* pointers
		// would be a type mismatch.
		for (UEdGraph* G : BP->FunctionGraphs)
		{
			if (G != nullptr && G->GetName() == TargetName) { return G; }
		}
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (G != nullptr && G->GetName() == TargetName) { return G; }
		}
		for (UEdGraph* G : BP->MacroGraphs)
		{
			if (G != nullptr && G->GetName() == TargetName) { return G; }
		}
		for (UEdGraph* G : BP->DelegateSignatureGraphs)
		{
			if (G != nullptr && G->GetName() == TargetName) { return G; }
		}
		return nullptr;
	}

	/**
	 * Find a node on a graph by its stable wire id (which is
	 * `UEdGraphNode::GetName()`). Case-sensitive — ids round-trip through
	 * the read side verbatim. Returns nullptr if not found; callers map
	 * to NOT_FOUND.
	 */
	static UEdGraphNode* FindNodeById(UEdGraph* Graph, const FString& NodeId)
	{
		if (Graph == nullptr || NodeId.IsEmpty())
		{
			return nullptr;
		}
		for (UEdGraphNode* N : Graph->Nodes)
		{
			if (N != nullptr && N->GetName() == NodeId)
			{
				return N;
			}
		}
		return nullptr;
	}

	/**
	 * Find a pin on a node by case-sensitive name match. Pin names are
	 * stable within the node and match what the read side emits. Returns
	 * nullptr if not found.
	 */
	static UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName)
	{
		if (Node == nullptr)
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin != nullptr && Pin->PinName.ToString() == PinName)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/**
	 * Build a compact pin summary for `add_node`'s response. We don't
	 * re-emit the full stage-1 pin schema (that would have callers pay
	 * the link-traversal cost on a freshly-made node with no links);
	 * just enough for the caller to know what pins exist so the next
	 * `connect_pins` call can target them.
	 */
	static TSharedRef<FJsonObject> BuildPinSummary(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Pin == nullptr)
		{
			Out->SetStringField(TEXT("name"),        FString());
			Out->SetStringField(TEXT("direction"),   TEXT("in"));
			Out->SetStringField(TEXT("category"),    FString());
			Out->SetStringField(TEXT("subcategory"), FString());
			return Out;
		}
		Out->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Out->SetStringField(TEXT("direction"),
			(Pin->Direction == EGPD_Output) ? TEXT("out") : TEXT("in"));
		Out->SetStringField(TEXT("category"),
			Pin->PinType.PinCategory.IsNone() ? FString() : Pin->PinType.PinCategory.ToString());
		Out->SetStringField(TEXT("subcategory"),
			Pin->PinType.PinSubCategory.IsNone() ? FString() : Pin->PinType.PinSubCategory.ToString());
		return Out;
	}

	/**
	 * Emit the stage-1-format array of `{name, direction, category, subcategory}`
	 * for every pin on a node.
	 */
	static TArray<TSharedPtr<FJsonValue>> BuildPinSummaryArray(const UEdGraphNode* Node)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (Node == nullptr)
		{
			return Out;
		}
		Out.Reserve(Node->Pins.Num());
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			Out.Add(MakeShared<FJsonValueObject>(BuildPinSummary(Pin)));
		}
		return Out;
	}

	// ----- helpers for bracketing mutations --------------------------

	/**
	 * Call BEFORE a mutation. Marks the BP transactable (for undo/redo
	 * in the editor UI, which even MCP-driven flows respect) and marks
	 * the graph transactable as well. Without this, an editor-side
	 * Ctrl+Z silently skips our edits instead of reverting them.
	 */
	static void BeginBlueprintMutation(UBlueprint* BP, UEdGraph* Graph)
	{
		if (BP != nullptr)
		{
			BP->Modify();
		}
		if (Graph != nullptr)
		{
			Graph->Modify();
		}
	}

	/**
	 * Call AFTER a mutation. Sets the blueprint's modified flag, marks
	 * the owning package dirty (so the editor offers to save), and kicks
	 * the structural-modification delegate chain that e.g. refreshes
	 * open blueprint editors.
	 *
	 * `bStructural=false` skips the structural bump — used for
	 * `set_pin_default`, which changes literal values without altering
	 * graph topology.
	 */
	static void EndBlueprintMutation(UBlueprint* BP, bool bStructural = true)
	{
		if (BP == nullptr)
		{
			return;
		}
		if (UPackage* Pkg = BP->GetOutermost())
		{
			Pkg->MarkPackageDirty();
		}
		if (bStructural)
		{
			FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		}
		else
		{
			FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
		}
	}

	// ----- supported K2Node classes ----------------------------------

	/** The stable list of node classes `add_node` accepts. Kept here as
	 *  one source of truth so the error-payload `supported_classes` array
	 *  and the dispatch switch can't drift. */
	static const TCHAR* const SupportedNodeClasses[] = {
		TEXT("K2Node_CallFunction"),
		TEXT("K2Node_VariableGet"),
		TEXT("K2Node_VariableSet"),
		TEXT("K2Node_IfThenElse"),
		TEXT("K2Node_ExecutionSequence"),
		TEXT("K2Node_Event"),
		TEXT("K2Node_CustomEvent"),
		TEXT("K2Node_ComponentBoundEvent"),
		TEXT("K2Node_MacroInstance"),
		TEXT("K2Node_Knot"),
	};

	// Forward decl — defined with blueprint.add_variable much further down
	// in the same anon namespace.
	static bool ParseVariableType(
		const FString& TypeStr, FEdGraphPinType& OutPinType, FString& OutError);

	/** Build the JSON array of supported class names for a SUPPORTED_SET
	 *  error detail. */
	static TArray<TSharedPtr<FJsonValue>> BuildSupportedClassesArray()
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(UE_ARRAY_COUNT(SupportedNodeClasses));
		for (const TCHAR* Name : SupportedNodeClasses)
		{
			Out.Add(MakeShared<FJsonValueString>(FString(Name)));
		}
		return Out;
	}

	/**
	 * Attach a node to a graph via the engine's node-spawn path. We use
	 * `FEdGraphSchemaAction_NewNode::SpawnNodeFromTemplate` so the
	 * schema's `PostPlacedNewNode` + `AllocateDefaultPins` fire. A raw
	 * `NewObject<>` + `Graph->AddNode` would skip those and leave the
	 * node pinless — the most common failure mode in prior-art UE MCPs.
	 *
	 * Template ownership: we `NewObject` a template under the graph's
	 * outer package (the blueprint asset), pass it to `SpawnNodeFromTemplate`,
	 * and receive the same pointer back. The template IS the finalised
	 * node — `CreateNode` reparents and finalises in place.
	 */
	template<class TNode>
	static TNode* SpawnNode(UEdGraph* Graph, int32 PosX, int32 PosY)
	{
		if (Graph == nullptr)
		{
			return nullptr;
		}
		TNode* Template = NewObject<TNode>(Graph);
		return FEdGraphSchemaAction_NewNode::SpawnNodeFromTemplate<TNode>(
			Graph, Template, FVector2f(static_cast<float>(PosX), static_cast<float>(PosY)),
			/*bSelectNewNode=*/false);
	}

	/**
	 * Read `{x, y}` out of an optional `position` object. Defaults to
	 * `(0, 0)`. Positioning is cosmetic — the editor's "straighten
	 * wires" command re-lays-out graphs anyway.
	 */
	static void ReadPosition(
		const TSharedRef<FJsonObject>& Args, int32& OutX, int32& OutY)
	{
		OutX = 0;
		OutY = 0;
		const TSharedPtr<FJsonObject>* PosObj = nullptr;
		if (!Args->TryGetObjectField(TEXT("position"), PosObj) || !PosObj->IsValid())
		{
			return;
		}
		int32 X = 0, Y = 0;
		if ((*PosObj)->TryGetNumberField(TEXT("x"), X))
		{
			OutX = X;
		}
		if ((*PosObj)->TryGetNumberField(TEXT("y"), Y))
		{
			OutY = Y;
		}
	}

	/**
	 * Build the `rollback: {tool, args}` payload for `add_node`. The
	 * natural inverse is `remove_node(bp_path, graph_name, node_id)`.
	 */
	static TSharedRef<FJsonObject> BuildAddNodeRollback(
		const FString& BpPath, const FString& GraphName, const FString& NodeId)
	{
		TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("bp_path"),    BpPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("node_id"),    NodeId);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("tool"), TEXT("blueprint.graph.remove_node"));
		Out->SetObjectField(TEXT("args"), Args);
		return Out;
	}

	/**
	 * Build the `rollback` payload for `connect_pins` (inverse:
	 * `disconnect_pins` with the same endpoints).
	 */
	static TSharedRef<FJsonObject> BuildConnectRollback(
		const FString& BpPath, const FString& GraphName,
		const FString& FromNode, const FString& FromPin,
		const FString& ToNode,   const FString& ToPin)
	{
		TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("bp_path"),    BpPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("from_node"),  FromNode);
		Args->SetStringField(TEXT("from_pin"),   FromPin);
		Args->SetStringField(TEXT("to_node"),    ToNode);
		Args->SetStringField(TEXT("to_pin"),     ToPin);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("tool"), TEXT("blueprint.graph.disconnect_pins"));
		Out->SetObjectField(TEXT("args"), Args);
		return Out;
	}

	/**
	 * Build the `rollback` payload for `disconnect_pins` (inverse:
	 * `connect_pins` with the same endpoints).
	 */
	static TSharedRef<FJsonObject> BuildDisconnectRollback(
		const FString& BpPath, const FString& GraphName,
		const FString& FromNode, const FString& FromPin,
		const FString& ToNode,   const FString& ToPin)
	{
		TSharedRef<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("bp_path"),    BpPath);
		Args->SetStringField(TEXT("graph_name"), GraphName);
		Args->SetStringField(TEXT("from_node"),  FromNode);
		Args->SetStringField(TEXT("from_pin"),   FromPin);
		Args->SetStringField(TEXT("to_node"),    ToNode);
		Args->SetStringField(TEXT("to_pin"),     ToPin);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("tool"), TEXT("blueprint.graph.connect_pins"));
		Out->SetObjectField(TEXT("args"), Args);
		return Out;
	}

	// =================================================================
	//   blueprint.graph.add_node
	// =================================================================

	/**
	 * Finalise a freshly-spawned node into the success response shape
	 * documented in the tool contract. Shared across all node-class
	 * branches of `add_node` so the shape is stable.
	 */
	static TSharedRef<FJsonObject> BuildAddNodeResponse(
		UEdGraphNode* Node,
		const FString& BpPath, const FString& GraphName)
	{
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("added"), true);
		Data->SetStringField(TEXT("node_id"),
			Node ? Node->GetName() : FString());
		Data->SetStringField(TEXT("class"),
			(Node && Node->GetClass()) ? Node->GetClass()->GetName() : FString());
		Data->SetStringField(TEXT("title"),
			Node ? Node->GetNodeTitle(ENodeTitleType::ListView).ToString() : FString());
		Data->SetArrayField(TEXT("pins"), BuildPinSummaryArray(Node));
		Data->SetStringField(TEXT("graph_name"), GraphName);
		Data->SetObjectField(TEXT("rollback"),
			BuildAddNodeRollback(BpPath, GraphName,
				Node ? Node->GetName() : FString()));
		return Data;
	}

	/**
	 * Add a `K2Node_CallFunction` resolved via
	 * `config.function_reference.{class_path, function_name}`.
	 *
	 * Why `SetFromFunction` + explicit `AllocateDefaultPins` here even
	 * though `SpawnNodeFromTemplate` already called the latter: the
	 * default allocation happens with an empty function reference (the
	 * node's reference is still the default-constructed `FMemberReference`),
	 * which means no parameters surface. We set the reference afterward
	 * and re-allocate so agents see the real pin set.
	 */
	static TSharedRef<FJsonObject> HandleAddCallFunction(
		UEdGraph* Graph, const TSharedPtr<FJsonObject>& Config,
		int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		if (!Config.IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("K2Node_CallFunction requires `config.function_reference`"));
		}
		const TSharedPtr<FJsonObject>* FuncRefObj = nullptr;
		if (!Config->TryGetObjectField(TEXT("function_reference"), FuncRefObj)
			|| !FuncRefObj->IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("K2Node_CallFunction requires `config.function_reference`"));
		}

		FString ClassPath, FunctionName;
		if (!(*FuncRefObj)->TryGetStringField(TEXT("class_path"), ClassPath)
			|| ClassPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`config.function_reference.class_path` is required"));
		}
		if (!(*FuncRefObj)->TryGetStringField(TEXT("function_name"), FunctionName)
			|| FunctionName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`config.function_reference.function_name` is required"));
		}

		// Resolve the UClass. Accepts a `/Script/...` or `/Game/....C`
		// path; for BP paths without the `_C` suffix we rely on the
		// resolver's strategy-4 append to reach the generated class.
		TSharedPtr<FJsonObject> ErrJson;
		UeMcp::FUeMcpResolvedObject ClassResolved =
			UeMcp::ResolveObject(ClassPath, nullptr);
		if (!ClassResolved.IsOk())
		{
			// Surface the resolver's structured error directly; it
			// already carries the `searched_strategies` detail.
			return ClassResolved.ErrorInfo.ToSharedRef();
		}
		UClass* TargetClass = Cast<UClass>(ClassResolved.Object);
		if (TargetClass == nullptr)
		{
			if (UBlueprint* BP = Cast<UBlueprint>(ClassResolved.Object))
			{
				TargetClass = BP->GeneratedClass;
			}
		}
		if (TargetClass == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("'%s' did not resolve to a UClass"),
					*ClassPath));
		}

		// Name-based lookup: `UClass::FindFunctionByName` walks the
		// class hierarchy, so this finds parent-inherited functions too
		// (the common case for e.g. `PrintString` on
		// `KismetSystemLibrary`).
		UFunction* Fn = TargetClass->FindFunctionByName(FName(*FunctionName));
		if (Fn == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PROP_NOT_FOUND"),
				FString::Printf(
					TEXT("Function '%s' not found on class '%s'"),
					*FunctionName, *TargetClass->GetName()));
		}

		// Wildcard-array functions (everything in KismetArrayLibrary,
		// plus any UFUNCTION annotated with ArrayParm metadata) need the
		// specialised K2Node_CallArrayFunction subclass so the wildcard-
		// propagation override fires on connect. Spawning the plain
		// K2Node_CallFunction leaves pins stuck at `wildcard` even with
		// the connect_pins PinConnectionListChanged fix — the override
		// that reads the connected array's type lives on the subclass.
		// Engine metadata key is `"ArrayParm"` (truncated, no second 'a').
		// The constant is FBlueprintMetadata::MD_ArrayParam at
		// EdGraphSchema_K2.cpp:249 — variable name is misleading.
		const bool bIsArrayFunction = Fn->HasMetaData(TEXT("ArrayParm"));

		UK2Node_CallFunction* Node = bIsArrayFunction
			? SpawnNode<UK2Node_CallArrayFunction>(Graph, PosX, PosY)
			: SpawnNode<UK2Node_CallFunction>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("K2Node_CallFunction spawn returned nullptr"));
		}

		// Wire the function reference, then re-allocate so the
		// input/output pins reflect the function's parameter list.
		Node->SetFromFunction(Fn);
		Node->AllocateDefaultPins();
		Node->ReconstructNode();

		return BuildAddNodeResponse(Node, BpPath, GraphName);
	}

	/**
	 * Add a variable-get or variable-set node bound to a BP-local
	 * variable (or, with `variable_class_path`, to a variable on an
	 * arbitrary class).
	 *
	 * The engine models these identically: both derive from `UK2Node_Variable`
	 * and both use `SetFromProperty` to bind. We branch only on the
	 * spawned type — pin construction is symmetric.
	 */
	template<class TVariableNode>
	static TSharedRef<FJsonObject> HandleAddVariableNode(
		UEdGraph* Graph, UBlueprint* BP,
		const TSharedPtr<FJsonObject>& Config,
		int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		if (!Config.IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("variable node requires `config.variable_name`"));
		}
		FString VariableName;
		if (!Config->TryGetStringField(TEXT("variable_name"), VariableName)
			|| VariableName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`config.variable_name` is required"));
		}

		// Default: the BP's own generated class. Override lets callers
		// reference a variable on a parent class or an unrelated class
		// entirely (unusual but occasionally useful for agent-driven
		// refactors).
		UClass* OwnerClass = BP->GeneratedClass;
		bool bSelfContext = true;

		FString OverrideClassPath;
		if (Config->TryGetStringField(TEXT("variable_class_path"), OverrideClassPath)
			&& !OverrideClassPath.IsEmpty())
		{
			UeMcp::FUeMcpResolvedObject ClassResolved =
				UeMcp::ResolveObject(OverrideClassPath, nullptr);
			if (!ClassResolved.IsOk())
			{
				return ClassResolved.ErrorInfo.ToSharedRef();
			}
			if (UClass* C = Cast<UClass>(ClassResolved.Object))
			{
				OwnerClass = C;
			}
			else if (UBlueprint* B = Cast<UBlueprint>(ClassResolved.Object))
			{
				OwnerClass = B->GeneratedClass;
			}
			else
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					FString::Printf(
						TEXT("'%s' did not resolve to a class"),
						*OverrideClassPath));
			}
			// Self-context is only true when the variable lives on the
			// authoring BP's generated class — otherwise the editor
			// treats it as an external member and renders the `Target`
			// pin accordingly.
			bSelfContext = (OwnerClass == BP->GeneratedClass);
		}

		FProperty* Prop = OwnerClass
			? FindFProperty<FProperty>(OwnerClass, FName(*VariableName))
			: nullptr;
		if (Prop == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PROP_NOT_FOUND"),
				FString::Printf(
					TEXT("Variable '%s' not found on class '%s'"),
					*VariableName,
					OwnerClass ? *OwnerClass->GetName() : TEXT("<null>")));
		}

		TVariableNode* Node = SpawnNode<TVariableNode>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("variable node spawn returned nullptr"));
		}

		Node->SetFromProperty(Prop, bSelfContext, OwnerClass);
		Node->AllocateDefaultPins();
		Node->ReconstructNode();

		return BuildAddNodeResponse(Node, BpPath, GraphName);
	}

	/**
	 * Add a control-flow branch node. No config needed — the schema's
	 * default pin allocation is already what the editor's node palette
	 * drops in.
	 */
	static TSharedRef<FJsonObject> HandleAddIfThenElse(
		UEdGraph* Graph, int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		UK2Node_IfThenElse* Node = SpawnNode<UK2Node_IfThenElse>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("K2Node_IfThenElse spawn returned nullptr"));
		}
		return BuildAddNodeResponse(Node, BpPath, GraphName);
	}

	/**
	 * Add a sequence node. Default pin set is `{Exec in, Then 0, Then 1}`;
	 * when `outputs_count > 2` we call `AddInputPin` repeatedly (its name
	 * is misleading — the engine treats the additional THEN-pins as
	 * "input pins" to the sequence node's own state machine). Values ≤ 2
	 * are a no-op.
	 */
	static TSharedRef<FJsonObject> HandleAddExecutionSequence(
		UEdGraph* Graph, const TSharedPtr<FJsonObject>& Config,
		int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		int32 OutputsCount = 2;
		if (Config.IsValid())
		{
			int32 Parsed = 0;
			if (Config->TryGetNumberField(TEXT("outputs_count"), Parsed) && Parsed > 0)
			{
				// Cap at something sane — we've never seen an author
				// need more than a dozen sequence branches, and runaway
				// values would bloat the response.
				OutputsCount = FMath::Clamp(Parsed, 2, 16);
			}
		}

		UK2Node_ExecutionSequence* Node =
			SpawnNode<UK2Node_ExecutionSequence>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("K2Node_ExecutionSequence spawn returned nullptr"));
		}

		// Default is 2 outputs; add more as requested. `AddInputPin`
		// internally bumps `NumAdditionalInputs` and re-syncs pins.
		const int32 Extras = FMath::Max(0, OutputsCount - 2);
		for (int32 i = 0; i < Extras; ++i)
		{
			Node->AddInputPin();
		}

		return BuildAddNodeResponse(Node, BpPath, GraphName);
	}

	/**
	 * Add an Event node (e.g. `ReceiveBeginPlay`). We set the
	 * `EventReference` to the parent-class function matching
	 * `event_name` and mark it as an override. Duplicate events on the
	 * same BP are refused — the editor itself disallows them.
	 */
	static TSharedRef<FJsonObject> HandleAddEvent(
		UEdGraph* Graph, UBlueprint* BP,
		const TSharedPtr<FJsonObject>& Config,
		int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		if (!Config.IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("K2Node_Event requires `config.event_name`"));
		}
		FString EventName;
		if (!Config->TryGetStringField(TEXT("event_name"), EventName)
			|| EventName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`config.event_name` is required"));
		}

		// Determine the class on which the event is declared. Default
		// to the BP's parent; `override_function_class_path` lets
		// callers reach down into a grandparent or an implemented
		// interface explicitly.
		UClass* OverrideClass = BP->ParentClass;
		FString OverrideClassPath;
		if (Config->TryGetStringField(TEXT("override_function_class_path"),
			OverrideClassPath) && !OverrideClassPath.IsEmpty())
		{
			UeMcp::FUeMcpResolvedObject ClassResolved =
				UeMcp::ResolveObject(OverrideClassPath, nullptr);
			if (!ClassResolved.IsOk())
			{
				return ClassResolved.ErrorInfo.ToSharedRef();
			}
			if (UClass* C = Cast<UClass>(ClassResolved.Object))
			{
				OverrideClass = C;
			}
			else
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					FString::Printf(
						TEXT("'%s' did not resolve to a class"),
						*OverrideClassPath));
			}
		}

		if (OverrideClass == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("Blueprint has no parent class to bind event against"));
		}
		UFunction* Fn = OverrideClass->FindFunctionByName(FName(*EventName));
		if (Fn == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PROP_NOT_FOUND"),
				FString::Printf(
					TEXT("Event '%s' not found on class '%s'"),
					*EventName, *OverrideClass->GetName()));
		}

		// Duplicate check. The editor shows a red "event already
		// implemented" node and disables placement; we refuse here with
		// a structured error rather than synthesize a broken node.
		for (UEdGraph* Ubergraph : BP->UbergraphPages)
		{
			if (Ubergraph == nullptr)
			{
				continue;
			}
			for (UEdGraphNode* N : Ubergraph->Nodes)
			{
				if (UK2Node_Event* Existing = Cast<UK2Node_Event>(N))
				{
					if (Existing->GetFunctionName() == FName(*EventName))
					{
						return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
							FString::Printf(
								TEXT("Event '%s' already exists in blueprint '%s'"),
								*EventName, *BP->GetName()));
					}
				}
			}
		}

		UK2Node_Event* Node = SpawnNode<UK2Node_Event>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("K2Node_Event spawn returned nullptr"));
		}

		Node->EventReference.SetExternalMember(FName(*EventName), OverrideClass);
		Node->bOverrideFunction = true;
		Node->AllocateDefaultPins();
		Node->ReconstructNode();

		return BuildAddNodeResponse(Node, BpPath, GraphName);
	}

	// =================================================================
	//   blueprint.graph.add_node — K2Node_CustomEvent branch (v1.1 Wave B)
	// =================================================================

	/**
	 * Add a `K2Node_CustomEvent`. Unlike `K2Node_Event`, custom events
	 * are BP-author-defined entry points — their signature is whatever
	 * the author declares via `config.parameters`. We bind
	 * `CustomFunctionName` directly and set `bOverrideFunction = false`
	 * so the node is NOT treated as an override of a parent UFUNCTION.
	 *
	 * UE convention: a custom event's *input* parameters surface as
	 * *output* pins on the event node (the node is the function entry;
	 * execution flows out of it). We therefore pass `EGPD_Output` to
	 * `CreateUserDefinedPin` for each declared parameter.
	 */
	static TSharedRef<FJsonObject> HandleAddCustomEvent(
		UEdGraph* Graph, UBlueprint* BP,
		const TSharedPtr<FJsonObject>& Config,
		int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		if (!Config.IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("K2Node_CustomEvent requires `config.event_name`"));
		}
		FString EventName;
		if (!Config->TryGetStringField(TEXT("event_name"), EventName)
			|| EventName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`config.event_name` is required"));
		}

		const FName EventFName(*EventName);

		// Duplicate check across all ubergraphs — matches HandleAddEvent.
		for (UEdGraph* Ubergraph : BP->UbergraphPages)
		{
			if (Ubergraph == nullptr) { continue; }
			for (UEdGraphNode* N : Ubergraph->Nodes)
			{
				if (UK2Node_CustomEvent* Existing = Cast<UK2Node_CustomEvent>(N))
				{
					if (Existing->CustomFunctionName == EventFName)
					{
						return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
							FString::Printf(
								TEXT("Custom event '%s' already exists in blueprint '%s'"),
								*EventName, *BP->GetName()));
					}
				}
			}
		}

		// Parse parameters BEFORE spawning so a bad type string fails
		// without leaving an orphan node in the graph.
		struct FParsedParam
		{
			FName           Name;
			FEdGraphPinType PinType;
			bool            bHasDefault = false;
			FString         DefaultValue;
		};
		TArray<FParsedParam> ParsedParams;

		const TArray<TSharedPtr<FJsonValue>>* ParamsArray = nullptr;
		if (Config->TryGetArrayField(TEXT("parameters"), ParamsArray)
			&& ParamsArray != nullptr)
		{
			ParsedParams.Reserve(ParamsArray->Num());
			for (int32 i = 0; i < ParamsArray->Num(); ++i)
			{
				const TSharedPtr<FJsonValue>& Entry = (*ParamsArray)[i];
				if (!Entry.IsValid() || Entry->Type != EJson::Object)
				{
					return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
						FString::Printf(
							TEXT("`config.parameters[%d]` must be an object"), i));
				}
				const TSharedPtr<FJsonObject>& Obj = Entry->AsObject();

				FString ParamName, ParamType;
				if (!Obj->TryGetStringField(TEXT("name"), ParamName)
					|| ParamName.IsEmpty())
				{
					return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
						FString::Printf(
							TEXT("`config.parameters[%d].name` is required"), i));
				}
				if (!Obj->TryGetStringField(TEXT("type"), ParamType)
					|| ParamType.IsEmpty())
				{
					return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
						FString::Printf(
							TEXT("`config.parameters[%d].type` is required"), i));
				}

				FParsedParam P;
				P.Name = FName(*ParamName);
				FString TypeErr;
				if (!ParseVariableType(ParamType, P.PinType, TypeErr))
				{
					return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
						FString::Printf(
							TEXT("`config.parameters[%d].type`: %s"),
							i, *TypeErr));
				}

				FString DefaultStr;
				if (Obj->TryGetStringField(TEXT("default"), DefaultStr))
				{
					P.bHasDefault = true;
					P.DefaultValue = DefaultStr;
				}
				ParsedParams.Add(MoveTemp(P));
			}
		}

		UK2Node_CustomEvent* Node =
			SpawnNode<UK2Node_CustomEvent>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("K2Node_CustomEvent spawn returned nullptr"));
		}

		Node->CustomFunctionName = EventFName;
		Node->bOverrideFunction  = false;
		Node->AllocateDefaultPins();

		for (const FParsedParam& P : ParsedParams)
		{
			UEdGraphPin* UserPin = Node->CreateUserDefinedPin(
				P.Name, P.PinType, EGPD_Output, /*bUseUniqueName=*/false);
			if (UserPin == nullptr)
			{
				return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
					FString::Printf(
						TEXT("CreateUserDefinedPin returned nullptr for '%s'"),
						*P.Name.ToString()));
			}
			if (P.bHasDefault)
			{
				UserPin->DefaultValue = P.DefaultValue;
			}
		}

		Node->ReconstructNode();

		TSharedRef<FJsonObject> Data = BuildAddNodeResponse(Node, BpPath, GraphName);
		Data->SetStringField(TEXT("event_name"), EventName);
		return Data;
	}

	// =================================================================
	//   blueprint.graph.add_node — K2Node_ComponentBoundEvent (v1.1 Wave B)
	// =================================================================

	/**
	 * Add a `K2Node_ComponentBoundEvent` — the node the editor spawns
	 * when you right-click a multicast delegate on a component and pick
	 * "Add ... event". Requires that the BP already expose the named
	 * component as an `FObjectProperty` on its generated class (i.e.
	 * either a native parent-class component or an SCS node;
	 * `blueprint.add_component` is Wave C).
	 */
	static TSharedRef<FJsonObject> HandleAddComponentBoundEvent(
		UEdGraph* Graph, UBlueprint* BP,
		const TSharedPtr<FJsonObject>& Config,
		int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		if (!Config.IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("K2Node_ComponentBoundEvent requires "
					 "`config.component_property_name` and `config.delegate_property_name`"));
		}
		FString ComponentPropName;
		if (!Config->TryGetStringField(TEXT("component_property_name"), ComponentPropName)
			|| ComponentPropName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`config.component_property_name` is required"));
		}
		FString DelegatePropName;
		if (!Config->TryGetStringField(TEXT("delegate_property_name"), DelegatePropName)
			|| DelegatePropName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`config.delegate_property_name` is required"));
		}

		// Resolve the component property on the BP's generated class.
		UClass* SearchClass = BP->SkeletonGeneratedClass
			? BP->SkeletonGeneratedClass
			: BP->GeneratedClass;
		if (SearchClass == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("Blueprint has no generated class; compile before adding bound events"));
		}

		FProperty* RawCompProp =
			SearchClass->FindPropertyByName(FName(*ComponentPropName));
		FObjectProperty* ComponentProp = CastField<FObjectProperty>(RawCompProp);
		if (ComponentProp == nullptr
			|| ComponentProp->PropertyClass == nullptr
			|| !ComponentProp->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
		{
			TSharedRef<FJsonObject> ErrOut = UeMcp::MakeInlineError(
				RawCompProp == nullptr ? TEXT("PROP_NOT_FOUND") : TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("Component property '%s' not found on '%s' "
						 "(or not a UActorComponent-typed FObjectProperty)"),
					*ComponentPropName, *SearchClass->GetName()));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("bp_path"), BpPath);
			Detail->SetStringField(TEXT("component_property"), ComponentPropName);
			if (RawCompProp != nullptr)
			{
				Detail->SetStringField(TEXT("actual_type"), RawCompProp->GetCPPType());
			}
			Detail->SetStringField(TEXT("hint"),
				TEXT("blueprint.add_component is v1.1 Wave C; until then the "
					 "component must be declared on the native parent class"));
			ErrOut->SetObjectField(TEXT("detail"), Detail);
			return ErrOut;
		}

		UClass* ComponentClass = ComponentProp->PropertyClass;

		// Resolve the multicast delegate on the component's class.
		FProperty* RawDelegateProp =
			ComponentClass->FindPropertyByName(FName(*DelegatePropName));
		FMulticastDelegateProperty* DelegateProp =
			CastField<FMulticastDelegateProperty>(RawDelegateProp);
		if (DelegateProp == nullptr)
		{
			TSharedRef<FJsonObject> ErrOut = UeMcp::MakeInlineError(
				RawDelegateProp == nullptr ? TEXT("PROP_NOT_FOUND") : TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("Delegate property '%s' not found on component class '%s' "
						 "(or not a multicast delegate)"),
					*DelegatePropName, *ComponentClass->GetName()));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("bp_path"), BpPath);
			Detail->SetStringField(TEXT("component_property"), ComponentPropName);
			Detail->SetStringField(TEXT("delegate_property"), DelegatePropName);
			Detail->SetStringField(TEXT("component_class"), ComponentClass->GetName());
			ErrOut->SetObjectField(TEXT("detail"), Detail);
			return ErrOut;
		}

		// Duplicate-binding guard.
		const FName CompFName(*ComponentPropName);
		const FName DelFName(*DelegatePropName);
		for (UEdGraph* Ubergraph : BP->UbergraphPages)
		{
			if (Ubergraph == nullptr) { continue; }
			for (UEdGraphNode* N : Ubergraph->Nodes)
			{
				UK2Node_ComponentBoundEvent* Existing =
					Cast<UK2Node_ComponentBoundEvent>(N);
				if (Existing
					&& Existing->ComponentPropertyName == CompFName
					&& Existing->DelegatePropertyName == DelFName)
				{
					return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
						FString::Printf(
							TEXT("Event already bound for %s.%s in '%s'"),
							*ComponentPropName, *DelegatePropName, *BP->GetName()));
				}
			}
		}

		UK2Node_ComponentBoundEvent* Node =
			SpawnNode<UK2Node_ComponentBoundEvent>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("K2Node_ComponentBoundEvent spawn returned nullptr"));
		}

		Node->InitializeComponentBoundEventParams(ComponentProp, DelegateProp);
		Node->AllocateDefaultPins();
		Node->ReconstructNode();

		TSharedRef<FJsonObject> Data = BuildAddNodeResponse(Node, BpPath, GraphName);
		Data->SetStringField(TEXT("component"), ComponentPropName);
		Data->SetStringField(TEXT("delegate"), DelegatePropName);
		Data->SetStringField(TEXT("generated_event_name"),
			Node->CustomFunctionName.ToString());
		return Data;
	}

	// =================================================================
	//   blueprint.graph.add_node — K2Node_MacroInstance (v1.1 Wave B)
	// =================================================================

	/** Well-known macro libraries consulted, in order, when callers pass
	 *  the short `macro_name` form. StandardMacros covers ForEachLoop,
	 *  ForLoop, WhileLoop, IsValid, Gate, DoOnce, DoN, MultiGate. */
	static const TCHAR* const WellKnownMacroLibraries[] = {
		TEXT("/Engine/EditorBlueprintResources/StandardMacros.StandardMacros"),
	};

	/** Walk a macro-library blueprint's MacroGraphs and return the graph
	 *  whose GetName() matches. */
	static UEdGraph* FindMacroGraphByName(UBlueprint* Library, const FString& GraphName)
	{
		if (Library == nullptr) { return nullptr; }
		for (UEdGraph* G : Library->MacroGraphs)
		{
			if (G != nullptr && G->GetName() == GraphName)
			{
				return G;
			}
		}
		return nullptr;
	}

	/** Resolve `"/Path/To/Lib.Lib:GraphName"` into a macro graph. */
	static UEdGraph* ResolveMacroByPath(
		const FString& Path, TSharedPtr<FJsonObject>& OutErr)
	{
		FString PkgPath, GraphName;
		if (!Path.Split(TEXT(":"), &PkgPath, &GraphName)
			|| PkgPath.IsEmpty() || GraphName.IsEmpty())
		{
			OutErr = UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				FString::Printf(
					TEXT("macro_graph_path must be of form 'Package.Asset:GraphName', got '%s'"),
					*Path));
			return nullptr;
		}
		UBlueprint* Lib = LoadObject<UBlueprint>(nullptr, *PkgPath);
		if (Lib == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Macro library '%s' not found"), *PkgPath));
			return nullptr;
		}
		UEdGraph* G = FindMacroGraphByName(Lib, GraphName);
		if (G == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("Macro library '%s' has no macro graph named '%s'"),
					*PkgPath, *GraphName));
			return nullptr;
		}
		return G;
	}

	/** Scan every well-known library for a graph with this short name. */
	static UEdGraph* ResolveMacroByName(
		const FString& MacroName, TSharedPtr<FJsonObject>& OutErr)
	{
		TArray<UEdGraph*> Hits;
		TArray<FString>   Candidates;
		for (const TCHAR* LibPath : WellKnownMacroLibraries)
		{
			UBlueprint* Lib = LoadObject<UBlueprint>(nullptr, LibPath);
			if (Lib == nullptr) { continue; }
			if (UEdGraph* G = FindMacroGraphByName(Lib, MacroName))
			{
				Hits.Add(G);
				Candidates.Add(FString::Printf(TEXT("%s:%s"), LibPath, *MacroName));
			}
		}
		if (Hits.Num() == 0)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("No well-known macro library contains a macro named '%s'"),
					*MacroName));
			return nullptr;
		}
		if (Hits.Num() > 1)
		{
			TSharedRef<FJsonObject> ErrOut = UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("Macro name '%s' is ambiguous across libraries"), *MacroName));
			TArray<TSharedPtr<FJsonValue>> Arr;
			for (const FString& C : Candidates)
			{
				Arr.Add(MakeShared<FJsonValueString>(C));
			}
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetArrayField(TEXT("candidates"), Arr);
			ErrOut->SetObjectField(TEXT("detail"), Detail);
			OutErr = ErrOut;
			return nullptr;
		}
		return Hits[0];
	}

	/**
	 * Add a `K2Node_MacroInstance` wired to a macro graph (ForEachLoop etc).
	 * Accepts either a fully-qualified `macro_graph_path` or a short
	 * `macro_name` resolved against the well-known-libraries list.
	 */
	static TSharedRef<FJsonObject> HandleAddMacroInstance(
		UEdGraph* Graph, const TSharedPtr<FJsonObject>& Config,
		int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		if (!Config.IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("K2Node_MacroInstance requires config.macro_graph_path or config.macro_name"));
		}

		FString MacroPath, MacroName;
		const bool bHasPath = Config->TryGetStringField(TEXT("macro_graph_path"), MacroPath)
							  && !MacroPath.IsEmpty();
		const bool bHasName = Config->TryGetStringField(TEXT("macro_name"), MacroName)
							  && !MacroName.IsEmpty();

		if (!bHasPath && !bHasName)
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("config must contain `macro_graph_path` or `macro_name`"));
		}

		TSharedPtr<FJsonObject> Err;
		UEdGraph* MacroGraph = bHasPath
			? ResolveMacroByPath(MacroPath, Err)
			: ResolveMacroByName(MacroName, Err);
		if (MacroGraph == nullptr)
		{
			return Err.ToSharedRef();
		}

		UK2Node_MacroInstance* Node =
			SpawnNode<UK2Node_MacroInstance>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("K2Node_MacroInstance spawn returned nullptr"));
		}

		Node->SetMacroGraph(MacroGraph);
		Node->AllocateDefaultPins();
		Node->ReconstructNode();

		TSharedRef<FJsonObject> Data = BuildAddNodeResponse(Node, BpPath, GraphName);
		if (UBlueprint* OwningLib = Cast<UBlueprint>(MacroGraph->GetOuter()))
		{
			Data->SetStringField(TEXT("macro_graph_path"),
				FString::Printf(TEXT("%s:%s"),
					*OwningLib->GetPathName(), *MacroGraph->GetName()));
		}
		return Data;
	}

	/**
	 * Add a reroute ("knot") node. Pure cosmetic wire-routing device —
	 * agents use these to straighten long wires. No config.
	 */
	static TSharedRef<FJsonObject> HandleAddKnot(
		UEdGraph* Graph, int32 PosX, int32 PosY,
		const FString& BpPath, const FString& GraphName)
	{
		UK2Node_Knot* Node = SpawnNode<UK2Node_Knot>(Graph, PosX, PosY);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("K2Node_Knot spawn returned nullptr"));
		}
		return BuildAddNodeResponse(Node, BpPath, GraphName);
	}

	/** `blueprint.graph.add_node` body. */
	static TSharedRef<FJsonObject> HandleAddNode(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required"));
		}
		FString GraphName;
		if (!Args->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`graph_name` is required"));
		}
		FString NodeClass;
		if (!Args->TryGetStringField(TEXT("node_class"), NodeClass) || NodeClass.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`node_class` is required"));
		}

		TSharedPtr<FJsonObject> Err;
		UBlueprint* BP = ResolveBlueprint(BpPath, Err);
		if (BP == nullptr)
		{
			return Err.ToSharedRef();
		}

		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (Graph == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(
					TEXT("Blueprint '%s' has no graph named '%s'"),
					*BP->GetName(), *GraphName));
		}

		// Early refusal: entry/result nodes are auto-managed by the
		// editor's function-graph machinery. Creating one by hand is
		// the fastest way to corrupt a BP's compile. Point callers at
		// the v1 create-function-graph tool (not yet implemented;
		// documented in the hint so they can plan).
		if (NodeClass == TEXT("K2Node_FunctionEntry")
			|| NodeClass == TEXT("K2Node_FunctionResult"))
		{
			TSharedRef<FJsonObject> ErrOut = UeMcp::MakeInlineError(
				TEXT("INVALID_PAYLOAD"),
				FString::Printf(
					TEXT("'%s' is auto-generated by the editor and cannot be added manually"),
					*NodeClass));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("hint"),
				TEXT("function entry/result nodes are auto-generated; "
				     "use tests.create_function_graph instead (v1.1+)"));
			ErrOut->SetObjectField(TEXT("detail"), Detail);
			return ErrOut;
		}

		// Read optional position + config.
		int32 PosX = 0, PosY = 0;
		ReadPosition(Args, PosX, PosY);

		TSharedPtr<FJsonObject> Config;
		const TSharedPtr<FJsonObject>* ConfigPtr = nullptr;
		if (Args->TryGetObjectField(TEXT("config"), ConfigPtr) && ConfigPtr->IsValid())
		{
			Config = *ConfigPtr;
		}

		// Bracket the mutation. We do this up-front for every branch —
		// even the error branches — so partial state after a crashed
		// node ctor is transactionally rolled back by undo.
		BeginBlueprintMutation(BP, Graph);

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();

		if (NodeClass == TEXT("K2Node_CallFunction"))
		{
			Result = HandleAddCallFunction(Graph, Config, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_VariableGet"))
		{
			Result = HandleAddVariableNode<UK2Node_VariableGet>(
				Graph, BP, Config, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_VariableSet"))
		{
			Result = HandleAddVariableNode<UK2Node_VariableSet>(
				Graph, BP, Config, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_IfThenElse"))
		{
			Result = HandleAddIfThenElse(Graph, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_ExecutionSequence"))
		{
			Result = HandleAddExecutionSequence(Graph, Config, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_Event"))
		{
			Result = HandleAddEvent(Graph, BP, Config, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_CustomEvent"))
		{
			Result = HandleAddCustomEvent(Graph, BP, Config, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_ComponentBoundEvent"))
		{
			Result = HandleAddComponentBoundEvent(Graph, BP, Config, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_MacroInstance"))
		{
			Result = HandleAddMacroInstance(Graph, Config, PosX, PosY, BpPath, GraphName);
		}
		else if (NodeClass == TEXT("K2Node_Knot"))
		{
			Result = HandleAddKnot(Graph, PosX, PosY, BpPath, GraphName);
		}
		else
		{
			TSharedRef<FJsonObject> ErrOut = UeMcp::MakeInlineError(
				TEXT("INVALID_PAYLOAD"),
				FString::Printf(TEXT("Unsupported node_class '%s'"), *NodeClass));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetArrayField(TEXT("supported_classes"),
				BuildSupportedClassesArray());
			ErrOut->SetObjectField(TEXT("detail"), Detail);
			Result = ErrOut;
		}

		// Mark modified on success only. Error branches may have
		// spawned nothing; structural-modified on an unchanged BP is
		// harmless but noisy in the editor's refresh path.
		FString ErrorField;
		const bool bSucceeded = !Result->TryGetStringField(TEXT("error"), ErrorField);
		if (bSucceeded)
		{
			EndBlueprintMutation(BP, /*bStructural=*/true);
		}

		return Result;
	}

	// =================================================================
	//   blueprint.graph.connect_pins
	// =================================================================

	/**
	 * Resolve the four-tuple of (from_node, from_pin, to_node, to_pin)
	 * into concrete pin pointers, returning structured errors for each
	 * missing piece. Shared by `connect_pins` and `disconnect_pins`.
	 *
	 * WHY separate checks for each missing piece: callers debugging a
	 * flaky wire-up script want to know which end failed to resolve,
	 * not a single opaque "NOT_FOUND".
	 */
	static bool ResolvePinPair(
		UEdGraph* Graph,
		const FString& FromNodeId, const FString& FromPinName,
		const FString& ToNodeId,   const FString& ToPinName,
		UEdGraphPin*& OutFromPin,  UEdGraphPin*& OutToPin,
		TSharedPtr<FJsonObject>& OutErr)
	{
		OutFromPin = nullptr;
		OutToPin   = nullptr;

		UEdGraphNode* FromNode = FindNodeById(Graph, FromNodeId);
		if (FromNode == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("from_node '%s' not found in graph '%s'"),
					*FromNodeId, *Graph->GetName()));
			return false;
		}
		UEdGraphNode* ToNode = FindNodeById(Graph, ToNodeId);
		if (ToNode == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("to_node '%s' not found in graph '%s'"),
					*ToNodeId, *Graph->GetName()));
			return false;
		}
		UEdGraphPin* FromPin = FindPinByName(FromNode, FromPinName);
		if (FromPin == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("from_pin '%s' not found on node '%s'"),
					*FromPinName, *FromNodeId));
			return false;
		}
		UEdGraphPin* ToPin = FindPinByName(ToNode, ToPinName);
		if (ToPin == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("to_pin '%s' not found on node '%s'"),
					*ToPinName, *ToNodeId));
			return false;
		}

		OutFromPin = FromPin;
		OutToPin   = ToPin;
		return true;
	}

	/** `blueprint.graph.connect_pins` body. */
	static TSharedRef<FJsonObject> HandleConnectPins(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, GraphName, FromNodeId, FromPinName, ToNodeId, ToPinName;
		if (!Args->TryGetStringField(TEXT("bp_path"),    BpPath)      || BpPath.IsEmpty()
			|| !Args->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty()
			|| !Args->TryGetStringField(TEXT("from_node"), FromNodeId) || FromNodeId.IsEmpty()
			|| !Args->TryGetStringField(TEXT("from_pin"),  FromPinName) || FromPinName.IsEmpty()
			|| !Args->TryGetStringField(TEXT("to_node"),   ToNodeId)   || ToNodeId.IsEmpty()
			|| !Args->TryGetStringField(TEXT("to_pin"),    ToPinName)  || ToPinName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("bp_path, graph_name, from_node, from_pin, to_node, to_pin all required"));
		}

		TSharedPtr<FJsonObject> Err;
		UBlueprint* BP = ResolveBlueprint(BpPath, Err);
		if (BP == nullptr)
		{
			return Err.ToSharedRef();
		}

		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (Graph == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Blueprint '%s' has no graph named '%s'"),
					*BP->GetName(), *GraphName));
		}

		UEdGraphPin *FromPin = nullptr, *ToPin = nullptr;
		if (!ResolvePinPair(Graph, FromNodeId, FromPinName, ToNodeId, ToPinName,
			FromPin, ToPin, Err))
		{
			return Err.ToSharedRef();
		}

		// Schema-level type check. `CanCreateConnection` returns a
		// response with one of the CONNECT_RESPONSE_* codes and a
		// human-readable message — we use both.
		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (Schema == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("graph has no schema"));
		}
		const FPinConnectionResponse CanCreate =
			Schema->CanCreateConnection(FromPin, ToPin);

		if (CanCreate.Response == CONNECT_RESPONSE_DISALLOW)
		{
			TSharedRef<FJsonObject> ErrOut = UeMcp::MakeInlineError(
				TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("connection refused: %s"),
					*CanCreate.Message.ToString()));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("reason"), CanCreate.Message.ToString());
			ErrOut->SetObjectField(TEXT("detail"), Detail);
			return ErrOut;
		}

		BeginBlueprintMutation(BP, Graph);

		// TryCreateConnection handles BREAK_OTHERS_A/B/AB and the
		// MAKE_WITH_PROMOTION path. For MAKE_WITH_CONVERSION_NODE the
		// schema delegates to CreateAutomaticConversionNodeAndConnections;
		// we call that explicitly so we can surface the inserted
		// cast/conversion node id in the response.
		bool bAutocastInserted = false;
		FString AutocastNodeId;
		bool bConnected = false;

		if (CanCreate.Response == CONNECT_RESPONSE_MAKE_WITH_CONVERSION_NODE)
		{
			const int32 PrevNodeCount = Graph->Nodes.Num();
			bConnected = Schema->CreateAutomaticConversionNodeAndConnections(FromPin, ToPin);
			if (bConnected && Graph->Nodes.Num() > PrevNodeCount)
			{
				// The newly-inserted node is at the tail of the array.
				bAutocastInserted = true;
				if (UEdGraphNode* NewNode = Graph->Nodes.Last())
				{
					AutocastNodeId = NewNode->GetName();
				}
			}
		}
		else
		{
			bConnected = Schema->TryCreateConnection(FromPin, ToPin);
		}

		if (!bConnected)
		{
			// No structural change; don't dirty the package.
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("schema refused to make connection: %s"),
					*CanCreate.Message.ToString()));
		}

		// Engine drag-drop path invokes PinConnectionListChanged on both
		// endpoints after a successful connection so wildcard nodes
		// (e.g. K2Node_GetArrayItem) can refine their pin types from the
		// newly-linked counterpart. UEdGraphSchema::TryCreateConnection
		// does this for CONNECT_RESPONSE_MAKE* via the base TryCreateConnection,
		// but the MAKE_WITH_CONVERSION_NODE / promotion paths invoked via
		// CreateAutomaticConversionNodeAndConnections skip it — call it
		// unconditionally (idempotent on already-refined pins).
		// Order matches the engine: FromPin (==PinA) first, then ToPin.
		if (FromPin != nullptr && !FromPin->IsPendingKill() &&
			FromPin->GetOwningNodeUnchecked() != nullptr)
		{
			FromPin->GetOwningNode()->PinConnectionListChanged(FromPin);
		}
		if (ToPin != nullptr && !ToPin->IsPendingKill() &&
			ToPin->GetOwningNodeUnchecked() != nullptr)
		{
			ToPin->GetOwningNode()->PinConnectionListChanged(ToPin);
		}

		EndBlueprintMutation(BP, /*bStructural=*/true);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("connected"), true);

		TSharedRef<FJsonObject> FromObj = MakeShared<FJsonObject>();
		FromObj->SetStringField(TEXT("node"), FromNodeId);
		FromObj->SetStringField(TEXT("pin"),  FromPinName);
		Data->SetObjectField(TEXT("from"), FromObj);

		TSharedRef<FJsonObject> ToObj = MakeShared<FJsonObject>();
		ToObj->SetStringField(TEXT("node"), ToNodeId);
		ToObj->SetStringField(TEXT("pin"),  ToPinName);
		Data->SetObjectField(TEXT("to"),   ToObj);

		if (bAutocastInserted)
		{
			TSharedRef<FJsonObject> Autocast = MakeShared<FJsonObject>();
			Autocast->SetStringField(TEXT("node_id"), AutocastNodeId);
			Data->SetObjectField(TEXT("autocast_inserted"), Autocast);
		}

		Data->SetObjectField(TEXT("rollback"),
			BuildConnectRollback(BpPath, GraphName,
				FromNodeId, FromPinName, ToNodeId, ToPinName));
		return Data;
	}

	// =================================================================
	//   blueprint.graph.disconnect_pins
	// =================================================================

	/** `blueprint.graph.disconnect_pins` body. */
	static TSharedRef<FJsonObject> HandleDisconnectPins(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, GraphName, FromNodeId, FromPinName, ToNodeId, ToPinName;
		if (!Args->TryGetStringField(TEXT("bp_path"),    BpPath)      || BpPath.IsEmpty()
			|| !Args->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty()
			|| !Args->TryGetStringField(TEXT("from_node"), FromNodeId) || FromNodeId.IsEmpty()
			|| !Args->TryGetStringField(TEXT("from_pin"),  FromPinName) || FromPinName.IsEmpty()
			|| !Args->TryGetStringField(TEXT("to_node"),   ToNodeId)   || ToNodeId.IsEmpty()
			|| !Args->TryGetStringField(TEXT("to_pin"),    ToPinName)  || ToPinName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("bp_path, graph_name, from_node, from_pin, to_node, to_pin all required"));
		}

		TSharedPtr<FJsonObject> Err;
		UBlueprint* BP = ResolveBlueprint(BpPath, Err);
		if (BP == nullptr)
		{
			return Err.ToSharedRef();
		}

		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (Graph == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Blueprint '%s' has no graph named '%s'"),
					*BP->GetName(), *GraphName));
		}

		UEdGraphPin *FromPin = nullptr, *ToPin = nullptr;
		if (!ResolvePinPair(Graph, FromNodeId, FromPinName, ToNodeId, ToPinName,
			FromPin, ToPin, Err))
		{
			return Err.ToSharedRef();
		}

		// Idempotency: if the two pins aren't currently linked, return
		// a structured `already_disconnected` success rather than an
		// error. Callers retrying a flow without state tracking get a
		// stable no-op.
		const bool bCurrentlyLinked = FromPin->LinkedTo.Contains(ToPin);
		if (!bCurrentlyLinked)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("disconnected"), false);
			Data->SetBoolField(TEXT("already_disconnected"), true);
			return Data;
		}

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (Schema == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("graph has no schema"));
		}

		BeginBlueprintMutation(BP, Graph);
		Schema->BreakSinglePinLink(FromPin, ToPin);
		EndBlueprintMutation(BP, /*bStructural=*/true);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("disconnected"), true);
		Data->SetBoolField(TEXT("already_disconnected"), false);
		Data->SetObjectField(TEXT("rollback"),
			BuildDisconnectRollback(BpPath, GraphName,
				FromNodeId, FromPinName, ToNodeId, ToPinName));
		return Data;
	}

	// =================================================================
	//   blueprint.graph.set_pin_default
	// =================================================================

	/**
	 * Coerce a JSON value to the string form that K2's schema expects
	 * for a pin's literal default. The schema's own `TrySetDefaultValue`
	 * takes an FString; the variety of representations lives here.
	 *
	 * Returns true on successful coercion; false with `OutError` set
	 * when we can't make sense of the value for the target pin type.
	 */
	static bool CoerceValueToPinDefaultString(
		const UEdGraphPin* Pin, const TSharedPtr<FJsonValue>& Value,
		FString& OutString, FString& OutError)
	{
		if (Pin == nullptr)
		{
			OutError = TEXT("null pin");
			return false;
		}
		if (!Value.IsValid())
		{
			OutError = TEXT("null value");
			return false;
		}

		const FName& Category = Pin->PinType.PinCategory;

		// Bool — accept either JSON bool or a recognised string literal.
		if (Category == UEdGraphSchema_K2::PC_Boolean)
		{
			bool B = false;
			if (Value->TryGetBool(B))
			{
				OutString = B ? TEXT("true") : TEXT("false");
				return true;
			}
			FString S;
			if (Value->TryGetString(S))
			{
				const FString Lower = S.ToLower();
				if (Lower == TEXT("true") || Lower == TEXT("1"))
				{
					OutString = TEXT("true"); return true;
				}
				if (Lower == TEXT("false") || Lower == TEXT("0"))
				{
					OutString = TEXT("false"); return true;
				}
			}
			OutError = TEXT("expected bool");
			return false;
		}

		// Integer-family: accept numbers, also digit-only strings.
		if (Category == UEdGraphSchema_K2::PC_Int
			|| Category == UEdGraphSchema_K2::PC_Int64
			|| Category == UEdGraphSchema_K2::PC_Byte)
		{
			double D = 0.0;
			if (Value->TryGetNumber(D))
			{
				if (Category == UEdGraphSchema_K2::PC_Int64)
				{
					OutString = FString::Printf(TEXT("%lld"), static_cast<int64>(D));
				}
				else
				{
					OutString = FString::FromInt(static_cast<int32>(D));
				}
				return true;
			}
			FString S;
			if (Value->TryGetString(S))
			{
				OutString = S;
				return true;
			}
			OutError = TEXT("expected integer");
			return false;
		}

		// Float-family.
		if (Category == UEdGraphSchema_K2::PC_Float
			|| Category == UEdGraphSchema_K2::PC_Double
			|| Category == UEdGraphSchema_K2::PC_Real)
		{
			double D = 0.0;
			if (Value->TryGetNumber(D))
			{
				OutString = FString::SanitizeFloat(D);
				return true;
			}
			FString S;
			if (Value->TryGetString(S))
			{
				OutString = S;
				return true;
			}
			OutError = TEXT("expected number");
			return false;
		}

		// String / Name — same wire form; schema handles the
		// construction on its side.
		if (Category == UEdGraphSchema_K2::PC_String
			|| Category == UEdGraphSchema_K2::PC_Name
			|| Category == UEdGraphSchema_K2::PC_Text)
		{
			FString S;
			if (Value->TryGetString(S))
			{
				OutString = S;
				return true;
			}
			OutError = TEXT("expected string");
			return false;
		}

		// Struct — accept either a pre-stringified literal form the
		// editor uses natively (e.g. `"(X=1.0,Y=2.0,Z=3.0)"`) OR a 3/4
		// element array for vector/rotator/quaternion structs, OR 2
		// for Vector2D. Everything else requires the string form.
		if (Category == UEdGraphSchema_K2::PC_Struct)
		{
			FString S;
			if (Value->TryGetString(S))
			{
				OutString = S;
				return true;
			}
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (Value->TryGetArray(Arr) && Arr != nullptr)
			{
				UObject* SubObj = Pin->PinType.PinSubCategoryObject.Get();
				const FString StructName =
					SubObj ? SubObj->GetName() : FString();
				if (Arr->Num() == 3 && (StructName == TEXT("Vector")
					|| StructName == TEXT("Vector3f")))
				{
					double X = 0, Y = 0, Z = 0;
					(*Arr)[0]->TryGetNumber(X);
					(*Arr)[1]->TryGetNumber(Y);
					(*Arr)[2]->TryGetNumber(Z);
					OutString = FString::Printf(TEXT("(X=%s,Y=%s,Z=%s)"),
						*FString::SanitizeFloat(X),
						*FString::SanitizeFloat(Y),
						*FString::SanitizeFloat(Z));
					return true;
				}
				if (Arr->Num() == 3 && StructName == TEXT("Rotator"))
				{
					double P = 0, Y = 0, R = 0;
					(*Arr)[0]->TryGetNumber(P);
					(*Arr)[1]->TryGetNumber(Y);
					(*Arr)[2]->TryGetNumber(R);
					OutString = FString::Printf(TEXT("(P=%s,Y=%s,R=%s)"),
						*FString::SanitizeFloat(P),
						*FString::SanitizeFloat(Y),
						*FString::SanitizeFloat(R));
					return true;
				}
				if (Arr->Num() == 2 && StructName == TEXT("Vector2D"))
				{
					double X = 0, Y = 0;
					(*Arr)[0]->TryGetNumber(X);
					(*Arr)[1]->TryGetNumber(Y);
					OutString = FString::Printf(TEXT("(X=%s,Y=%s)"),
						*FString::SanitizeFloat(X),
						*FString::SanitizeFloat(Y));
					return true;
				}
				OutError = FString::Printf(
					TEXT("array-form default not supported for struct '%s'"),
					*StructName);
				return false;
			}
			OutError = TEXT("expected string or typed array for struct default");
			return false;
		}

		// Object / Class — accept a path string. The schema's own
		// TrySetDefaultValue handles UObject resolution.
		if (Category == UEdGraphSchema_K2::PC_Object
			|| Category == UEdGraphSchema_K2::PC_Class
			|| Category == UEdGraphSchema_K2::PC_Interface)
		{
			FString S;
			if (Value->TryGetString(S))
			{
				OutString = S;
				return true;
			}
			OutError = TEXT("expected object/class path string");
			return false;
		}

		// Enums — accept string (the enum entry name) or number.
		if (Category == UEdGraphSchema_K2::PC_Enum)
		{
			FString S;
			if (Value->TryGetString(S))
			{
				OutString = S;
				return true;
			}
			double D = 0.0;
			if (Value->TryGetNumber(D))
			{
				OutString = FString::FromInt(static_cast<int32>(D));
				return true;
			}
			OutError = TEXT("expected enum entry name or integer");
			return false;
		}

		// Fallback: pass through string form. Most exotic pin types
		// (soft-class, soft-object, wildcard) accept the same string
		// rep the editor shows.
		FString S;
		if (Value->TryGetString(S))
		{
			OutString = S;
			return true;
		}
		OutError = FString::Printf(
			TEXT("unsupported pin category '%s' for default value"),
			*Category.ToString());
		return false;
	}

	/** `blueprint.graph.set_pin_default` body. */
	static TSharedRef<FJsonObject> HandleSetPinDefault(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, GraphName, NodeId, PinName;
		if (!Args->TryGetStringField(TEXT("bp_path"),    BpPath)   || BpPath.IsEmpty()
			|| !Args->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty()
			|| !Args->TryGetStringField(TEXT("node_id"),    NodeId)    || NodeId.IsEmpty()
			|| !Args->TryGetStringField(TEXT("pin_name"),   PinName)   || PinName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("bp_path, graph_name, node_id, pin_name all required"));
		}

		TSharedPtr<FJsonValue> ValueField = Args->TryGetField(TEXT("value"));
		if (!ValueField.IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`value` is required"));
		}

		TSharedPtr<FJsonObject> Err;
		UBlueprint* BP = ResolveBlueprint(BpPath, Err);
		if (BP == nullptr)
		{
			return Err.ToSharedRef();
		}

		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (Graph == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Blueprint '%s' has no graph named '%s'"),
					*BP->GetName(), *GraphName));
		}

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("node '%s' not found in graph '%s'"),
					*NodeId, *GraphName));
		}
		UEdGraphPin* Pin = FindPinByName(Node, PinName);
		if (Pin == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("pin '%s' not found on node '%s'"),
					*PinName, *NodeId));
		}

		// Refuse to write a default on a linked pin. The editor ignores
		// the literal on a linked pin (the wire value wins at runtime),
		// so silently accepting the write would be a
		// looked-like-it-worked surprise.
		if (Pin->LinkedTo.Num() > 0)
		{
			TSharedRef<FJsonObject> ErrOut = UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("pin '%s' is linked; defaults are ignored"),
					*PinName));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("hint"), TEXT("pin is linked; disconnect first"));
			ErrOut->SetObjectField(TEXT("detail"), Detail);
			return ErrOut;
		}

		// Coerce the incoming JSON value to the pin's expected string
		// form. On failure, TYPE_MISMATCH with a focused message.
		FString CoercedValue, CoerceErr;
		if (!CoerceValueToPinDefaultString(Pin, ValueField, CoercedValue, CoerceErr))
		{
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("could not coerce value for pin '%s': %s"),
					*PinName, *CoerceErr));
		}

		// Capture previous default before the mutation so we can emit
		// a faithful rollback. DefaultValue is the string literal; we
		// don't attempt to round-trip object defaults here because
		// `DefaultObject` + `DefaultValue` both combining into a single
		// rollback semantic is best-effort and we prefer to omit the
		// hint over returning a wrong one.
		const FString PreviousValue = Pin->DefaultValue;

		const UEdGraphSchema* Schema = Graph->GetSchema();
		if (Schema == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("graph has no schema"));
		}

		BeginBlueprintMutation(BP, Graph);

		// Class- and object-ref pins store their literal in
		// `Pin->DefaultObject`, NOT in `Pin->DefaultValue`. The schema's
		// `TrySetDefaultValue` does set DefaultObject internally when it
		// can parse the string, but in practice it doesn't reliably
		// trigger the node's PinDefaultValueChanged callback — so a node
		// like `GetAllActorsOfClass` never propagates its new
		// ActorClass choice into the `OutActors` array's element type,
		// and downstream wildcard pins stay undetermined through
		// compile. Explicit branch: load the object, write
		// DefaultObject directly, then fire the callback.
		const FName PinCat = Pin->PinType.PinCategory;
		const bool bIsClassPin = (PinCat == UEdGraphSchema_K2::PC_Class
			|| PinCat == UEdGraphSchema_K2::PC_SoftClass);
		const bool bIsObjectPin = (PinCat == UEdGraphSchema_K2::PC_Object
			|| PinCat == UEdGraphSchema_K2::PC_Interface
			|| PinCat == UEdGraphSchema_K2::PC_SoftObject);

		if (bIsClassPin)
		{
			// Load as UClass specifically — TSubclassOf pins need a class.
			UClass* Cls = LoadObject<UClass>(nullptr, *CoercedValue);
			if (Cls == nullptr)
			{
				EndBlueprintMutation(BP, /*bStructural=*/false);
				return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("Could not load class '%s' for pin '%s'"),
						*CoercedValue, *PinName));
			}
			Pin->DefaultObject = Cls;
			Pin->DefaultValue = FString();
			Pin->AutogeneratedDefaultValue = FString();
			Node->PinDefaultValueChanged(Pin);
			// ReconstructNode when the node is a K2 node — many K2 nodes
			// (GetAllActorsOfClass, SpawnActorFromClass, ConstructObject,
			// etc.) derive their OUTPUT pin types from the class literal.
			// PinDefaultValueChanged alone doesn't force that rebuild;
			// ReconstructNode does. Downstream wildcards that were
			// already wired (e.g. Array_Get.TargetArray) stay connected
			// through the reconstruct as long as the class is valid.
			if (UK2Node* K2 = Cast<UK2Node>(Node))
			{
				K2->ReconstructNode();
			}
		}
		else if (bIsObjectPin)
		{
			UObject* Obj = LoadObject<UObject>(nullptr, *CoercedValue);
			if (Obj == nullptr)
			{
				EndBlueprintMutation(BP, /*bStructural=*/false);
				return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("Could not load object '%s' for pin '%s'"),
						*CoercedValue, *PinName));
			}
			Pin->DefaultObject = Obj;
			Pin->DefaultValue = FString();
			Pin->AutogeneratedDefaultValue = FString();
			Node->PinDefaultValueChanged(Pin);
			// Some object-ref pins also drive output types (e.g. literal
			// object feeding a Cast). Reconstruct for safety.
			if (UK2Node* K2 = Cast<UK2Node>(Node))
			{
				K2->ReconstructNode();
			}
		}
		else
		{
			// Scalar / struct / enum / name / text — the schema knows.
			Schema->TrySetDefaultValue(*Pin, CoercedValue, /*bMarkAsModified=*/true);
		}

		EndBlueprintMutation(BP, /*bStructural=*/false);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("set"), true);
		Data->SetStringField(TEXT("path"),
			FString::Printf(TEXT("%s.%s.%s"), *GraphName, *NodeId, *PinName));
		Data->SetStringField(TEXT("previous_value"), PreviousValue);

		// Rollback: invoke ourselves with the captured previous value.
		// Always faithful for scalar defaults; omitted-value rollback
		// would silently drop the semantic difference between "empty
		// literal" and "no change", so we always emit.
		TSharedRef<FJsonObject> RollbackArgs = MakeShared<FJsonObject>();
		RollbackArgs->SetStringField(TEXT("bp_path"),    BpPath);
		RollbackArgs->SetStringField(TEXT("graph_name"), GraphName);
		RollbackArgs->SetStringField(TEXT("node_id"),    NodeId);
		RollbackArgs->SetStringField(TEXT("pin_name"),   PinName);
		RollbackArgs->SetStringField(TEXT("value"),      PreviousValue);

		TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
		Rollback->SetStringField(TEXT("tool"), TEXT("blueprint.graph.set_pin_default"));
		Rollback->SetObjectField(TEXT("args"), RollbackArgs);
		Data->SetObjectField(TEXT("rollback"), Rollback);

		return Data;
	}

	// =================================================================
	//   blueprint.graph.remove_node
	// =================================================================

	/** `blueprint.graph.remove_node` body. */
	static TSharedRef<FJsonObject> HandleRemoveNode(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, GraphName, NodeId;
		if (!Args->TryGetStringField(TEXT("bp_path"),    BpPath)    || BpPath.IsEmpty()
			|| !Args->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty()
			|| !Args->TryGetStringField(TEXT("node_id"),    NodeId)    || NodeId.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("bp_path, graph_name, node_id all required"));
		}

		TSharedPtr<FJsonObject> Err;
		UBlueprint* BP = ResolveBlueprint(BpPath, Err);
		if (BP == nullptr)
		{
			return Err.ToSharedRef();
		}

		UEdGraph* Graph = FindGraphByName(BP, GraphName);
		if (Graph == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Blueprint '%s' has no graph named '%s'"),
					*BP->GetName(), *GraphName));
		}

		UEdGraphNode* Node = FindNodeById(Graph, NodeId);
		if (Node == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("node '%s' not found in graph '%s'"),
					*NodeId, *GraphName));
		}

		// Refuse to remove entry/result nodes. The editor's function-
		// graph infrastructure owns these; removing one orphans the
		// function signature and breaks every caller.
		UClass* NodeCls = Node->GetClass();
		const FString NodeClassName = NodeCls ? NodeCls->GetName() : FString();
		if (NodeClassName == TEXT("K2Node_FunctionEntry")
			|| NodeClassName == TEXT("K2Node_FunctionResult"))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("cannot remove '%s' — entry/result nodes are auto-managed"),
					*NodeClassName));
		}

		BeginBlueprintMutation(BP, Graph);

		// Break every link THEN let FBlueprintEditorUtils::RemoveNode
		// do the actual removal. BreakAllNodeLinks fires the
		// PinConnectionListChanged callbacks so connected nodes can
		// refresh (e.g. orphan their pins in wildcard cases).
		Node->BreakAllNodeLinks();
		FBlueprintEditorUtils::RemoveNode(BP, Node, /*bDontRecompile=*/true);

		EndBlueprintMutation(BP, /*bStructural=*/true);

		// No `rollback` field: truly faithfully reversing a node
		// deletion would require capturing every pin default, object
		// default, and connection, then a recreate-plus-rewire sequence.
		// Documented as non-reversible in the tool description.
		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("removed"), true);
		Data->SetStringField(TEXT("node_id"),    NodeId);
		Data->SetStringField(TEXT("graph_name"), GraphName);
		return Data;
	}

	// =================================================================
	//   blueprint.graph.compile
	// =================================================================

	/**
	 * Translate an FTokenizedMessage severity to the wire bucket
	 * (`errors` vs `warnings`). Info-level messages are silently
	 * dropped — we don't want to drown agents in Kismet's chattier
	 * informational output.
	 */
	static bool IsErrorSeverity(EMessageSeverity::Type Severity)
	{
		return Severity == EMessageSeverity::Error;
	}

	static bool IsWarningSeverity(EMessageSeverity::Type Severity)
	{
		return Severity == EMessageSeverity::Warning
			|| Severity == EMessageSeverity::PerformanceWarning;
	}

	/**
	 * Flatten a compiler results log into `errors: []` and `warnings: []`
	 * string arrays. We use the message's own `ToText()` since it
	 * collapses the tokenized message into a single line matching what
	 * the Kismet compiler tab shows.
	 */
	static void SplitCompileMessages(
		const FCompilerResultsLog& Log,
		TArray<TSharedPtr<FJsonValue>>& OutErrors,
		TArray<TSharedPtr<FJsonValue>>& OutWarnings)
	{
		for (const TSharedRef<FTokenizedMessage>& Msg : Log.Messages)
		{
			const FString Line = Msg->ToText().ToString();
			const EMessageSeverity::Type Sev = Msg->GetSeverity();
			if (IsErrorSeverity(Sev))
			{
				OutErrors.Add(MakeShared<FJsonValueString>(Line));
			}
			else if (IsWarningSeverity(Sev))
			{
				OutWarnings.Add(MakeShared<FJsonValueString>(Line));
			}
		}
	}

	/** `blueprint.graph.compile` body. */
	static TSharedRef<FJsonObject> HandleCompile(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required"));
		}
		bool bSaveOnCompile = false;
		Args->TryGetBoolField(TEXT("save_on_compile"), bSaveOnCompile);

		if (GEditor == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; cannot compile"));
		}

		TSharedPtr<FJsonObject> Err;
		UBlueprint* BP = ResolveBlueprint(BpPath, Err);
		if (BP == nullptr)
		{
			return Err.ToSharedRef();
		}

		// Capture messages via an explicit results log — we own it,
		// which means we don't have to rely on the BP's transient
		// MessageLog (whose contents can be mutated by the editor's
		// Kismet tab mid-read).
		FCompilerResultsLog Results;
		Results.SetSourcePath(BP->GetPathName());
		Results.BeginEvent(TEXT("UeMcp.BlueprintCompile"));
		FKismetEditorUtilities::CompileBlueprint(
			BP, EBlueprintCompileOptions::None, &Results);
		Results.EndEvent();

		TArray<TSharedPtr<FJsonValue>> Errors, Warnings;
		SplitCompileMessages(Results, Errors, Warnings);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		const bool bCompiled = (Results.NumErrors == 0);
		Data->SetBoolField(TEXT("compiled"), bCompiled);
		Data->SetArrayField(TEXT("errors"),   Errors);
		Data->SetArrayField(TEXT("warnings"), Warnings);

		if (!bCompiled)
		{
			// Failure path: never save. Pre-compile state is undefined
			// per the tool contract; no rollback either.
			return Data;
		}

		if (bSaveOnCompile)
		{
			// `UEditorAssetSubsystem` lives in the `UnrealEd` module (we
			// already link against it) — avoids pulling in the
			// `EditorScriptingUtilities` plugin just to reach
			// `UEditorAssetLibrary`, which is a thin wrapper over this
			// subsystem anyway. `bOnlyIfIsDirty=false` forces the save
			// even if the compile didn't mark the package dirty (rare
			// but possible when a prior edit already marked it).
			bool bSaved = false;
			FString SaveErrorMsg;
			if (GEditor == nullptr)
			{
				SaveErrorMsg = TEXT("GEditor went away between compile and save");
			}
			else if (UEditorAssetSubsystem* AssetSubsystem =
				GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
			{
				bSaved = AssetSubsystem->SaveLoadedAsset(BP, /*bOnlyIfIsDirty=*/false);
				if (!bSaved)
				{
					SaveErrorMsg = TEXT("UEditorAssetSubsystem::SaveLoadedAsset returned false; "
						"see editor log for details");
				}
			}
			else
			{
				SaveErrorMsg = TEXT("UEditorAssetSubsystem not available");
			}

			if (bSaved)
			{
				Data->SetBoolField(TEXT("saved"), true);
			}
			else
			{
				// Compile DID land; save failed. Caller retries save
				// explicitly via a future asset.save tool.
				Data->SetBoolField(TEXT("saved"), false);
				Data->SetStringField(TEXT("save_error"), SaveErrorMsg);
			}
		}

		return Data;
	}

	// ------------------------------------------------------------------
	// blueprint.graph.add_node_with_defaults — COMPOSITE handler.
	//
	// Informed by the tool-call ledger: add_node is almost always followed
	// by a set_pin_default and/or a connect_pins in the next 1-3 calls.
	// This tool collapses that sequence into one round-trip with
	// transactional semantics: add + N defaults + optional two exec wires.
	// On first failure we roll back by removing the added node (which
	// also severs any partial connections).
	// ------------------------------------------------------------------

	/**
	 * Read a string field on an inline error (error,message) payload, or
	 * empty when not present.
	 */
	static FString InlineErrorString(
		const TSharedRef<FJsonObject>& Obj, const TCHAR* Field)
	{
		FString Out;
		Obj->TryGetStringField(Field, Out);
		return Out;
	}

	/** True if the response from a delegated handler is an inline error. */
	static bool IsInlineError(const TSharedRef<FJsonObject>& Obj)
	{
		FString Code;
		return Obj->TryGetStringField(TEXT("error"), Code) && !Code.IsEmpty();
	}

	/** `blueprint.graph.add_node_with_defaults` handler body. */
	static TSharedRef<FJsonObject> HandleAddNodeWithDefaults(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		// --- Required string args --------------------------------------
		FString BpPath, GraphName, NodeClass;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`graph_name` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("node_class"), NodeClass) || NodeClass.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`node_class` is required and must be a non-empty string"));
		}

		// --- Optional pin_defaults + connect_from + connect_to ---------
		const TSharedPtr<FJsonObject>* PinDefaultsObj = nullptr;
		Args->TryGetObjectField(TEXT("pin_defaults"), PinDefaultsObj);

		const TSharedPtr<FJsonObject>* ConnectFromObj = nullptr;
		Args->TryGetObjectField(TEXT("connect_from"), ConnectFromObj);

		const TSharedPtr<FJsonObject>* ConnectToObj = nullptr;
		Args->TryGetObjectField(TEXT("connect_to"), ConnectToObj);

		// --- Step 1: add the node. Forward everything the primitive
		// already understands (bp_path, graph_name, node_class, config,
		// position). Response carries node_id on success.
		TSharedRef<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("bp_path"), BpPath);
		AddArgs->SetStringField(TEXT("graph_name"), GraphName);
		AddArgs->SetStringField(TEXT("node_class"), NodeClass);
		{
			const TSharedPtr<FJsonObject>* ConfigObj = nullptr;
			if (Args->TryGetObjectField(TEXT("config"), ConfigObj) && ConfigObj)
			{
				AddArgs->SetObjectField(TEXT("config"), *ConfigObj);
			}
			const TSharedPtr<FJsonObject>* PositionObj = nullptr;
			if (Args->TryGetObjectField(TEXT("position"), PositionObj) && PositionObj)
			{
				AddArgs->SetObjectField(TEXT("position"), *PositionObj);
			}
		}
		TSharedRef<FJsonObject> AddResp = HandleAddNode(AddArgs, Cancel);
		if (IsInlineError(AddResp))
		{
			// Nothing to roll back — the add itself failed.
			return AddResp;
		}
		FString NodeId;
		AddResp->TryGetStringField(TEXT("node_id"), NodeId);
		if (NodeId.IsEmpty())
		{
			// Defensive — add_node always returns node_id on success, but
			// if the contract ever drifts we surface a clear error rather
			// than silently continuing with an empty id.
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("add_node succeeded but returned no node_id"));
		}

		// Helper lambda: roll back the partially-built composite by
		// destroying the node we just added. Errors here are logged-only
		// — we're already in an error-return path, don't shadow the
		// original failure.
		auto Rollback = [&](const TCHAR* Reason)
		{
			TSharedRef<FJsonObject> RemoveArgs = MakeShared<FJsonObject>();
			RemoveArgs->SetStringField(TEXT("bp_path"), BpPath);
			RemoveArgs->SetStringField(TEXT("graph_name"), GraphName);
			RemoveArgs->SetStringField(TEXT("node_id"), NodeId);
			TSharedRef<FJsonObject> RemResp = HandleRemoveNode(RemoveArgs, Cancel);
			if (IsInlineError(RemResp))
			{
				UE_LOG(LogTemp, Warning,
					TEXT("add_node_with_defaults rollback failed after %s: %s"),
					Reason, *InlineErrorString(RemResp, TEXT("message")));
			}
		};

		// --- Step 2: apply each pin default ----------------------------
		TArray<TSharedPtr<FJsonValue>> AppliedDefaults;
		if (PinDefaultsObj && *PinDefaultsObj)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : (*PinDefaultsObj)->Values)
			{
				const FString& PinName = Entry.Key;
				const TSharedPtr<FJsonValue>& Value = Entry.Value;
				TSharedRef<FJsonObject> SetArgs = MakeShared<FJsonObject>();
				SetArgs->SetStringField(TEXT("bp_path"), BpPath);
				SetArgs->SetStringField(TEXT("graph_name"), GraphName);
				SetArgs->SetStringField(TEXT("node_id"), NodeId);
				SetArgs->SetStringField(TEXT("pin_name"), PinName);
				SetArgs->SetField(TEXT("value"), Value);

				TSharedRef<FJsonObject> SetResp = HandleSetPinDefault(SetArgs, Cancel);
				if (IsInlineError(SetResp))
				{
					Rollback(TEXT("set_pin_default failed"));
					// Enrich the error with which pin broke the composite.
					SetResp->SetStringField(TEXT("failed_step"), TEXT("set_pin_default"));
					SetResp->SetStringField(TEXT("failed_pin"), PinName);
					return SetResp;
				}
				AppliedDefaults.Add(MakeShared<FJsonValueString>(PinName));
			}
		}

		// --- Step 3: optional connect_from -----------------------------
		// Shape: {node: "<node_id>", pin?: "then" (source output pin on
		// the existing node), target_pin?: "execute" (input pin on our
		// new node)}. Defaults fit the common "exec wire in" case.
		TSharedPtr<FJsonObject> ConnectedFromOut;
		if (ConnectFromObj && *ConnectFromObj)
		{
			FString FromNode;
			(*ConnectFromObj)->TryGetStringField(TEXT("node"), FromNode);
			if (FromNode.IsEmpty())
			{
				Rollback(TEXT("connect_from missing 'node'"));
				return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
					TEXT("`connect_from.node` is required when connect_from is provided"));
			}
			FString FromPin = TEXT("then");
			(*ConnectFromObj)->TryGetStringField(TEXT("pin"), FromPin);
			FString TargetPin = TEXT("execute");
			(*ConnectFromObj)->TryGetStringField(TEXT("target_pin"), TargetPin);

			TSharedRef<FJsonObject> CArgs = MakeShared<FJsonObject>();
			CArgs->SetStringField(TEXT("bp_path"), BpPath);
			CArgs->SetStringField(TEXT("graph_name"), GraphName);
			CArgs->SetStringField(TEXT("from_node"), FromNode);
			CArgs->SetStringField(TEXT("from_pin"), FromPin);
			CArgs->SetStringField(TEXT("to_node"), NodeId);
			CArgs->SetStringField(TEXT("to_pin"), TargetPin);
			TSharedRef<FJsonObject> CResp = HandleConnectPins(CArgs, Cancel);
			if (IsInlineError(CResp))
			{
				Rollback(TEXT("connect_from failed"));
				CResp->SetStringField(TEXT("failed_step"), TEXT("connect_from"));
				return CResp;
			}
			ConnectedFromOut = MakeShared<FJsonObject>();
			ConnectedFromOut->SetStringField(TEXT("from_node"), FromNode);
			ConnectedFromOut->SetStringField(TEXT("from_pin"), FromPin);
			ConnectedFromOut->SetStringField(TEXT("to_pin"), TargetPin);
			const TSharedPtr<FJsonObject>* AutoCast = nullptr;
			if (CResp->TryGetObjectField(TEXT("autocast_inserted"), AutoCast) && AutoCast)
			{
				ConnectedFromOut->SetObjectField(TEXT("autocast_inserted"), *AutoCast);
			}
		}

		// --- Step 4: optional connect_to -------------------------------
		// Shape: {from_pin?: "then" (source output pin on our new node),
		// node: "<node_id>", pin?: "execute" (input pin on existing node)}.
		TSharedPtr<FJsonObject> ConnectedToOut;
		if (ConnectToObj && *ConnectToObj)
		{
			FString ToNode;
			(*ConnectToObj)->TryGetStringField(TEXT("node"), ToNode);
			if (ToNode.IsEmpty())
			{
				Rollback(TEXT("connect_to missing 'node'"));
				return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
					TEXT("`connect_to.node` is required when connect_to is provided"));
			}
			FString FromPin = TEXT("then");
			(*ConnectToObj)->TryGetStringField(TEXT("from_pin"), FromPin);
			FString ToPin = TEXT("execute");
			(*ConnectToObj)->TryGetStringField(TEXT("pin"), ToPin);

			TSharedRef<FJsonObject> CArgs = MakeShared<FJsonObject>();
			CArgs->SetStringField(TEXT("bp_path"), BpPath);
			CArgs->SetStringField(TEXT("graph_name"), GraphName);
			CArgs->SetStringField(TEXT("from_node"), NodeId);
			CArgs->SetStringField(TEXT("from_pin"), FromPin);
			CArgs->SetStringField(TEXT("to_node"), ToNode);
			CArgs->SetStringField(TEXT("to_pin"), ToPin);
			TSharedRef<FJsonObject> CResp = HandleConnectPins(CArgs, Cancel);
			if (IsInlineError(CResp))
			{
				Rollback(TEXT("connect_to failed"));
				CResp->SetStringField(TEXT("failed_step"), TEXT("connect_to"));
				return CResp;
			}
			ConnectedToOut = MakeShared<FJsonObject>();
			ConnectedToOut->SetStringField(TEXT("from_pin"), FromPin);
			ConnectedToOut->SetStringField(TEXT("to_node"), ToNode);
			ConnectedToOut->SetStringField(TEXT("to_pin"), ToPin);
			const TSharedPtr<FJsonObject>* AutoCast = nullptr;
			if (CResp->TryGetObjectField(TEXT("autocast_inserted"), AutoCast) && AutoCast)
			{
				ConnectedToOut->SetObjectField(TEXT("autocast_inserted"), *AutoCast);
			}
		}

		// --- Success: build composite response -------------------------
		// Start from the add_node response so the caller gets the same
		// shape (node_id, class, title, pins, graph_name, rollback). Then
		// layer our composite-specific fields on top.
		TSharedRef<FJsonObject> Out = AddResp;
		Out->SetArrayField(TEXT("applied_defaults"), AppliedDefaults);
		if (ConnectedFromOut.IsValid())
		{
			Out->SetObjectField(TEXT("connected_from"), ConnectedFromOut);
		}
		if (ConnectedToOut.IsValid())
		{
			Out->SetObjectField(TEXT("connected_to"), ConnectedToOut);
		}
		return Out;
	}

	// ------------------------------------------------------------------
	// blueprint.add_variable — add a member variable to a BP.
	//
	// Closes one of the three big gaps flagged by the BP_Turret battle
	// test: authoring state variables (Health, FireRate, CurrentTarget,
	// etc.) on a BP currently requires `python_exec
	// BlueprintEditorLibrary.add_member_variable`. This tool makes it
	// native.
	//
	// Accepts a short type string matching UE's Blueprint pin-type DSL:
	//   "bool" | "byte" | "int" | "int64" | "float" | "double" |
	//   "string" | "name" | "text" |
	//   "vector" | "rotator" | "transform" | "color" | "linear_color" |
	//   "object:/Script/Engine.Actor" (object reference, subcategory is
	//                                  the class path) |
	//   "struct:/Script/Engine.BlueprintCallable" (struct, subcategory
	//                                  is the struct path).
	//
	// Returns `{added: true, name, type_category, rollback}` on success.
	// Rollback names `blueprint.remove_variable` — which doesn't exist
	// yet; the field is emitted for forward-compat with a future
	// flow-runner rather than for immediate undo.
	// ------------------------------------------------------------------

	/** Parse the type string into an FEdGraphPinType.
	 *
	 *  Grammar (recursive — containers wrap a nested terminal type):
	 *    bool | byte | int | int64 | float | double |
	 *    string | name | text |
	 *    vector | rotator | transform | color | linear_color |
	 *    object:<class_path>       -> UObject ref (UClass* sub)
	 *    class:<class_path>        -> TSubclassOf<T>        (PC_Class)
	 *    soft_object:<class_path>  -> TSoftObjectPtr<T>     (PC_SoftObject)
	 *    soft_class:<class_path>   -> TSoftClassPtr<T>      (PC_SoftClass)
	 *    struct:<struct_path>      -> UScriptStruct ref
	 *    enum:<enum_path>          -> PC_Byte + UEnum* sub
	 *    array:<inner>             -> Container=Array, inner recurses
	 *    set:<inner>               -> Container=Set, inner recurses
	 *    map:<key>,<value>         -> Container=Map, first ',' splits K/V
	 */
	static bool ParseVariableType(
		const FString& TypeStr, FEdGraphPinType& OutPinType, FString& OutError)
	{
		using UEdGraphSchemaK2 = UEdGraphSchema_K2;
		OutPinType = FEdGraphPinType();
		OutPinType.ContainerType = EPinContainerType::None;

		// ----- container prefixes: array:/set:/map: recurse on inner -----
		auto SetContainerFromInner = [&](
			const FEdGraphPinType& Inner, EPinContainerType Container)
		{
			OutPinType = Inner;
			OutPinType.ContainerType = Container;
		};

		if (TypeStr.StartsWith(TEXT("array:"), ESearchCase::IgnoreCase))
		{
			const FString InnerStr = TypeStr.Mid(6);
			if (InnerStr.IsEmpty())
			{
				OutError = TEXT("`type` 'array:' requires an inner type (e.g. 'array:int', 'array:object:/Script/Engine.Actor')");
				return false;
			}
			FEdGraphPinType Inner;
			if (!ParseVariableType(InnerStr, Inner, OutError)) { return false; }
			if (Inner.ContainerType != EPinContainerType::None)
			{
				OutError = FString::Printf(
					TEXT("`type` nested containers are not supported (got 'array:%s')"),
					*InnerStr);
				return false;
			}
			SetContainerFromInner(Inner, EPinContainerType::Array);
			return true;
		}
		if (TypeStr.StartsWith(TEXT("set:"), ESearchCase::IgnoreCase))
		{
			const FString InnerStr = TypeStr.Mid(4);
			if (InnerStr.IsEmpty())
			{
				OutError = TEXT("`type` 'set:' requires an inner type (e.g. 'set:name')");
				return false;
			}
			FEdGraphPinType Inner;
			if (!ParseVariableType(InnerStr, Inner, OutError)) { return false; }
			if (Inner.ContainerType != EPinContainerType::None)
			{
				OutError = FString::Printf(
					TEXT("`type` nested containers are not supported (got 'set:%s')"),
					*InnerStr);
				return false;
			}
			SetContainerFromInner(Inner, EPinContainerType::Set);
			return true;
		}
		if (TypeStr.StartsWith(TEXT("map:"), ESearchCase::IgnoreCase))
		{
			const FString Rest = TypeStr.Mid(4);
			// Split on FIRST comma — the value half may contain ':' but
			// not another ',' in our grammar (no nested maps).
			int32 CommaIdx = INDEX_NONE;
			if (!Rest.FindChar(TEXT(','), CommaIdx))
			{
				OutError = TEXT("`type` 'map:' requires '<key_type>,<value_type>' (e.g. 'map:name,int')");
				return false;
			}
			const FString KeyStr = Rest.Left(CommaIdx);
			const FString ValueStr = Rest.Mid(CommaIdx + 1);
			if (KeyStr.IsEmpty() || ValueStr.IsEmpty())
			{
				OutError = TEXT("`type` 'map:' both key and value types are required");
				return false;
			}
			FEdGraphPinType Key, Value;
			if (!ParseVariableType(KeyStr, Key, OutError))     { return false; }
			if (!ParseVariableType(ValueStr, Value, OutError)) { return false; }
			if (Key.ContainerType != EPinContainerType::None ||
				Value.ContainerType != EPinContainerType::None)
			{
				OutError = TEXT("`type` 'map:' key and value must be terminal (non-container) types");
				return false;
			}
			OutPinType = Key;
			OutPinType.ContainerType = EPinContainerType::Map;
			OutPinType.PinValueType.TerminalCategory          = Value.PinCategory;
			OutPinType.PinValueType.TerminalSubCategory       = Value.PinSubCategory;
			OutPinType.PinValueType.TerminalSubCategoryObject = Value.PinSubCategoryObject;
			return true;
		}

		// ----- terminal types: split on ':' for category:subcategory -----
		FString Category = TypeStr;
		FString SubPath;
		TypeStr.Split(TEXT(":"), &Category, &SubPath);

		const FString Lower = Category.ToLower();
		if (Lower == TEXT("bool"))         { OutPinType.PinCategory = UEdGraphSchemaK2::PC_Boolean; return true; }
		if (Lower == TEXT("byte"))         { OutPinType.PinCategory = UEdGraphSchemaK2::PC_Byte;    return true; }
		if (Lower == TEXT("int"))          { OutPinType.PinCategory = UEdGraphSchemaK2::PC_Int;     return true; }
		if (Lower == TEXT("int64"))        { OutPinType.PinCategory = UEdGraphSchemaK2::PC_Int64;   return true; }
		if (Lower == TEXT("float"))
		{
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_Real;
			OutPinType.PinSubCategory = UEdGraphSchemaK2::PC_Float;
			return true;
		}
		if (Lower == TEXT("double"))
		{
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_Real;
			OutPinType.PinSubCategory = UEdGraphSchemaK2::PC_Double;
			return true;
		}
		if (Lower == TEXT("string"))       { OutPinType.PinCategory = UEdGraphSchemaK2::PC_String;  return true; }
		if (Lower == TEXT("name"))         { OutPinType.PinCategory = UEdGraphSchemaK2::PC_Name;    return true; }
		if (Lower == TEXT("text"))         { OutPinType.PinCategory = UEdGraphSchemaK2::PC_Text;    return true; }

		auto MakeStruct = [&](UScriptStruct* S)
		{
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_Struct;
			OutPinType.PinSubCategoryObject = S;
		};
		if (Lower == TEXT("vector"))       { MakeStruct(TBaseStructure<FVector>::Get());       return true; }
		if (Lower == TEXT("rotator"))      { MakeStruct(TBaseStructure<FRotator>::Get());      return true; }
		if (Lower == TEXT("transform"))    { MakeStruct(TBaseStructure<FTransform>::Get());    return true; }
		if (Lower == TEXT("color"))        { MakeStruct(TBaseStructure<FColor>::Get());        return true; }
		if (Lower == TEXT("linear_color")) { MakeStruct(TBaseStructure<FLinearColor>::Get());  return true; }

		auto LoadClassOrErr = [&](const FString& Label) -> UClass*
		{
			if (SubPath.IsEmpty())
			{
				OutError = FString::Printf(
					TEXT("`type` '%s' requires a class path after ':' (e.g. '%s:/Script/Engine.Actor')"),
					*Label, *Label);
				return nullptr;
			}
			UClass* Cls = LoadObject<UClass>(nullptr, *SubPath);
			if (Cls == nullptr)
			{
				OutError = FString::Printf(TEXT("Could not load class '%s'"), *SubPath);
			}
			return Cls;
		};

		if (Lower == TEXT("object"))
		{
			UClass* Cls = LoadClassOrErr(TEXT("object"));
			if (!Cls) { return false; }
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_Object;
			OutPinType.PinSubCategoryObject = Cls;
			return true;
		}
		if (Lower == TEXT("class"))
		{
			UClass* Cls = LoadClassOrErr(TEXT("class"));
			if (!Cls) { return false; }
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_Class;
			OutPinType.PinSubCategoryObject = Cls;
			return true;
		}
		if (Lower == TEXT("soft_object"))
		{
			UClass* Cls = LoadClassOrErr(TEXT("soft_object"));
			if (!Cls) { return false; }
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_SoftObject;
			OutPinType.PinSubCategoryObject = Cls;
			return true;
		}
		if (Lower == TEXT("soft_class"))
		{
			UClass* Cls = LoadClassOrErr(TEXT("soft_class"));
			if (!Cls) { return false; }
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_SoftClass;
			OutPinType.PinSubCategoryObject = Cls;
			return true;
		}
		if (Lower == TEXT("struct"))
		{
			if (SubPath.IsEmpty())
			{
				OutError = TEXT("`type` 'struct' requires a struct path after ':' (e.g. 'struct:/Script/CoreUObject.Vector')");
				return false;
			}
			UScriptStruct* S = LoadObject<UScriptStruct>(nullptr, *SubPath);
			if (S == nullptr)
			{
				OutError = FString::Printf(TEXT("Could not load struct '%s'"), *SubPath);
				return false;
			}
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_Struct;
			OutPinType.PinSubCategoryObject = S;
			return true;
		}
		if (Lower == TEXT("enum"))
		{
			if (SubPath.IsEmpty())
			{
				OutError = TEXT("`type` 'enum' requires an enum path after ':' (e.g. 'enum:/Script/Engine.ECollisionChannel')");
				return false;
			}
			UEnum* E = LoadObject<UEnum>(nullptr, *SubPath);
			if (E == nullptr)
			{
				OutError = FString::Printf(TEXT("Could not load enum '%s'"), *SubPath);
				return false;
			}
			OutPinType.PinCategory = UEdGraphSchemaK2::PC_Byte;
			OutPinType.PinSubCategoryObject = E;
			return true;
		}

		OutError = FString::Printf(
			TEXT("Unknown variable type '%s'. Supported: bool, byte, int, int64, float, double, ")
			TEXT("string, name, text, vector, rotator, transform, color, linear_color, ")
			TEXT("object:<class_path>, class:<class_path>, soft_object:<class_path>, ")
			TEXT("soft_class:<class_path>, struct:<struct_path>, enum:<enum_path>, ")
			TEXT("array:<inner>, set:<inner>, map:<key>,<value>."),
			*TypeStr);
		return false;
	}

	/** `blueprint.add_variable` handler body. */
	static TSharedRef<FJsonObject> HandleBlueprintAddVariable(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, VariableName, TypeStr;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("variable_name"), VariableName) ||
			VariableName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`variable_name` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`type` is required and must be a non-empty string"));
		}

		TSharedPtr<FJsonObject> BpErr;
		UBlueprint* BP = ResolveBlueprint(BpPath, BpErr);
		if (BP == nullptr)
		{
			return BpErr.ToSharedRef();
		}

		// Reject duplicate name up front — FBlueprintEditorUtils returns
		// false silently on conflict, which we want to surface.
		const FName VarFName(*VariableName);
		for (const FBPVariableDescription& Existing : BP->NewVariables)
		{
			if (Existing.VarName == VarFName)
			{
				return UeMcp::MakeInlineError(TEXT("PROP_NOT_FOUND"),
					FString::Printf(
						TEXT("Variable '%s' already exists on '%s'; use blueprint.remove_variable first or pick a different name"),
						*VariableName, *BpPath));
			}
		}

		FEdGraphPinType PinType;
		FString TypeErr;
		if (!ParseVariableType(TypeStr, PinType, TypeErr))
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"), TypeErr);
		}

		const bool bAdded = FBlueprintEditorUtils::AddMemberVariable(
			BP, VarFName, PinType);
		if (!bAdded)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(
					TEXT("FBlueprintEditorUtils::AddMemberVariable returned false for '%s' on '%s'"),
					*VariableName, *BpPath));
		}

		// Optional default_value — set it via FBlueprintEditorUtils too.
		const TSharedPtr<FJsonValue>* DefaultValue = nullptr;
		if (Args->Values.Contains(TEXT("default_value")))
		{
			DefaultValue = &Args->Values[TEXT("default_value")];
		}
		if (DefaultValue && DefaultValue->IsValid() &&
			(*DefaultValue)->Type != EJson::Null)
		{
			FString DefaultStr;
			switch ((*DefaultValue)->Type)
			{
			case EJson::String:
				DefaultStr = (*DefaultValue)->AsString();
				break;
			case EJson::Boolean:
				DefaultStr = (*DefaultValue)->AsBool() ? TEXT("true") : TEXT("false");
				break;
			case EJson::Number:
				DefaultStr = FString::SanitizeFloat((*DefaultValue)->AsNumber());
				break;
			default:
				// Structs / arrays passed as JSON aren't supported here —
				// caller uses blueprint.graph.set_pin_default on a Get/Set
				// node for complex values. Silently skip.
				break;
			}
			if (!DefaultStr.IsEmpty())
			{
				// Write the string form into the BPVariableDescription
				// we just added. This is the canonical path for member-
				// variable defaults; the engine serialises VariableData
				// from this string on each BP recompile.
				for (FBPVariableDescription& Var : BP->NewVariables)
				{
					if (Var.VarName == VarFName)
					{
						Var.DefaultValue = DefaultStr;
						break;
					}
				}
			}
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("added"), true);
		Data->SetStringField(TEXT("name"), VariableName);
		Data->SetStringField(TEXT("type_category"),
			PinType.PinCategory.IsNone() ? FString() : PinType.PinCategory.ToString());
		if (PinType.PinSubCategoryObject.IsValid())
		{
			Data->SetStringField(TEXT("type_sub_path"),
				PinType.PinSubCategoryObject->GetPathName());
		}
		return Data;
	}

	/** `blueprint.remove_variable` handler body.
	 *
	 * Inverse of blueprint.add_variable. Non-reversible (removing a
	 * variable loses its default value + metadata; source control is the
	 * undo), so no `rollback` field per docs/handler-conventions.md §4.
	 * Idempotent: missing variable returns {removed:false,
	 * already_removed:true}, not an error.
	 */
	static TSharedRef<FJsonObject> HandleBlueprintRemoveVariable(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, VariableName;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("variable_name"), VariableName) ||
			VariableName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`variable_name` is required and must be a non-empty string"));
		}

		TSharedPtr<FJsonObject> BpErr;
		UBlueprint* BP = ResolveBlueprint(BpPath, BpErr);
		if (BP == nullptr) { return BpErr.ToSharedRef(); }

		const FName VarFName(*VariableName);

		bool bPresent = false;
		for (const FBPVariableDescription& Existing : BP->NewVariables)
		{
			if (Existing.VarName == VarFName)
			{
				bPresent = true;
				break;
			}
		}
		if (!bPresent)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("removed"), false);
			Data->SetBoolField(TEXT("already_removed"), true);
			Data->SetStringField(TEXT("variable_name"), VariableName);
			return Data;
		}

		// RemoveMemberVariable drops the FBPVariableDescription, fixes
		// up VariableGet/Set nodes, and calls MarkBlueprintAsStructurallyModified
		// internally — we don't need to do any of that here.
		FBlueprintEditorUtils::RemoveMemberVariable(BP, VarFName);

		// Defensive post-check: confirm the entry is actually gone.
		for (const FBPVariableDescription& Existing : BP->NewVariables)
		{
			if (Existing.VarName == VarFName)
			{
				return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
					FString::Printf(
						TEXT("FBlueprintEditorUtils::RemoveMemberVariable left '%s' on '%s'"),
						*VariableName, *BpPath));
			}
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("removed"), true);
		Data->SetBoolField(TEXT("already_removed"), false);
		Data->SetStringField(TEXT("variable_name"), VariableName);
		return Data;
	}

	// ==================================================================
	// blueprint.add_component / blueprint.remove_component (Wave C)
	//
	// Author the Simple Construction Script of a BP from outside Kismet.
	// add_component creates a new USCS_Node for the requested class and
	// either parents it under an existing SCS node or attaches at the
	// root (via SCS->AddNode, which the engine auto-reparents to
	// DefaultSceneRoot at construction time for scene components).
	//
	// remove_component is non-reversible (lost serialised defaults);
	// source control is the undo.
	// ==================================================================

	/** `blueprint.add_component` handler body. */
	static TSharedRef<FJsonObject> HandleBlueprintAddComponent(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, ComponentName, ComponentClassPath;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("component_name"), ComponentName) ||
			ComponentName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`component_name` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("component_class"), ComponentClassPath) ||
			ComponentClassPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`component_class` is required (full path like '/Script/Engine.StaticMeshComponent')"));
		}

		FString AttachTo = TEXT("DefaultSceneRoot");
		Args->TryGetStringField(TEXT("attach_to"), AttachTo);

		TSharedPtr<FJsonObject> BpErr;
		UBlueprint* BP = ResolveBlueprint(BpPath, BpErr);
		if (BP == nullptr) { return BpErr.ToSharedRef(); }

		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (SCS == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript (not an Actor-derived BP)"),
					*BpPath));
		}

		UClass* ComponentClass = LoadObject<UClass>(nullptr, *ComponentClassPath);
		if (ComponentClass == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("component_class '%s' could not be resolved to a UClass"),
					*ComponentClassPath));
		}
		if (!ComponentClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("component_class '%s' is not a UActorComponent subclass"),
					*ComponentClassPath));
		}
		if (ComponentClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("component_class '%s' is Abstract/Deprecated and cannot be instantiated"),
					*ComponentClassPath));
		}

		const FName ComponentFName(*ComponentName);
		if (SCS->FindSCSNode(ComponentFName) != nullptr)
		{
			TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
			Err->SetStringField(TEXT("error"),   TEXT("COMPONENT_EXISTS"));
			Err->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Component '%s' already exists on '%s'"),
					*ComponentName, *BpPath));
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("bp_path"),        BpPath);
			Detail->SetStringField(TEXT("component_name"), ComponentName);
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}

		USCS_Node* AttachParent = nullptr;
		bool bAttachAsRoot = false;
		const bool bWantsDefaultRoot =
			AttachTo.IsEmpty() || AttachTo.Equals(TEXT("DefaultSceneRoot"), ESearchCase::IgnoreCase);
		USCS_Node* RootNode = SCS->GetDefaultSceneRootNode();
		if (bWantsDefaultRoot)
		{
			bAttachAsRoot = true;
		}
		else if (RootNode && RootNode->GetVariableName() == FName(*AttachTo))
		{
			bAttachAsRoot = true;
		}
		else
		{
			AttachParent = SCS->FindSCSNode(FName(*AttachTo));
			if (AttachParent == nullptr)
			{
				TSharedRef<FJsonObject> Err = UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
					FString::Printf(TEXT("attach_to node '%s' not found on '%s'"),
						*AttachTo, *BpPath));
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("attach_to"), AttachTo);
				Detail->SetStringField(TEXT("bp_path"),   BpPath);
				Err->SetObjectField(TEXT("detail"), Detail);
				return Err;
			}
		}

		BP->Modify();
		SCS->Modify();
		USCS_Node* NewNode = SCS->CreateNode(ComponentClass, ComponentFName);
		if (NewNode == nullptr || NewNode->ComponentTemplate == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(TEXT("USimpleConstructionScript::CreateNode returned null for '%s' of class '%s'"),
					*ComponentName, *ComponentClassPath));
		}

		if (bAttachAsRoot)
		{
			SCS->AddNode(NewNode);
		}
		else
		{
			AttachParent->Modify();
			AttachParent->AddChildNode(NewNode);
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("added"), true);
		Data->SetStringField(TEXT("component_name"), ComponentName);
		Data->SetStringField(TEXT("class_path"),     ComponentClass->GetPathName());
		Data->SetStringField(TEXT("attach_to"),
			bAttachAsRoot ? TEXT("DefaultSceneRoot") : AttachTo);
		{
			TSharedRef<FJsonObject> RbArgs = MakeShared<FJsonObject>();
			RbArgs->SetStringField(TEXT("bp_path"),        BpPath);
			RbArgs->SetStringField(TEXT("component_name"), ComponentName);
			TSharedRef<FJsonObject> Rb = MakeShared<FJsonObject>();
			Rb->SetStringField(TEXT("tool"), TEXT("blueprint.remove_component"));
			Rb->SetObjectField(TEXT("args"), RbArgs);
			Data->SetObjectField(TEXT("rollback"), Rb);
		}
		return Data;
	}

	/** `blueprint.remove_component` handler body. NON-REVERSIBLE. */
	static TSharedRef<FJsonObject> HandleBlueprintRemoveComponent(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, ComponentName;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("component_name"), ComponentName) ||
			ComponentName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`component_name` is required and must be a non-empty string"));
		}

		TSharedPtr<FJsonObject> BpErr;
		UBlueprint* BP = ResolveBlueprint(BpPath, BpErr);
		if (BP == nullptr) { return BpErr.ToSharedRef(); }

		USimpleConstructionScript* SCS = BP->SimpleConstructionScript;
		if (SCS == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript"),
					*BpPath));
		}

		const FName ComponentFName(*ComponentName);
		USCS_Node* Node = SCS->FindSCSNode(ComponentFName);
		if (Node == nullptr)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("removed"),          false);
			Data->SetBoolField(TEXT("already_removed"),  true);
			Data->SetStringField(TEXT("component_name"), ComponentName);
			return Data;
		}

		BP->Modify();
		SCS->Modify();
		SCS->RemoveNode(Node, /*bValidateSceneRootNodes=*/true);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("removed"),          true);
		Data->SetBoolField(TEXT("already_removed"),  false);
		Data->SetStringField(TEXT("component_name"), ComponentName);
		return Data;
	}

	// ==================================================================
	// blueprint.set_component_property (Wave C)
	//
	// Write a default on a BP's SCS component template (not a runtime
	// instance). Routes through the existing reflection core —
	// FUeMcpPropertyAccessor::SetValue — so path grammar, type coercion,
	// and error taxonomy are identical to `set_property`.
	// ==================================================================

	/** `blueprint.set_component_property` handler body. */
	static TSharedRef<FJsonObject> HandleBlueprintSetComponentProperty(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, ComponentName, PropertyPath;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("component_name"), ComponentName)
			|| ComponentName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`component_name` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("property_path"), PropertyPath)
			|| PropertyPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`property_path` is required and must be a non-empty string"));
		}
		const TSharedPtr<FJsonValue> RawValue = Args->TryGetField(TEXT("value"));
		if (!RawValue.IsValid())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`value` is required (may be null, bool, number, string, array, or object)"));
		}

		TSharedPtr<FJsonObject> BpErr;
		UBlueprint* BP = ResolveBlueprint(BpPath, BpErr);
		if (BP == nullptr) { return BpErr.ToSharedRef(); }

		if (BP->SimpleConstructionScript == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("Blueprint '%s' has no SimpleConstructionScript"),
					*BpPath));
		}

		const FName TargetName(*ComponentName);
		USCS_Node* Found = nullptr;
		for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
		{
			if (Node != nullptr && Node->GetVariableName() == TargetName)
			{
				Found = Node;
				break;
			}
		}
		if (Found == nullptr)
		{
			TArray<TSharedPtr<FJsonValue>> Known;
			for (USCS_Node* Node : BP->SimpleConstructionScript->GetAllNodes())
			{
				if (Node != nullptr)
				{
					Known.Add(MakeShared<FJsonValueString>(Node->GetVariableName().ToString()));
				}
			}
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetArrayField(TEXT("known_components"), Known);
			TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
			Err->SetStringField(TEXT("error"), TEXT("NOT_FOUND"));
			Err->SetStringField(TEXT("message"),
				FString::Printf(TEXT("Component '%s' not found on '%s'"),
					*ComponentName, *BpPath));
			Err->SetObjectField(TEXT("detail"), Detail);
			return Err;
		}

		UActorComponent* Template = Found->ComponentTemplate;
		if (Template == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(TEXT("SCS node '%s' on '%s' has a null ComponentTemplate"),
					*ComponentName, *BpPath));
		}

		FUeMcpPropertyValue PreValue;
		FUeMcpAccessorErrorInfo PreErr;
		const bool bHavePreState = FUeMcpPropertyAccessor::GetValue(
			Template, PropertyPath, PreValue, PreErr);

		Template->Modify();
		BP->Modify();

		FUeMcpAccessorErrorInfo WriteErr;
		if (!FUeMcpPropertyAccessor::SetValue(
				Template, PropertyPath, RawValue, WriteErr))
		{
			// Accessor-error -> wire-code goes through the shared
			// `UeMcp::AccessorErrorToCode` (issue #62); no local switch.
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("error"), UeMcp::AccessorErrorToCode(WriteErr.Code));
			Out->SetStringField(TEXT("message"), WriteErr.Message);
			if (WriteErr.Detail.IsValid())
			{
				Out->SetObjectField(TEXT("detail"), WriteErr.Detail);
			}
			return Out;
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("set"), true);
		Data->SetStringField(TEXT("component_name"), ComponentName);
		Data->SetStringField(TEXT("property_path"), PropertyPath);
		Data->SetStringField(TEXT("bp_path"), BpPath);
		if (bHavePreState && PreValue.Json.IsValid())
		{
			Data->SetField(TEXT("previous_value"), PreValue.Json);
			TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
			Rollback->SetStringField(TEXT("tool"),
				TEXT("blueprint.set_component_property"));
			TSharedRef<FJsonObject> RbArgs = MakeShared<FJsonObject>();
			RbArgs->SetStringField(TEXT("bp_path"), BpPath);
			RbArgs->SetStringField(TEXT("component_name"), ComponentName);
			RbArgs->SetStringField(TEXT("property_path"), PropertyPath);
			RbArgs->SetField(TEXT("value"), PreValue.Json);
			Rollback->SetObjectField(TEXT("args"), RbArgs);
			Data->SetObjectField(TEXT("rollback"), Rollback);
		}
		return Data;
	}

	// ==================================================================
	// blueprint.add_function (Wave C)
	//
	// Create a user-defined function graph with input params on the
	// FunctionEntry and output params on the FunctionResult. Reuses
	// ParseVariableType (forward-decl at top of namespace).
	// ==================================================================

	static bool AccessStringToFuncFlags(
		const FString& Access, int32& OutAddFlags, FString& OutError)
	{
		const FString Lower = Access.ToLower();
		OutAddFlags = 0;
		if (Lower == TEXT("public"))    { return true; }
		if (Lower == TEXT("protected")) { OutAddFlags = FUNC_Protected; return true; }
		if (Lower == TEXT("private"))   { OutAddFlags = FUNC_Private;   return true; }
		OutError = FString::Printf(
			TEXT("`access` must be 'public', 'protected', or 'private' (got '%s')"),
			*Access);
		return false;
	}

	struct FParsedFunctionParam
	{
		FName           Name;
		FEdGraphPinType PinType;
		bool            bHasDefault = false;
		FString         DefaultValue;
	};

	static bool ParseFunctionParamArray(
		const TArray<TSharedPtr<FJsonValue>>* Arr,
		const TCHAR* FieldLabel,
		TArray<FParsedFunctionParam>& OutParams,
		TSharedPtr<FJsonObject>& OutErr)
	{
		if (Arr == nullptr) { return true; }
		OutParams.Reserve(Arr->Num());
		for (int32 i = 0; i < Arr->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Entry = (*Arr)[i];
			if (!Entry.IsValid() || Entry->Type != EJson::Object)
			{
				OutErr = UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
					FString::Printf(TEXT("`%s[%d]` must be an object"), FieldLabel, i));
				return false;
			}
			const TSharedPtr<FJsonObject>& Obj = Entry->AsObject();

			FString ParamName, ParamType;
			if (!Obj->TryGetStringField(TEXT("name"), ParamName) || ParamName.IsEmpty())
			{
				OutErr = UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
					FString::Printf(TEXT("`%s[%d].name` is required"), FieldLabel, i));
				return false;
			}
			if (!Obj->TryGetStringField(TEXT("type"), ParamType) || ParamType.IsEmpty())
			{
				OutErr = UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
					FString::Printf(TEXT("`%s[%d].type` is required"), FieldLabel, i));
				return false;
			}

			FParsedFunctionParam P;
			P.Name = FName(*ParamName);
			FString TypeErr;
			if (!ParseVariableType(ParamType, P.PinType, TypeErr))
			{
				OutErr = UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
					FString::Printf(TEXT("`%s[%d].type`: %s"), FieldLabel, i, *TypeErr));
				return false;
			}

			FString DefaultStr;
			if (Obj->TryGetStringField(TEXT("default"), DefaultStr))
			{
				P.bHasDefault = true;
				P.DefaultValue = DefaultStr;
			}
			OutParams.Add(MoveTemp(P));
		}
		return true;
	}

	/** `blueprint.add_function` handler body. */
	static TSharedRef<FJsonObject> HandleBlueprintAddFunction(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath, FunctionName;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}
		if (!Args->TryGetStringField(TEXT("name"), FunctionName) || FunctionName.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`name` is required and must be a non-empty string"));
		}

		FString AccessStr = TEXT("public");
		Args->TryGetStringField(TEXT("access"), AccessStr);
		int32 AccessFlags = 0;
		{
			FString AccessErr;
			if (!AccessStringToFuncFlags(AccessStr, AccessFlags, AccessErr))
			{
				return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"), AccessErr);
			}
		}

		TSharedPtr<FJsonObject> BpErr;
		UBlueprint* BP = ResolveBlueprint(BpPath, BpErr);
		if (BP == nullptr) { return BpErr.ToSharedRef(); }

		const FName NewGraphName(*FunctionName);
		for (const UEdGraph* Existing : BP->FunctionGraphs)
		{
			if (Existing != nullptr && Existing->GetFName() == NewGraphName)
			{
				return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
					FString::Printf(
						TEXT("Function '%s' already exists on '%s'"),
						*FunctionName, *BpPath));
			}
		}

		TArray<FParsedFunctionParam> Inputs, Outputs;
		{
			const TArray<TSharedPtr<FJsonValue>>* InArr = nullptr;
			Args->TryGetArrayField(TEXT("inputs"), InArr);
			TSharedPtr<FJsonObject> Err;
			if (!ParseFunctionParamArray(InArr, TEXT("inputs"), Inputs, Err))
			{
				return Err.ToSharedRef();
			}
		}
		{
			const TArray<TSharedPtr<FJsonValue>>* OutArr = nullptr;
			Args->TryGetArrayField(TEXT("outputs"), OutArr);
			TSharedPtr<FJsonObject> Err;
			if (!ParseFunctionParamArray(OutArr, TEXT("outputs"), Outputs, Err))
			{
				return Err.ToSharedRef();
			}
		}

		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, NewGraphName,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());
		if (NewGraph == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("FBlueprintEditorUtils::CreateNewGraph returned nullptr"));
		}

		FBlueprintEditorUtils::AddFunctionGraph<UClass>(
			BP, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/nullptr);

		UK2Node_FunctionEntry*  EntryNode  = nullptr;
		UK2Node_FunctionResult* ResultNode = nullptr;
		for (UEdGraphNode* Node : NewGraph->Nodes)
		{
			if (EntryNode == nullptr)  { EntryNode  = Cast<UK2Node_FunctionEntry>(Node); }
			if (ResultNode == nullptr) { ResultNode = Cast<UK2Node_FunctionResult>(Node); }
		}
		if (EntryNode == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				TEXT("AddFunctionGraph did not produce a UK2Node_FunctionEntry"));
		}

		if (AccessFlags != 0)
		{
			EntryNode->AddExtraFlags(AccessFlags);
		}

		for (const FParsedFunctionParam& P : Inputs)
		{
			UEdGraphPin* Pin = EntryNode->CreateUserDefinedPin(
				P.Name, P.PinType, EGPD_Output, /*bUseUniqueName=*/false);
			if (Pin == nullptr)
			{
				return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
					FString::Printf(
						TEXT("Entry CreateUserDefinedPin returned nullptr for '%s'"),
						*P.Name.ToString()));
			}
			if (P.bHasDefault)
			{
				Pin->DefaultValue = P.DefaultValue;
				Pin->AutogeneratedDefaultValue = P.DefaultValue;
			}
		}

		if (Outputs.Num() > 0 && ResultNode == nullptr)
		{
			FGraphNodeCreator<UK2Node_FunctionResult> Creator(*NewGraph);
			ResultNode = Creator.CreateNode();
			ResultNode->FunctionReference.SetSelfMember(NewGraphName);
			ResultNode->NodePosX = EntryNode->NodePosX + 400;
			ResultNode->NodePosY = EntryNode->NodePosY;
			Creator.Finalize();
		}
		for (const FParsedFunctionParam& P : Outputs)
		{
			UEdGraphPin* Pin = ResultNode->CreateUserDefinedPin(
				P.Name, P.PinType, EGPD_Input, /*bUseUniqueName=*/false);
			if (Pin == nullptr)
			{
				return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
					FString::Printf(
						TEXT("Result CreateUserDefinedPin returned nullptr for '%s'"),
						*P.Name.ToString()));
			}
		}

		if (EntryNode  != nullptr) { EntryNode->ReconstructNode();  }
		if (ResultNode != nullptr) { ResultNode->ReconstructNode(); }

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("added"), true);
		Data->SetStringField(TEXT("name"), FunctionName);
		Data->SetStringField(TEXT("function_graph_path"), NewGraph->GetPathName());
		Data->SetStringField(TEXT("entry_node_id"),  EntryNode->NodeGuid.ToString());
		if (ResultNode != nullptr)
		{
			Data->SetStringField(TEXT("result_node_id"), ResultNode->NodeGuid.ToString());
		}

		TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
		Rollback->SetStringField(TEXT("tool"), TEXT("blueprint.remove_function"));
		TSharedRef<FJsonObject> RbArgs = MakeShared<FJsonObject>();
		RbArgs->SetStringField(TEXT("bp_path"), BpPath);
		RbArgs->SetStringField(TEXT("name"),    FunctionName);
		Rollback->SetObjectField(TEXT("args"), RbArgs);
		Data->SetObjectField(TEXT("rollback"), Rollback);
		return Data;
	}

	// ==================================================================
	// blueprint.create — authoring tool.
	//
	// Create a new UBlueprint asset at `parent_dir/name` with the given
	// parent class. Idempotent per `handler-conventions.md §3`: natural
	// key is the resulting package path; `on_conflict` selects skip (the
	// default), update (no-op today — reparenting is v1.1-deferred), or
	// error. Emits a rollback pointing at `blueprint.delete` on any
	// response where `created: true`.
	// ==================================================================

	/** Canonicalise `parent_dir + name` into a `/Game/...` package path
	 *  with no trailing slash / no duplicate slashes. */
	static FString BuildPackagePath(const FString& ParentDir, const FString& Name)
	{
		FString Dir = ParentDir;
		while (Dir.EndsWith(TEXT("/"))) { Dir.LeftChopInline(1, EAllowShrinking::No); }
		if (Dir.IsEmpty()) { Dir = TEXT("/Game/Blueprints"); }
		if (!Dir.StartsWith(TEXT("/"))) { Dir = TEXT("/") + Dir; }
		return Dir / Name;
	}

	/** `blueprint.create` handler body. */
	static TSharedRef<FJsonObject> HandleBlueprintCreate(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString Name;
		if (!Args->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`name` is required and must be a non-empty string"));
		}

		FString ParentClassPath = TEXT("/Script/Engine.Actor");
		Args->TryGetStringField(TEXT("parent_class"), ParentClassPath);

		FString ParentDir = TEXT("/Game/Blueprints/");
		Args->TryGetStringField(TEXT("parent_dir"), ParentDir);

		FString OnConflict = TEXT("skip");
		Args->TryGetStringField(TEXT("on_conflict"), OnConflict);
		OnConflict = OnConflict.ToLower();
		if (OnConflict != TEXT("skip") && OnConflict != TEXT("update") &&
			OnConflict != TEXT("error"))
		{
			return UeMcp::MakeInlineError(TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`on_conflict` must be one of 'skip', 'update', 'error' (got '%s')"),
					*OnConflict));
		}

		// parent_class must be a full path (e.g. `/Script/Engine.Actor`).
		// ANY_PACKAGE-based short-name lookup was deprecated in UE 5.1+.
		UClass* ParentClass = LoadObject<UClass>(nullptr, *ParentClassPath);
		if (ParentClass == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
				FString::Printf(TEXT("parent_class '%s' could not be resolved to a UClass (use a full path like '/Script/Engine.Actor')"),
					*ParentClassPath));
		}

		const FString PackagePath = BuildPackagePath(ParentDir, Name);
		const FString ObjectPath  = PackagePath + TEXT(".") + Name;
		const FString ClassPath   = ObjectPath + TEXT("_C");

		// --- existence check (natural-key idempotency) -------------------
		UBlueprint* Existing = LoadObject<UBlueprint>(nullptr, *ObjectPath);
		if (Existing != nullptr)
		{
			if (OnConflict == TEXT("error"))
			{
				TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
				Err->SetStringField(TEXT("error"),   TEXT("BLUEPRINT_EXISTS"));
				Err->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Blueprint already exists at '%s'"), *ObjectPath));
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("bp_path"), ObjectPath);
				Err->SetObjectField(TEXT("detail"), Detail);
				return Err;
			}

			UClass* ExistingParent = Existing->ParentClass;
			if (ExistingParent != ParentClass)
			{
				TSharedRef<FJsonObject> Err = MakeShared<FJsonObject>();
				Err->SetStringField(TEXT("error"),   TEXT("TYPE_MISMATCH"));
				Err->SetStringField(TEXT("message"),
					FString::Printf(TEXT("Existing blueprint '%s' has parent '%s', not requested '%s'"),
						*ObjectPath,
						ExistingParent ? *ExistingParent->GetPathName() : TEXT("(null)"),
						*ParentClass->GetPathName()));
				TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
				Detail->SetStringField(TEXT("bp_path"), ObjectPath);
				Detail->SetStringField(TEXT("expected_parent_class"), ParentClass->GetPathName());
				Detail->SetStringField(TEXT("actual_parent_class"),
					ExistingParent ? ExistingParent->GetPathName() : FString());
				Err->SetObjectField(TEXT("detail"), Detail);
				return Err;
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("created"),  false);
			Data->SetBoolField(TEXT("existed"),  true);
			if (OnConflict == TEXT("update"))
			{
				// Reparenting is v1.1-deferred; parent matched so nothing to do.
				Data->SetBoolField(TEXT("updated"), false);
			}
			Data->SetStringField(TEXT("bp_path"),    ObjectPath);
			Data->SetStringField(TEXT("class_path"), ClassPath);
			return Data;
		}

		// --- create path -------------------------------------------------
		if (!FKismetEditorUtilities::CanCreateBlueprintOfClass(ParentClass))
		{
			return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(TEXT("Parent class '%s' is not a legal blueprint parent (abstract / NotBlueprintable)"),
					*ParentClass->GetPathName()));
		}

		UPackage* Pkg = CreatePackage(*PackagePath);
		if (Pkg == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(TEXT("CreatePackage('%s') returned null"), *PackagePath));
		}
		Pkg->FullyLoad();

		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Pkg,
			FName(*Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			FName(TEXT("UeMcpBlueprintCreate")));
		if (BP == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(TEXT("FKismetEditorUtilities::CreateBlueprint returned null for '%s'"),
					*ObjectPath));
		}

		FAssetRegistryModule::AssetCreated(BP);
		Pkg->MarkPackageDirty();

		bool bSaved = false;
		if (GEditor != nullptr)
		{
			if (UEditorAssetSubsystem* AssetSubsystem =
				GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
			{
				bSaved = AssetSubsystem->SaveLoadedAsset(BP, /*bOnlyIfIsDirty=*/false);
			}
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("created"),    true);
		Data->SetBoolField(TEXT("existed"),    false);
		Data->SetStringField(TEXT("bp_path"),    ObjectPath);
		Data->SetStringField(TEXT("class_path"), ClassPath);
		if (!bSaved)
		{
			Data->SetBoolField(TEXT("saved"), false);
			Data->SetStringField(TEXT("save_error"),
				TEXT("SaveLoadedAsset returned false or editor subsystem unavailable; "
					 "asset exists in-memory, caller may retry save"));
		}
		else
		{
			Data->SetBoolField(TEXT("saved"), true);
		}

		// Rollback — inverse is blueprint.delete by path.
		{
			TSharedRef<FJsonObject> RbArgs = MakeShared<FJsonObject>();
			RbArgs->SetStringField(TEXT("bp_path"), ObjectPath);
			TSharedRef<FJsonObject> Rb = MakeShared<FJsonObject>();
			Rb->SetStringField(TEXT("tool"), TEXT("blueprint.delete"));
			Rb->SetObjectField(TEXT("args"), RbArgs);
			Data->SetObjectField(TEXT("rollback"), Rb);
		}

		return Data;
	}

	// ==================================================================
	// blueprint.delete — NON-REVERSIBLE authoring tool.
	//
	// Deletes a blueprint asset. Idempotent: a path that resolves to no
	// asset returns {deleted:false, already_deleted:true}. Per
	// `handler-conventions.md §4` this tool emits NO `rollback` field.
	// ==================================================================

	/** `blueprint.delete` handler body. */
	static TSharedRef<FJsonObject> HandleBlueprintDelete(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString BpPath;
		if (!Args->TryGetStringField(TEXT("bp_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}

		UEditorAssetSubsystem* AssetSubsystem =
			GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
		if (AssetSubsystem == nullptr)
		{
			return UeMcp::MakeInlineError(TEXT("EDITOR_NOT_READY"),
				TEXT("UEditorAssetSubsystem unavailable"));
		}

		if (!AssetSubsystem->DoesAssetExist(BpPath))
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetBoolField(TEXT("deleted"),         false);
			Data->SetBoolField(TEXT("already_deleted"), true);
			Data->SetStringField(TEXT("bp_path"),       BpPath);
			return Data;
		}

		const bool bDeleted = AssetSubsystem->DeleteAsset(BpPath);
		if (!bDeleted)
		{
			return UeMcp::MakeInlineError(TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(TEXT("UEditorAssetSubsystem::DeleteAsset returned false for '%s' (asset may be referenced / checked out / read-only)"),
					*BpPath));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("deleted"),         true);
		Data->SetBoolField(TEXT("already_deleted"), false);
		Data->SetStringField(TEXT("bp_path"),       BpPath);
		return Data;
	}

	// ==================================================================
	// blueprint.find_function — READ-ONLY discovery of Blueprint-callable
	// UFunctions. Motivated by UE-version-specific path churn
	// (AActor.K2_DestroyActor vs DestroyActor, Subtract_DoubleDouble vs
	// Subtract_FloatFloat). Scoped either to a single class, to a BP's
	// inherited surface, or globally across all loaded UClasses.
	// ==================================================================

	/** One candidate row. Held in a list while we rank, then emitted to JSON. */
	struct FFindFunctionCandidate
	{
		UClass*    OwnerClass = nullptr;
		UFunction* Function   = nullptr;
		int32      Rank       = 2; // 0=exact, 1=starts-with, 2=contains
	};

	/** Render one UFunction to the response shape. */
	static TSharedRef<FJsonObject> BuildFindFunctionRow(
		UClass* OwnerClass, UFunction* Fn)
	{
		TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("class_path"),
			OwnerClass ? OwnerClass->GetPathName() : FString());
		Row->SetStringField(TEXT("function_name"), Fn->GetName());

		FString DisplayName = Fn->GetMetaData(TEXT("DisplayName"));
		if (DisplayName.IsEmpty())
		{
			DisplayName = Fn->GetName();
		}
		Row->SetStringField(TEXT("display_name"), DisplayName);

		FProperty* ReturnProp = Fn->GetReturnProperty();
		FString Sig = ReturnProp ? ReturnProp->GetCPPType() : FString(TEXT("void"));
		Sig += TEXT(" ");
		Sig += Fn->GetName();
		Sig += TEXT("(");
		bool bFirstParam = true;
		for (TFieldIterator<FProperty> PIt(Fn); PIt; ++PIt)
		{
			FProperty* P = *PIt;
			if (P == nullptr || P == ReturnProp) { continue; }
			if (!P->HasAnyPropertyFlags(CPF_Parm)) { continue; }
			if (!bFirstParam) { Sig += TEXT(", "); }
			Sig += P->GetCPPType();
			Sig += TEXT(" ");
			Sig += P->GetName();
			bFirstParam = false;
		}
		Sig += TEXT(")");
		Row->SetStringField(TEXT("signature"), Sig);

		Row->SetBoolField(TEXT("is_static"),
			Fn->HasAnyFunctionFlags(FUNC_Static));
		Row->SetBoolField(TEXT("is_pure"),
			Fn->HasAnyFunctionFlags(FUNC_BlueprintPure));

		const FString Tooltip = Fn->GetMetaData(TEXT("Tooltip"));
		if (!Tooltip.IsEmpty())
		{
			Row->SetStringField(TEXT("tooltip"), Tooltip);
		}
		return Row;
	}

	/** Could an agent call this UFunction from a BP graph? */
	static bool IsBlueprintCallableFn(UFunction* Fn)
	{
		if (Fn == nullptr) { return false; }
		if (Fn->HasAnyFunctionFlags(FUNC_Delegate)) { return false; }
		if (Fn->HasMetaData(TEXT("DeprecatedFunction"))) { return false; }
		const EFunctionFlags CallableMask = FUNC_BlueprintCallable | FUNC_BlueprintPure;
		return Fn->HasAnyFunctionFlags(CallableMask);
	}

	/** 0=exact, 1=starts-with, 2=contains, -1=no match. */
	static int32 RankNameMatch(const FString& Name, const FString& Fragment)
	{
		if (Fragment.IsEmpty()) { return 2; }
		if (Name.Equals(Fragment, ESearchCase::IgnoreCase)) { return 0; }
		if (Name.StartsWith(Fragment, ESearchCase::IgnoreCase)) { return 1; }
		if (Name.Contains(Fragment, ESearchCase::IgnoreCase)) { return 2; }
		return -1;
	}

	/** Scan a class's own + (optionally) inherited UFunctions and append matches. */
	static void ScanClassFunctions(
		UClass* Cls,
		const FString& NameFragment,
		bool bIncludeSupers,
		TArray<FFindFunctionCandidate>& OutHits,
		FUeMcpCancelToken& Cancel,
		int32& FnCounter)
	{
		if (Cls == nullptr) { return; }

		const EFieldIteratorFlags::SuperClassFlags SuperFlag =
			bIncludeSupers
				? EFieldIteratorFlags::IncludeSuper
				: EFieldIteratorFlags::ExcludeSuper;

		for (TFieldIterator<UFunction> It(Cls, SuperFlag); It; ++It)
		{
			UFunction* Fn = *It;
			++FnCounter;
			if ((FnCounter % 200) == 0 && Cancel.IsCancellationRequested())
			{
				return;
			}
			if (!IsBlueprintCallableFn(Fn)) { continue; }
			const int32 Rank = RankNameMatch(Fn->GetName(), NameFragment);
			if (Rank < 0) { continue; }
			UClass* Owner = Fn->GetOwnerClass();
			if (Owner == nullptr) { Owner = Cls; }
			FFindFunctionCandidate C;
			C.OwnerClass = Owner;
			C.Function   = Fn;
			C.Rank       = Rank;
			OutHits.Add(C);
		}
	}

	/** `blueprint.find_function` handler body. READ-ONLY. */
	static TSharedRef<FJsonObject> HandleBlueprintFindFunction(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		check(IsInGameThread());

		FString NameFragment;
		Args->TryGetStringField(TEXT("name_fragment"), NameFragment);

		FString ClassPathArg, BlueprintPathArg;
		Args->TryGetStringField(TEXT("class_path"), ClassPathArg);
		Args->TryGetStringField(TEXT("blueprint_path"), BlueprintPathArg);

		int32 MaxResults = 20;
		int32 MaxResultsIn = 0;
		if (Args->TryGetNumberField(TEXT("max_results"), MaxResultsIn) && MaxResultsIn > 0)
		{
			MaxResults = FMath::Min(MaxResultsIn, 500);
		}

		TArray<FFindFunctionCandidate> Hits;
		Hits.Reserve(64);
		int32 FnCounter = 0;

		if (!BlueprintPathArg.IsEmpty())
		{
			TSharedPtr<FJsonObject> BpErr;
			UBlueprint* BP = ResolveBlueprint(BlueprintPathArg, BpErr);
			if (BP == nullptr)
			{
				return BpErr.ToSharedRef();
			}
			UClass* Gen = BP->GeneratedClass;
			if (Gen == nullptr)
			{
				return UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
					FString::Printf(
						TEXT("Blueprint '%s' has no GeneratedClass yet; compile it first"),
						*BlueprintPathArg));
			}
			ScanClassFunctions(Gen, NameFragment,
				/*bIncludeSupers=*/ true, Hits, Cancel, FnCounter);
		}
		else if (!ClassPathArg.IsEmpty())
		{
			UClass* Cls = LoadObject<UClass>(nullptr, *ClassPathArg);
			if (Cls == nullptr)
			{
				return UeMcp::MakeInlineError(TEXT("NOT_FOUND"),
					FString::Printf(
						TEXT("Could not load class '%s'"),
						*ClassPathArg));
			}
			ScanClassFunctions(Cls, NameFragment,
				/*bIncludeSupers=*/ false, Hits, Cancel, FnCounter);
		}
		else
		{
			for (TObjectIterator<UClass> It; It; ++It)
			{
				UClass* Cls = *It;
				if (Cls == nullptr) { continue; }
				const FString CN = Cls->GetName();
				if (CN.StartsWith(TEXT("SKEL_")) ||
					CN.StartsWith(TEXT("REINST_")) ||
					CN.StartsWith(TEXT("TRASHCLASS_")) ||
					CN.StartsWith(TEXT("HOTRELOADED_")))
				{
					continue;
				}
				ScanClassFunctions(Cls, NameFragment,
					/*bIncludeSupers=*/ false, Hits, Cancel, FnCounter);
				if (Cancel.IsCancellationRequested())
				{
					return UeMcp::MakeInlineError(TEXT("CANCELLED"),
						TEXT("blueprint.find_function cancelled mid-scan"));
				}
			}
		}

		Hits.Sort([](const FFindFunctionCandidate& A, const FFindFunctionCandidate& B)
		{
			if (A.Rank != B.Rank) { return A.Rank < B.Rank; }
			const FString AC = A.OwnerClass ? A.OwnerClass->GetPathName() : FString();
			const FString BC = B.OwnerClass ? B.OwnerClass->GetPathName() : FString();
			if (AC != BC) { return AC < BC; }
			return A.Function->GetName() < B.Function->GetName();
		});

		TArray<TSharedPtr<FJsonValue>> CandidatesJson;
		const int32 Emit = FMath::Min(Hits.Num(), MaxResults);
		CandidatesJson.Reserve(Emit);
		for (int32 i = 0; i < Emit; ++i)
		{
			CandidatesJson.Add(MakeShared<FJsonValueObject>(
				BuildFindFunctionRow(Hits[i].OwnerClass, Hits[i].Function)));
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetArrayField(TEXT("candidates"), CandidatesJson);
		Data->SetNumberField(TEXT("total_matches"), Hits.Num());
		Data->SetBoolField(TEXT("truncated"), Hits.Num() > Emit);
		return Data;
	}
}

void UeMcp::RegisterBlueprintAuthoringHandler(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpBlueprintAuthoringHandlerPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.graph.add_node"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleAddNode);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.graph.connect_pins"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleConnectPins);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.graph.disconnect_pins"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleDisconnectPins);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.graph.set_pin_default"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleSetPinDefault);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.graph.remove_node"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleRemoveNode);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.graph.compile"));
		Reg.DefaultTimeoutSeconds = CompileTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleCompile);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.graph.add_node_with_defaults"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleAddNodeWithDefaults);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.add_variable"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintAddVariable);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.remove_variable"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintRemoveVariable);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.create"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintCreate);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.delete"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintDelete);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.find_function"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintFindFunction);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.add_component"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintAddComponent);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.remove_component"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintRemoveComponent);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.set_component_property"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintSetComponentProperty);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.add_function"));
		Reg.DefaultTimeoutSeconds = AuthoringDefaultTimeoutSeconds;
		Reg.bMutating = true;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintAddFunction);
		Dispatcher.RegisterTool(Reg);
	}
}
