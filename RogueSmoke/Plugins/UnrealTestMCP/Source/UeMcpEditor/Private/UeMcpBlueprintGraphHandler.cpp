// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpBlueprintGraphHandler.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/Level.h"
#include "Engine/LevelScriptBlueprint.h"
#include "Engine/World.h"
#include "UObject/Class.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpBlueprintGraphHandlerPrivate
{
	/** Dispatcher timeout for `blueprint.graph`. Reads only — no disk I/O
	 *  beyond whatever the object-resolver's `TryLoad` may trigger on
	 *  first access. 20s mirrors `blueprint.outline`. A very-large BP
	 *  (thousands of nodes across many graphs) is kept in check by
	 *  `max_nodes_per_graph`, not by tightening this cap. */
	static constexpr double GraphTimeoutSeconds = 20.0;

	/** Default node-count cap per graph. Oversized graphs truncate
	 *  emission at this count and set `truncated: true`. Chosen to be
	 *  comfortably above the size of a typical hand-authored function
	 *  (~30) and a busy event graph (~100) without being so high that a
	 *  pathological graph blows the wire budget. Callers can override. */
	static constexpr int32 DefaultMaxNodesPerGraph = 200;

	/**
	 * The four graph buckets on `UBlueprint`, each tagged with the
	 * wire-level `kind` we emit. Mirrors the mapping documented in the
	 * tool contract: function graphs become `"function"` (or
	 * `"construction"` when they're the construction script), ubergraph
	 * pages become `"ubergraph"`, macros `"macro"`, delegate signatures
	 * `"delegate"`.
	 */
	enum class EGraphBucket : uint8
	{
		Function,
		Ubergraph,
		Macro,
		Delegate,
	};

	/** The construction script graph has a well-known engine-internal
	 *  name; we tag it `"construction"` instead of the generic
	 *  `"function"` so agents can filter by lifecycle phase without
	 *  matching on the name string. */
	static const TCHAR* const ConstructionScriptGraphName =
		TEXT("UserConstructionScript");

	/**
	 * Map a bucket + graph-name pair to the wire `kind` string.
	 *
	 * WHY the special case on function graphs: `UserConstructionScript`
	 * lives under `UBlueprint::FunctionGraphs` (engine models it as a
	 * specialized function), but semantically it's the construction
	 * script — callers want to distinguish it from hand-authored
	 * functions.
	 */
	static const TCHAR* BucketToKindString(EGraphBucket Bucket, const FString& GraphName)
	{
		switch (Bucket)
		{
			case EGraphBucket::Function:
				if (GraphName == ConstructionScriptGraphName)
				{
					return TEXT("construction");
				}
				return TEXT("function");
			case EGraphBucket::Ubergraph: return TEXT("ubergraph");
			case EGraphBucket::Macro:     return TEXT("macro");
			case EGraphBucket::Delegate:  return TEXT("delegate");
			default:                      return TEXT("unknown");
		}
	}

	/**
	 * Resolve a class/BP path to a `UBlueprint*` (plus the owning class).
	 * Unlike the introspection-handlers' shared helper we need only the
	 * blueprint branch here — native classes short-circuit to a stable
	 * `{is_native: true, graphs: []}` response before we ever call this.
	 *
	 * Returns nullptr and fills `OutErr` on failure. Passes nullptr as the
	 * world hint: asset resolution only (these handlers never want to
	 * match a live actor labelled `BP_Door`).
	 */
	static UObject* ResolveBlueprintAsset(
		const FString& ObjectId,
		UBlueprint*& OutBlueprint,
		UClass*& OutClass,
		TSharedPtr<FJsonObject>& OutErr)
	{
		OutBlueprint = nullptr;
		OutClass = nullptr;

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
			// Native classes have `ClassGeneratedBy == nullptr`; for
			// BP-generated classes, `ClassGeneratedBy` points at the
			// authoring `UBlueprint`. This is how `blueprint.outline`
			// rediscovers the asset from a `_C` path.
			if (UObject* ClassOuter = Cls->ClassGeneratedBy)
			{
				OutBlueprint = Cast<UBlueprint>(ClassOuter);
			}
		}
		else if (UWorld* World = Cast<UWorld>(Resolved.Object))
		{
			// Level Blueprint support. A level asset (`/Game/Maps/Foo` or
			// `/Game/Maps/Foo.Foo`) resolves to a UWorld; its level
			// script BP is a subobject of PersistentLevel. Read-only —
			// pass `bDontCreate=true` so a level without a Level BP
			// surfaces as a clean TYPE_MISMATCH rather than materialising
			// an empty BP on disk as a side effect of an outline read.
			if (ULevel* Persistent = World->PersistentLevel)
			{
				if (UBlueprint* LSB = Persistent->GetLevelScriptBlueprint(
						/*bDontCreate=*/ true))
				{
					OutBlueprint = LSB;
					OutClass = LSB->GeneratedClass;
				}
			}
		}

		if (OutClass == nullptr)
		{
			OutErr = UeMcp::MakeInlineError(TEXT("TYPE_MISMATCH"),
				FString::Printf(
					TEXT("'%s' did not resolve to a class or blueprint"),
					*ObjectId));
			return nullptr;
		}
		return Resolved.Object;
	}

	/**
	 * Walk EITHER a single pin's `LinkedTo` list and emit
	 * `{node, pin}` entries pointing at each connection's far end.
	 *
	 * WHY only from output pins (callers of this helper filter): each
	 * graph edge appears exactly once on the wire. Iterating both
	 * directions would emit each wire twice. Agents reconstructing the
	 * graph topology pick the convention up from the tool's doc.
	 */
	static TArray<TSharedPtr<FJsonValue>> RenderPinLinks(const UEdGraphPin* Pin)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		if (Pin == nullptr)
		{
			return Out;
		}
		Out.Reserve(Pin->LinkedTo.Num());
		for (const UEdGraphPin* Other : Pin->LinkedTo)
		{
			if (Other == nullptr)
			{
				continue;
			}
			TSharedRef<FJsonObject> Link = MakeShared<FJsonObject>();
			const UEdGraphNode* FarNode = Other->GetOwningNodeUnchecked();
			Link->SetStringField(TEXT("node"),
				FarNode ? FarNode->GetName() : FString());
			Link->SetStringField(TEXT("pin"), Other->PinName.ToString());
			Out.Add(MakeShared<FJsonValueObject>(Link));
		}
		return Out;
	}

	/**
	 * Build one pin's JSON representation. Shape (stable):
	 *   { name, direction, category, subcategory, default, object_default,
	 *     links? }
	 *
	 * `links` is emitted only for OUTPUT pins; input pins carry an empty
	 * list so the field shape stays stable without doubling the wire cost
	 * of every connection. See `RenderPinLinks` header comment.
	 */
	static TSharedRef<FJsonObject> BuildPinJson(const UEdGraphPin* Pin)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Pin == nullptr)
		{
			// Defensive: stable shape so callers don't crash on a corrupt
			// pin pointer. Real K2 graphs never have null pins in `Pins`,
			// but we harden anyway.
			Out->SetStringField(TEXT("name"),         FString());
			Out->SetStringField(TEXT("direction"),    TEXT("in"));
			Out->SetStringField(TEXT("category"),     FString());
			Out->SetStringField(TEXT("subcategory"),  FString());
			Out->SetField(TEXT("default"),        MakeShared<FJsonValueNull>());
			Out->SetField(TEXT("object_default"), MakeShared<FJsonValueNull>());
			Out->SetArrayField(TEXT("links"), TArray<TSharedPtr<FJsonValue>>());
			return Out;
		}

		Out->SetStringField(TEXT("name"), Pin->PinName.ToString());
		Out->SetStringField(TEXT("direction"),
			(Pin->Direction == EGPD_Output) ? TEXT("out") : TEXT("in"));

		// `PinCategory` / `PinSubCategory` are the most useful type
		// discriminators for an agent: `"exec"`, `"bool"`, `"int"`,
		// `"object"`, `"struct"`, etc. Subcategory distinguishes between
		// different `object` pins (the UClass name) or different `struct`
		// pins (the struct name).
		Out->SetStringField(TEXT("category"),
			Pin->PinType.PinCategory.IsNone() ? FString() : Pin->PinType.PinCategory.ToString());
		Out->SetStringField(TEXT("subcategory"),
			Pin->PinType.PinSubCategory.IsNone() ? FString() : Pin->PinType.PinSubCategory.ToString());

		// Scalar default: `DefaultValue` is the stringified literal the
		// editor shows in the details panel. Empty string on a freshly-
		// connected pin means "no literal, value comes from the wire" —
		// emit null to keep the "this pin has a literal" signal sharp.
		if (Pin->DefaultValue.IsEmpty())
		{
			Out->SetField(TEXT("default"), MakeShared<FJsonValueNull>());
		}
		else
		{
			Out->SetStringField(TEXT("default"), Pin->DefaultValue);
		}

		// Object default: non-null when the pin carries a literal object
		// reference (e.g. a hard-referenced `UStaticMesh*` default on a
		// `K2Node_VariableSet`). Emit the path name so agents can read or
		// pass it through. Null otherwise.
		if (UObject* DefaultObj = Pin->DefaultObject)
		{
			Out->SetStringField(TEXT("object_default"), DefaultObj->GetPathName());
		}
		else
		{
			Out->SetField(TEXT("object_default"), MakeShared<FJsonValueNull>());
		}

		// Emit connection edges from output pins only. Input pins get an
		// empty list (stable shape, zero duplication). Tracing from the
		// input side would emit the same wire twice.
		if (Pin->Direction == EGPD_Output)
		{
			Out->SetArrayField(TEXT("links"), RenderPinLinks(Pin));
		}
		else
		{
			Out->SetArrayField(TEXT("links"), TArray<TSharedPtr<FJsonValue>>());
		}
		return Out;
	}

	/**
	 * Build one node's JSON representation.
	 *
	 * `id`  — `Node->GetName()`, stable within its owning graph.
	 * `class` — the node's UClass short name (e.g. `K2Node_CallFunction`).
	 *           Agents pattern-match on this to categorise nodes without
	 *           having to parse the title.
	 * `title` — `GetNodeTitle(ListView).ToString()`; single-line human
	 *           label, matches what the editor's node browser shows.
	 * `pos` — emitted only when `bIncludePositions`. Positions are rarely
	 *         useful to an agent; keeping them opt-in keeps the payload
	 *         tight on large graphs.
	 */
	static TSharedRef<FJsonObject> BuildNodeJson(
		const UEdGraphNode* Node, bool bIncludePositions)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Node == nullptr)
		{
			Out->SetStringField(TEXT("id"),    FString());
			Out->SetStringField(TEXT("class"), FString());
			Out->SetStringField(TEXT("title"), FString());
			Out->SetArrayField(TEXT("pins"), TArray<TSharedPtr<FJsonValue>>());
			return Out;
		}

		Out->SetStringField(TEXT("id"), Node->GetName());
		UClass* NodeClass = Node->GetClass();
		Out->SetStringField(TEXT("class"),
			NodeClass ? NodeClass->GetName() : FString());
		Out->SetStringField(TEXT("title"),
			Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

		if (bIncludePositions)
		{
			TArray<TSharedPtr<FJsonValue>> Pos;
			Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
			Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
			Out->SetArrayField(TEXT("pos"), Pos);
		}

		TArray<TSharedPtr<FJsonValue>> PinValues;
		PinValues.Reserve(Node->Pins.Num());
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			PinValues.Add(MakeShared<FJsonValueObject>(BuildPinJson(Pin)));
		}
		Out->SetArrayField(TEXT("pins"), PinValues);
		return Out;
	}

	/**
	 * Render one `UEdGraph` to its wire JSON. Honours `MaxNodes` —
	 * emits the first `MaxNodes` entries and sets `truncated: true` if
	 * the graph has more. A cap of 0 is treated as "no cap"; negative
	 * values are rejected at the Python layer and cannot reach us.
	 *
	 * WHY the cap rather than a tight timeout: a pathological graph
	 * with thousands of nodes could exceed the 20s timeout mid-walk and
	 * return nothing useful. Bounded emission lets the caller see the
	 * first N nodes plus a signal that more exist.
	 */
	static TSharedRef<FJsonObject> BuildGraphJson(
		const UEdGraph* Graph,
		EGraphBucket Bucket,
		int32 MaxNodes,
		bool bIncludePositions)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Graph == nullptr)
		{
			Out->SetStringField(TEXT("name"), FString());
			Out->SetStringField(TEXT("kind"), TEXT("unknown"));
			Out->SetNumberField(TEXT("node_count"), 0);
			Out->SetBoolField(TEXT("truncated"), false);
			Out->SetArrayField(TEXT("nodes"), TArray<TSharedPtr<FJsonValue>>());
			return Out;
		}

		const FString GraphName = Graph->GetName();
		Out->SetStringField(TEXT("name"), GraphName);
		Out->SetStringField(TEXT("kind"),
			BucketToKindString(Bucket, GraphName));

		const int32 TotalNodes = Graph->Nodes.Num();
		Out->SetNumberField(TEXT("node_count"), TotalNodes);

		const bool bHasCap = MaxNodes > 0;
		const int32 EmitCount = bHasCap ? FMath::Min(TotalNodes, MaxNodes) : TotalNodes;
		const bool bTruncated = bHasCap && TotalNodes > MaxNodes;
		Out->SetBoolField(TEXT("truncated"), bTruncated);

		TArray<TSharedPtr<FJsonValue>> NodeValues;
		NodeValues.Reserve(EmitCount);
		for (int32 i = 0; i < EmitCount; ++i)
		{
			const UEdGraphNode* Node = Graph->Nodes[i];
			NodeValues.Add(MakeShared<FJsonValueObject>(
				BuildNodeJson(Node, bIncludePositions)));
		}
		Out->SetArrayField(TEXT("nodes"), NodeValues);
		return Out;
	}

	/**
	 * Describe every graph on `BP` as a sequence of `{Graph, Bucket}`
	 * pairs in emit order: functions (including construction script),
	 * ubergraph pages, macro graphs, delegate signature graphs. Matches
	 * the order callers see in the BP editor sidebar, roughly.
	 *
	 * Using a single helper keeps the two code paths (emit-all vs
	 * filter-by-name) from drifting on bucket tagging.
	 */
	struct FBPGraphEntry
	{
		UEdGraph* Graph = nullptr;
		EGraphBucket Bucket = EGraphBucket::Function;
	};

	static void CollectAllGraphs(UBlueprint* BP, TArray<FBPGraphEntry>& OutEntries)
	{
		if (BP == nullptr)
		{
			return;
		}
		auto Push = [&OutEntries](UEdGraph* G, EGraphBucket B)
		{
			if (G != nullptr)
			{
				OutEntries.Add({G, B});
			}
		};

		for (UEdGraph* G : BP->FunctionGraphs)   { Push(G, EGraphBucket::Function);  }
		for (UEdGraph* G : BP->UbergraphPages)   { Push(G, EGraphBucket::Ubergraph); }
		for (UEdGraph* G : BP->MacroGraphs)      { Push(G, EGraphBucket::Macro);     }
		for (UEdGraph* G : BP->DelegateSignatureGraphs)
		{
			Push(G, EGraphBucket::Delegate);
		}
	}

	/**
	 * Locate the first graph matching `TargetName` (case-sensitive —
	 * `UEdGraph::GetName()` is stable and callers get it from
	 * `blueprint.outline`'s `functions[].name`). Bucket search order
	 * mirrors `CollectAllGraphs`.
	 *
	 * First hit wins. In practice names are unique within a BP; a
	 * macro and a function with the same name would be an authoring
	 * collision the editor would already flag.
	 */
	static bool FindGraphByName(
		UBlueprint* BP, const FString& TargetName, FBPGraphEntry& OutEntry)
	{
		TArray<FBPGraphEntry> All;
		CollectAllGraphs(BP, All);
		for (const FBPGraphEntry& Entry : All)
		{
			if (Entry.Graph != nullptr && Entry.Graph->GetName() == TargetName)
			{
				OutEntry = Entry;
				return true;
			}
		}
		return false;
	}

	/** `blueprint.graph` body. */
	static TSharedRef<FJsonObject> HandleBlueprintGraph(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		FString AssetPath;
		if (!Args->TryGetStringField(TEXT("bp_path"), AssetPath) || AssetPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(TEXT("INVALID_PAYLOAD"),
				TEXT("`bp_path` is required and must be a non-empty string"));
		}

		// `function_name` / `include_positions` / `max_nodes_per_graph`
		// all optional. Missing is distinct from present-and-empty on the
		// wire; TryGetStringField sets the out-param only when the field
		// exists, giving us clean "absent" semantics.
		FString FunctionName;
		const bool bHasFunctionFilter =
			Args->TryGetStringField(TEXT("function_name"), FunctionName)
			&& !FunctionName.IsEmpty();

		bool bIncludePositions = false;
		Args->TryGetBoolField(TEXT("include_positions"), bIncludePositions);

		// `max_nodes_per_graph`: safety cap. 0 is the "no cap" sentinel
		// (see `BuildGraphJson`); negatives can't reach us because the
		// Python wrapper rejects them pre-hop. We still clamp defensively
		// in case a caller talks to the plugin without the wrapper.
		int32 MaxNodesPerGraph = DefaultMaxNodesPerGraph;
		int32 MaxNodesArg = 0;
		if (Args->TryGetNumberField(TEXT("max_nodes_per_graph"), MaxNodesArg))
		{
			MaxNodesPerGraph = FMath::Max(0, MaxNodesArg);
		}

		UBlueprint* BP = nullptr;
		UClass* Cls = nullptr;
		TSharedPtr<FJsonObject> Err;
		if (!ResolveBlueprintAsset(AssetPath, BP, Cls, Err))
		{
			return Err.ToSharedRef();
		}

		// Native class: stable empty-graphs shape. Matches the
		// `blueprint.outline` contract for the same case — agents don't
		// have to special-case "is this a BP or a C++ class" at their
		// layer.
		if (BP == nullptr)
		{
			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("asset_path"), AssetPath);
			Data->SetStringField(TEXT("class_path"),
				Cls ? Cls->GetPathName() : FString());
			Data->SetBoolField(TEXT("is_native"), true);
			Data->SetArrayField(TEXT("graphs"), TArray<TSharedPtr<FJsonValue>>());
			return Data;
		}

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), BP->GetPathName());
		Data->SetStringField(TEXT("class_path"),
			BP->GeneratedClass ? BP->GeneratedClass->GetPathName() : FString());
		Data->SetBoolField(TEXT("is_native"), false);

		TArray<TSharedPtr<FJsonValue>> GraphValues;

		if (bHasFunctionFilter)
		{
			FBPGraphEntry Hit;
			if (!FindGraphByName(BP, FunctionName, Hit))
			{
				// Borrow PROP_NOT_FOUND for "no graph by that name" —
				// semantically the closest code in the taxonomy (§2.2)
				// for "you asked for a named member that doesn't exist."
				return UeMcp::MakeInlineError(TEXT("PROP_NOT_FOUND"),
					FString::Printf(
						TEXT("Blueprint '%s' has no graph named '%s'"),
						*BP->GetName(), *FunctionName));
			}
			GraphValues.Add(MakeShared<FJsonValueObject>(
				BuildGraphJson(Hit.Graph, Hit.Bucket,
					MaxNodesPerGraph, bIncludePositions)));
		}
		else
		{
			TArray<FBPGraphEntry> All;
			CollectAllGraphs(BP, All);
			GraphValues.Reserve(All.Num());
			for (const FBPGraphEntry& Entry : All)
			{
				GraphValues.Add(MakeShared<FJsonValueObject>(
					BuildGraphJson(Entry.Graph, Entry.Bucket,
						MaxNodesPerGraph, bIncludePositions)));
			}
		}

		Data->SetArrayField(TEXT("graphs"), GraphValues);
		return Data;
	}
}

void UeMcp::RegisterBlueprintGraphHandler(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpBlueprintGraphHandlerPrivate;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("blueprint.graph"));
	Reg.DefaultTimeoutSeconds = GraphTimeoutSeconds;
	Reg.bMutating = false;
	Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleBlueprintGraph);
	Dispatcher.RegisterTool(Reg);
}
