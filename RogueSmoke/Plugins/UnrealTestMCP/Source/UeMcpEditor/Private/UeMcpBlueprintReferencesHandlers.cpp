// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
//
// `blueprint.find_variable_references` / `blueprint.find_function_references`
// — call-site search across all Blueprints in scope. Equivalent to UE's
// "Find References" magnifier in the BP editor.
//
// Algorithm
// =========
//
// 1. Resolve the target `blueprint_path` to a `UBlueprint*` and pull its
//    `GeneratedClass`. Set `target.found_definition` based on whether the
//    named symbol exists on that class (by `FindPropertyByName` /
//    `FindFunctionByName` — these also pick up inherited members from a
//    C++ parent).
//
// 2. Enumerate Blueprints in scope via `IAssetRegistry::GetAssets` with
//    `Filter.ClassPaths.Add(FTopLevelAssetPath("/Script/Engine", "Blueprint"))`
//    + `bRecursiveClasses=true` so we pick up Animation BPs, Widget BPs,
//    etc. (every UBlueprint subclass). Optional `scope_paths` narrow the
//    package paths; default is the entire asset registry.
//
// 3. For each `FAssetData`: optionally skip if not loaded
//    (`scan_loaded_only=true` → fast path). Else `Asset.GetAsset()` to
//    force-load. Skip nulls. Skip the target BP itself in the scan iteration
//    so a get/set inside the source BP doesn't double-count its own
//    declaration.
//
// 4. Walk the graph collections — `UbergraphPages`, `FunctionGraphs`,
//    `MacroGraphs`. For each graph, iterate `Graph->Nodes` and dispatch on
//    node class:
//      - Variable refs: `UK2Node_Variable*` (parent of VariableGet/Set).
//      - Function refs: `UK2Node_CallFunction*` (call sites),
//        `UK2Node_Event*` (custom-event/UnrealEvent declarations matching
//        the name — the BP editor's Find Refs surfaces these too).
//    Match by `MemberName` AND `MemberParentClass(BlueprintClass)` so
//    callers don't get false positives from same-named members on
//    unrelated classes.
//
// 5. Caps: 5000 BPs scanned OR 10s wall-clock — emit `truncated: true`.
//    The handler is read-only and not pending — it runs synchronously
//    inside a single game-thread hop. The wall-cap protects the plugin
//    socket from holding the line during a worst-case full-project scan.
//
// Scan modes (`scan_mode` arg, default "full")
// ============================================
//
// "full"        — the algorithm above. Loads every in-scope BP via
//                  GetAsset() and walks K2 graphs. Ground-truth, node-
//                  precise, but the first call after editor launch pays
//                  the cold-load cost (capped 5000 BPs / 10s).
//
// "loaded_only" — same graph walk, but only over BPs already resident in
//                  memory (FAssetData::IsAssetLoaded()). No GetAsset()
//                  load. Microsecond-class; node-precise for the BPs the
//                  editor has open, silent on the rest. Equivalent to the
//                  legacy `scan_loaded_only=true` boolean (still honored
//                  for back-compat: `scan_loaded_only=true` with no
//                  explicit `scan_mode` is treated as "loaded_only").
//
// "lazy"        — AR-tag-only. Loads NOTHING. Cannot walk graphs, so it
//                  cannot return node-level call sites. Instead it returns
//                  the *candidate* set: BPs that, by asset-registry class
//                  metadata alone, *could* reference the symbol —
//                    (a) any BP whose generated class is the target class
//                        or derives from it (self-context Get/Set/Call of
//                        an inherited member), resolved purely from the
//                        `ParentClassPath` tag graph via
//                        IAssetRegistry::GetDerivedClassNames (no load);
//                    (b) any BP that lists the target class in its
//                        `ImplementedInterfaces` AR tag.
//                  Restricted to BPs whose `FindInBlueprintsData` tag is
//                  non-empty (i.e. they actually have searchable graph
//                  metadata — empties are data-only / never-compiled and
//                  cannot host a call site). Results carry
//                  `accuracy:"approximate"` and a per-candidate
//                  `match_basis`; there is NO graph-walk precision. This
//                  is the "what BPs *might* use this?" go-to lookup, meant
//                  to be narrowed by a follow-up "full" scan scoped to the
//                  returned packages.

#include "UeMcpBlueprintReferencesHandlers.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/ARFilter.h"
#include "Blueprint/BlueprintSupport.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/MemberReference.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_Variable.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Misc/DateTime.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include "UeMcpDispatcher.h"
#include "UeMcpEditorSubsystem.h"
#include "UeMcpGameThreadExecutor.h"
#include "UeMcpObjectResolver.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpBlueprintReferencesHandlersPrivate
{
	// Read-only diagnostics with a hard 10s cap baked into the algorithm —
	// 30s timeout gives us comfortable headroom over the cap so the cap
	// (not the dispatcher) is what the caller sees on a full-project scan.
	static constexpr double DefaultTimeoutSeconds = 30.0;

	// Hard caps from the spec. Kept as constants so a future tuning pass
	// has one place to change.
	static constexpr int32 MaxBlueprintsScanned = 5000;
	static constexpr double WallClockCapSeconds = 10.0;

	// Log every N scanned BPs at Verbose so a stuck scan is diagnosable
	// without spamming Display. Power-of-two for cheap modulo.
	static constexpr int32 ScanProgressLogStride = 256;

	// ------------------------------------------------------------------
	// Resolve the target blueprint path to (UBlueprint*, UClass*). Mirrors
	// the authoring handler's pattern but trimmed to read-only — no level
	// blueprints, no _C class fall-through. The caller passes a BP asset
	// path, full stop.
	// ------------------------------------------------------------------
	static UBlueprint* ResolveTargetBlueprint(
		const FString& BpPath, TSharedPtr<FJsonObject>& OutErr)
	{
		UeMcp::FUeMcpResolvedObject Resolved = UeMcp::ResolveObject(BpPath, nullptr);
		if (!Resolved.IsOk())
		{
			OutErr = Resolved.ErrorInfo;
			return nullptr;
		}

		if (UBlueprint* BP = Cast<UBlueprint>(Resolved.Object))
		{
			return BP;
		}

		// `_C` path lands on the UClass — walk back to the authoring BP.
		if (UClass* Cls = Cast<UClass>(Resolved.Object))
		{
			if (UBlueprint* BP = Cast<UBlueprint>(Cls->ClassGeneratedBy))
			{
				return BP;
			}
		}

		OutErr = UeMcp::MakeInlineError(
			TEXT("TYPE_MISMATCH"),
			FString::Printf(
				TEXT("'%s' did not resolve to a UBlueprint asset"),
				*BpPath));
		return nullptr;
	}

	// ------------------------------------------------------------------
	// Read optional `scope_paths` — array of package paths. Empty array
	// (or missing field) means "all loaded assets visible to the registry".
	// Each entry must start with `/Game/...` (or another mount root); we
	// don't validate that, the asset registry does.
	// ------------------------------------------------------------------
	static TArray<FName> ParseScopePaths(const TSharedRef<FJsonObject>& Args)
	{
		TArray<FName> Out;
		const TArray<TSharedPtr<FJsonValue>>* ArrPtr = nullptr;
		if (!Args->TryGetArrayField(TEXT("scope_paths"), ArrPtr) || ArrPtr == nullptr)
		{
			return Out;
		}
		Out.Reserve(ArrPtr->Num());
		for (const TSharedPtr<FJsonValue>& V : *ArrPtr)
		{
			if (!V.IsValid() || V->Type != EJson::String)
			{
				continue;
			}
			FString Str = V->AsString();
			if (Str.IsEmpty())
			{
				continue;
			}
			Out.Add(FName(*Str));
		}
		return Out;
	}

	// ------------------------------------------------------------------
	// Scan-mode selection. `scan_mode` (string) is the forward-looking
	// knob; the legacy `scan_loaded_only` (bool) is still honored so
	// existing callers don't break. Precedence:
	//   1. explicit `scan_mode` string wins outright;
	//   2. else `scan_loaded_only=true` → Loaded;
	//   3. else Full.
	// An unrecognised `scan_mode` string is reported as a SCHEMA_ERROR by
	// the driver (we return Invalid here so the caller gets a clear msg
	// instead of silently falling back to Full).
	// ------------------------------------------------------------------
	enum class EScanMode : uint8 { Full, Loaded, Lazy, Invalid };

	static EScanMode ParseScanMode(const TSharedRef<FJsonObject>& Args)
	{
		FString ModeStr;
		if (Args->TryGetStringField(TEXT("scan_mode"), ModeStr) && !ModeStr.IsEmpty())
		{
			if (ModeStr.Equals(TEXT("full"),        ESearchCase::IgnoreCase)) return EScanMode::Full;
			if (ModeStr.Equals(TEXT("loaded_only"), ESearchCase::IgnoreCase)) return EScanMode::Loaded;
			if (ModeStr.Equals(TEXT("lazy"),        ESearchCase::IgnoreCase)) return EScanMode::Lazy;
			return EScanMode::Invalid;
		}

		bool bScanLoadedOnly = false;
		(void)Args->TryGetBoolField(TEXT("scan_loaded_only"), bScanLoadedOnly);
		return bScanLoadedOnly ? EScanMode::Loaded : EScanMode::Full;
	}

	static const TCHAR* ScanModeWireName(EScanMode Mode)
	{
		switch (Mode)
		{
			case EScanMode::Full:   return TEXT("full");
			case EScanMode::Loaded: return TEXT("loaded_only");
			case EScanMode::Lazy:   return TEXT("lazy");
			default:                return TEXT("invalid");
		}
	}

	// ------------------------------------------------------------------
	// Classify a graph by which UBlueprint collection it lives on. The
	// stage-1 reader and authoring handler use the same buckets — the
	// labels here match the wire contract the agent expects.
	// ------------------------------------------------------------------
	static const TCHAR* ClassifyGraphKind(UBlueprint* BP, UEdGraph* Graph)
	{
		if (BP == nullptr || Graph == nullptr) return TEXT("Unknown");
		for (UEdGraph* G : BP->UbergraphPages)        { if (G == Graph) return TEXT("EventGraph"); }
		for (UEdGraph* G : BP->FunctionGraphs)        { if (G == Graph) return TEXT("Function");   }
		for (UEdGraph* G : BP->MacroGraphs)           { if (G == Graph) return TEXT("Macro");      }
		for (UEdGraph* G : BP->DelegateSignatureGraphs){ if (G == Graph) return TEXT("Delegate");  }
		return TEXT("Unknown");
	}

	// ------------------------------------------------------------------
	// Build one `references[]` entry. The shape is the wire contract from
	// the task spec. `node_kind` is caller-supplied (Get/Set/Call/Event)
	// so this stays neutral.
	// ------------------------------------------------------------------
	static TSharedRef<FJsonObject> BuildReferenceEntry(
		const FString&  BpPath,
		const FString&  GraphName,
		const TCHAR*    GraphKind,
		const FString&  FunctionName,  // empty for non-Function graphs
		UEdGraphNode*   Node,
		const FString&  NodeKind)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("blueprint_path"), BpPath);
		Out->SetStringField(TEXT("graph_name"),     GraphName);
		Out->SetStringField(TEXT("graph_kind"),     GraphKind);
		if (!FunctionName.IsEmpty())
		{
			Out->SetStringField(TEXT("function_name"), FunctionName);
		}

		if (Node != nullptr)
		{
			Out->SetStringField(TEXT("node_guid"),
				Node->NodeGuid.ToString(EGuidFormats::Digits));
			Out->SetStringField(TEXT("node_kind"),  NodeKind);
			Out->SetStringField(TEXT("node_title"),
				Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

			TArray<TSharedPtr<FJsonValue>> Pos;
			Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosX));
			Pos.Add(MakeShared<FJsonValueNumber>(Node->NodePosY));
			Out->SetArrayField(TEXT("node_pos"), Pos);

			if (!Node->NodeComment.IsEmpty())
			{
				Out->SetStringField(TEXT("comment"), Node->NodeComment);
			}
		}
		return Out;
	}

	// ------------------------------------------------------------------
	// Build the list of UBlueprints to scan via the asset registry, then
	// optionally narrow to just the loaded ones. The returned array is
	// the work list; ordering is whatever the registry hands us.
	// ------------------------------------------------------------------
	static void GatherBlueprintAssets(
		const TArray<FName>& ScopePaths,
		bool                 bLoadedOnly,
		TArray<FAssetData>&  OutAssets)
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry"));
		IAssetRegistry& Registry = AssetRegistryModule.Get();

		FARFilter Filter;
		// Walk the entire UBlueprint hierarchy so Animation / Widget /
		// custom subclasses are all included. The class path is the
		// stable UE 5.x form (FTopLevelAssetPath); a string-FName-based
		// `ClassNames` lookup would log a deprecation warning.
		Filter.ClassPaths.Add(FTopLevelAssetPath(
			TEXT("/Script/Engine"), TEXT("Blueprint")));
		Filter.bRecursiveClasses = true;

		if (ScopePaths.Num() > 0)
		{
			Filter.PackagePaths = ScopePaths;
			Filter.bRecursivePaths = true;
		}

		Registry.GetAssets(Filter, OutAssets);

		if (bLoadedOnly)
		{
			OutAssets.RemoveAll([](const FAssetData& AD)
			{
				return !AD.IsAssetLoaded();
			});
		}

		// Defensive filter: skip any UBlueprint whose AR tag says it has
		// no `GeneratedClass`. Loading such a BP via `GetAsset()` runs
		// `UBlueprint::PostLoad`, which asserts at Blueprint.cpp:793 when
		// SCS is null AND GeneratedClass is null — fatal `check()`, not
		// catchable. Stale / never-compiled BPs left behind in shared test
		// projects are the most common source. The tag is `TT_Hidden` but
		// always populated by `UBlueprint::GetAssetRegistryTags`, so an
		// EMPTY value is the precise tell-tale for "this asset would
		// crash us if we touched it".
		OutAssets.RemoveAll([](const FAssetData& AD)
		{
			const FString GeneratedClassTag =
				AD.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath);
			return GeneratedClassTag.IsEmpty();
		});
	}

	// ------------------------------------------------------------------
	// Convert a UBlueprint to its asset path string (`/Game/.../BP_Foo`).
	// We strip the package's tail object name so the result matches what
	// callers pass on the wire.
	// ------------------------------------------------------------------
	static FString BlueprintAssetPath(UBlueprint* BP)
	{
		if (BP == nullptr) return FString();
		// `GetPackage()->GetName()` yields `/Game/.../BP_Foo` — exactly the
		// wire shape (no trailing `.BP_Foo` object subpath, no `_C`).
		if (UPackage* Pkg = BP->GetOutermost())
		{
			return Pkg->GetName();
		}
		return BP->GetPathName();
	}

	// ------------------------------------------------------------------
	// Walk every node of every relevant graph on `BP` and collect refs.
	// The matcher is a callable that receives a node and returns either
	// (matched=false) or (matched=true, node_kind=...). Centralises the
	// bucket-traversal so variable / function paths share it.
	// ------------------------------------------------------------------
	template <typename TMatcher>
	static void WalkAndCollect(
		UBlueprint*                                  BP,
		const FString&                               BpPath,
		TMatcher                                     Matcher,
		TArray<TSharedPtr<FJsonValue>>&              OutRefs)
	{
		if (BP == nullptr) return;

		auto WalkOneBucket = [&](const TArray<TObjectPtr<UEdGraph>>& Bucket)
		{
			for (UEdGraph* Graph : Bucket)
			{
				if (Graph == nullptr) continue;

				const TCHAR* GraphKind = ClassifyGraphKind(BP, Graph);
				const FString GraphName = Graph->GetName();
				// For Function graphs the graph name IS the host function.
				const FString FunctionName =
					(FCString::Strcmp(GraphKind, TEXT("Function")) == 0) ? GraphName : FString();

				for (UEdGraphNode* Node : Graph->Nodes)
				{
					if (Node == nullptr) continue;
					FString NodeKind;
					if (!Matcher(Node, NodeKind))
					{
						continue;
					}
					OutRefs.Add(MakeShared<FJsonValueObject>(
						BuildReferenceEntry(
							BpPath, GraphName, GraphKind, FunctionName, Node, NodeKind)));
				}
			}
		};

		WalkOneBucket(BP->UbergraphPages);
		WalkOneBucket(BP->FunctionGraphs);
		WalkOneBucket(BP->MacroGraphs);
	}

	// ------------------------------------------------------------------
	// VARIABLE matcher. Matches `UK2Node_Variable*` whose VariableReference
	// resolves to (TargetVarName, TargetClass).
	//
	// `GetMemberParentClass(SelfScope)` returns SelfScope when the ref is
	// self-context — i.e. an internal Get/Set inside a BP that derives
	// from TargetClass references TargetClass's variable, but the parent-
	// class field on the FMemberReference itself is null. We pass the
	// scanned BP's class as SelfScope to handle both external (`MyOther.X`)
	// and self-context (`X` inside MyOther where MyOther extends Source)
	// references uniformly.
	//
	// For variables we ALSO accept a parent-class match: a BP that
	// inherits from BP_RefSrc and has its own `Get Health` node references
	// BP_RefSrc.Health when SelfScope's class chain contains TargetClass.
	// We compare with `IsChildOf` to keep that semantic.
	// ------------------------------------------------------------------
	struct FVariableMatcher
	{
		FName    TargetName;
		UClass*  TargetClass = nullptr;

		bool operator()(UEdGraphNode* Node, FString& OutKind) const
		{
			UK2Node_Variable* Var = Cast<UK2Node_Variable>(Node);
			if (Var == nullptr) return false;
			if (Var->VariableReference.GetMemberName() != TargetName) return false;

			UClass* SelfScope = Var->GetBlueprintClassFromNode();
			UClass* OwnerClass =
				Var->VariableReference.GetMemberParentClass(SelfScope);
			if (OwnerClass == nullptr) return false;

			// SelfScope-as-OwnerClass means the ref is "self.<name>" in a
			// BP that subclasses TargetClass — accept when the chain
			// reaches TargetClass.
			if (OwnerClass != TargetClass && !OwnerClass->IsChildOf(TargetClass))
			{
				return false;
			}

			OutKind = (Cast<UK2Node_VariableSet>(Var) != nullptr)
				? FString(TEXT("Set"))
				: FString(TEXT("Get"));
			return true;
		}
	};

	// ------------------------------------------------------------------
	// FUNCTION matcher. Matches:
	//   - `UK2Node_CallFunction` whose FunctionReference -> (Name, Class)
	//     → `node_kind = "Call"`.
	//   - `UK2Node_Event` whose EventReference -> (Name, Class)
	//     → `node_kind = "Event"` (covers UnrealEvents bound by name and
	//     ComponentBoundEvent — both subclass UK2Node_Event).
	//   - `UK2Node_CustomEvent` (also a UK2Node_Event subclass) is folded
	//     into the same path; its EventReference uses Name = the custom
	//     event name and class = the BP's own GeneratedClass when in a
	//     self-context. Handled by the `IsChildOf` widening below.
	//
	// We deliberately do NOT match `UK2Node_FunctionEntry` for function
	// references — that's the callee's own declaration, not a call site,
	// and the BP editor's Find Refs surfaces it as the *definition*, not
	// a reference. Including it would double-count the source BP's own
	// function graph entry.
	// ------------------------------------------------------------------
	struct FFunctionMatcher
	{
		FName    TargetName;
		UClass*  TargetClass = nullptr;

		bool operator()(UEdGraphNode* Node, FString& OutKind) const
		{
			if (UK2Node_CallFunction* Call = Cast<UK2Node_CallFunction>(Node))
			{
				if (Call->FunctionReference.GetMemberName() != TargetName) return false;
				UClass* SelfScope = Call->GetBlueprintClassFromNode();
				UClass* OwnerClass =
					Call->FunctionReference.GetMemberParentClass(SelfScope);
				if (OwnerClass == nullptr) return false;
				if (OwnerClass != TargetClass && !OwnerClass->IsChildOf(TargetClass))
				{
					return false;
				}
				OutKind = TEXT("Call");
				return true;
			}

			if (UK2Node_Event* Evt = Cast<UK2Node_Event>(Node))
			{
				// CustomEvent: the EventReference's MemberName is the custom
				// event name; the parent class is the BP's own generated
				// class. We only want CustomEvents whose name matches AND
				// whose owning class is the target — i.e. the custom event
				// declared on TargetClass. ComponentBoundEvent matches
				// similarly when its delegate property name equals our
				// search name.
				if (Evt->EventReference.GetMemberName() != TargetName) return false;
				UClass* SelfScope = Evt->GetBlueprintClassFromNode();
				UClass* OwnerClass =
					Evt->EventReference.GetMemberParentClass(SelfScope);
				if (OwnerClass == nullptr) return false;
				if (OwnerClass != TargetClass && !OwnerClass->IsChildOf(TargetClass))
				{
					return false;
				}
				OutKind = TEXT("Event");
				return true;
			}

			return false;
		}
	};

	// ------------------------------------------------------------------
	// Strip a UE export-path tag value down to the bare top-level asset
	// path. AR class tags are stored in export form, e.g.
	//   BlueprintGeneratedClass'/Game/BPs/BP_Foo.BP_Foo_C'
	//   Class'/Script/Engine.Actor'
	// We want `/Game/BPs/BP_Foo.BP_Foo_C` resp. `/Script/Engine.Actor`.
	// Tolerates a bare path with no `Type'...'` wrapper as well.
	// ------------------------------------------------------------------
	static FString ExportPathToObjectPath(const FString& In)
	{
		FString S = In;
		int32 Quote;
		if (S.FindChar(TCHAR('\''), Quote))
		{
			S = S.RightChop(Quote + 1);
			S.RemoveFromEnd(TEXT("'"));
		}
		S.TrimStartAndEndInline();
		return S;
	}

	// ------------------------------------------------------------------
	// LAZY (AR-tag-only) candidate scan. Loads NOTHING.
	//
	// The honest, no-load answer to "which BPs could reference symbol S on
	// class C?" derivable from asset-registry metadata alone:
	//
	//   (a) Inheritance set — any BP whose generated class IS C or derives
	//       from C. Such a BP can have a self-context Get/Set/Call of an
	//       inherited member. Built purely from the `ParentClassPath` tag
	//       graph via IAssetRegistry::GetDerivedClassNames (zero loads).
	//
	//   (b) Interface implementors — any BP whose `ImplementedInterfaces`
	//       AR tag names C (function references through an interface the
	//       BP implements). Parsed from the exported struct-array string,
	//       same substring shape UE's own ExtractUnloadedFiBData uses
	//       (FindInBlueprintManager.cpp:2369).
	//
	// Restricted to BPs whose `FindInBlueprintsData` tag is NON-EMPTY:
	// that tag holds the Find-in-Blueprints graph index (Blueprint.cpp:
	// 1143). Empty ⇒ data-only / never-compiled / no graphs ⇒ cannot host
	// a call site, so it can be pruned without loading.
	//
	// This is APPROXIMATE: it is the over-set of BPs that *might* contain a
	// reference (no node-level proof), and it intentionally does not prove
	// that any specific Get/Set/Call node exists. Each candidate carries a
	// `match_basis`. Callers narrow with a follow-up "full" scan scoped to
	// the returned packages.
	// ------------------------------------------------------------------
	static TSharedRef<FJsonObject> RunLazyCandidateScan(
		const TSharedRef<FJsonObject>& Args,
		const FString&                 BpPath,
		const TCHAR*                   SymbolField,
		const FString&                 SymbolName,
		UClass*                        TargetClass,
		bool                           bFoundDefinition,
		const TArray<FName>&           ScopePaths)
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(
				TEXT("AssetRegistry"));
		IAssetRegistry& Registry = AssetRegistryModule.Get();

		// Enable temporary caching so the GetDerivedClassNames /
		// GetAssets sweep over a large project stays cheap (the AR API
		// docs explicitly call this out as the slow-vs-fast switch).
		Registry.SetTemporaryCachingMode(true);
		ON_SCOPE_EXIT { Registry.SetTemporaryCachingMode(false); };

		// --- (a) inheritance closure of the target generated class ------
		const FTopLevelAssetPath TargetClassPath(TargetClass->GetPathName());
		TSet<FTopLevelAssetPath> DerivedClassPaths;
		{
			TArray<FTopLevelAssetPath> Roots;
			Roots.Add(TargetClassPath);
			TSet<FTopLevelAssetPath> Excluded;
			Registry.GetDerivedClassNames(Roots, Excluded, DerivedClassPaths);
		}
		// GetDerivedClassNames is inclusive of the roots in UE 5.x, but be
		// defensive — a self-reference inside the target's own class is a
		// legitimate candidate too.
		DerivedClassPaths.Add(TargetClassPath);

		// --- enumerate every BP asset in scope (metadata only) ----------
		FARFilter Filter;
		Filter.ClassPaths.Add(FTopLevelAssetPath(
			TEXT("/Script/Engine"), TEXT("Blueprint")));
		Filter.bRecursiveClasses = true;
		if (ScopePaths.Num() > 0)
		{
			Filter.PackagePaths   = ScopePaths;
			Filter.bRecursivePaths = true;
		}
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);

		const FString TargetClassObjPath = TargetClass->GetPathName();
		// Bare class name (no package) for the cheap interface substring
		// pre-test before the precise compare.
		FString TargetClassName = TargetClassObjPath;
		{
			int32 Dot;
			if (TargetClassName.FindLastChar(TCHAR('.'), Dot))
			{
				TargetClassName = TargetClassName.RightChop(Dot + 1);
			}
		}

		TArray<TSharedPtr<FJsonValue>> Candidates;
		int32 Examined = 0;

		for (const FAssetData& AD : Assets)
		{
			++Examined;

			// Stale-BP guard (gotcha §18): empty GeneratedClass tag ⇒ the
			// asset would crash a load; here we'd never load it, but an
			// empty value also means "no compiled class / no graphs", so
			// it cannot host a reference. Prune.
			const FString GenClassTag =
				AD.GetTagValueRef<FString>(FBlueprintTags::GeneratedClassPath);
			if (GenClassTag.IsEmpty() || GenClassTag == TEXT("None"))
			{
				continue;
			}

			// Must have graph search metadata to possibly host a call
			// site. Empty FiB tag ⇒ data-only / never-compiled.
			const FString FiBData =
				AD.GetTagValueRef<FString>(FBlueprintTags::FindInBlueprintsData);
			if (FiBData.IsEmpty())
			{
				continue;
			}

			const FString GenClassObjPath = ExportPathToObjectPath(GenClassTag);
			const bool bIsTargetItself =
				(GenClassObjPath == TargetClassObjPath);

			// (a) inheritance match
			bool bInheritsTarget = bIsTargetItself;
			if (!bInheritsTarget)
			{
				const FTopLevelAssetPath GenClassTLAP(GenClassObjPath);
				bInheritsTarget = DerivedClassPaths.Contains(GenClassTLAP);
			}

			// (b) interface-implementor match
			bool bImplementsTarget = false;
			if (!bInheritsTarget)
			{
				const FString Impl = AD.GetTagValueRef<FString>(
					FBlueprintTags::ImplementedInterfaces);
				if (!Impl.IsEmpty() &&
					Impl.Contains(TargetClassName, ESearchCase::CaseSensitive))
				{
					// Cheap substring hit — confirm with the same parse
					// shape UE uses so `MyIface2` doesn't match `MyIface`.
					bImplementsTarget =
						Impl.Contains(TargetClassObjPath, ESearchCase::CaseSensitive);
				}
			}

			if (!bInheritsTarget && !bImplementsTarget)
			{
				continue;
			}

			TSharedRef<FJsonObject> Cand = MakeShared<FJsonObject>();
			Cand->SetStringField(TEXT("blueprint_path"),
				AD.PackageName.ToString());
			Cand->SetStringField(TEXT("generated_class_path"),
				GenClassObjPath);
			Cand->SetStringField(TEXT("match_basis"),
				bIsTargetItself     ? TEXT("self")
				: bInheritsTarget   ? TEXT("inherits_target_class")
				:                     TEXT("implements_target_interface"));
			Candidates.Add(MakeShared<FJsonValueObject>(Cand));
		}

		TSharedRef<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("blueprint_path"), BpPath);
		TargetObj->SetStringField(TEXT("class_path"),
			TargetClass->GetPathName());
		TargetObj->SetStringField(SymbolField, SymbolName);
		TargetObj->SetBoolField(TEXT("found_definition"), bFoundDefinition);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("target"), TargetObj);
		Out->SetStringField(TEXT("scan_mode"), TEXT("lazy"));
		Out->SetStringField(TEXT("accuracy"), TEXT("approximate"));
		Out->SetStringField(TEXT("accuracy_note"),
			TEXT("AR-tag-only: no Blueprint was loaded and no graph was "
			     "walked. `candidates` is the over-set of Blueprints that "
			     "*could* reference the symbol (class inherits the target "
			     "or implements it as an interface, and the BP has graph "
			     "search metadata). There is NO per-node proof and there "
			     "may be false positives. Re-run with scan_mode=\"full\" "
			     "(optionally scope_paths to these packages) for "
			     "node-precise ground truth."));
		Out->SetNumberField(TEXT("blueprints_examined"), Examined);
		Out->SetArrayField(TEXT("candidates"), Candidates);
		Out->SetNumberField(TEXT("count"), Candidates.Num());
		// `references` intentionally omitted — lazy mode produces no
		// node-level references, only candidate BP paths. Keeping the key
		// absent (rather than an empty array) makes the shape difference
		// explicit so callers don't mistake "no nodes walked" for "zero
		// references found".
		return Out;
	}

	// ------------------------------------------------------------------
	// Shared driver: pull args, resolve target, gather BP assets, walk
	// each one through the supplied matcher. `bIsVariable` toggles the
	// `target.found_definition` lookup (FProperty vs UFunction) and the
	// arg-name (`variable_name` vs `function_name`).
	// ------------------------------------------------------------------
	template <typename TMatcher>
	static TSharedRef<FJsonObject> RunReferenceScan(
		const TSharedRef<FJsonObject>& Args,
		FUeMcpCancelToken&             Cancel,
		bool                           bIsVariable)
	{
		check(IsInGameThread());

		const TCHAR* SymbolField = bIsVariable
			? TEXT("variable_name")
			: TEXT("function_name");

		// ---- arg parse + validation -------------------------------------
		FString BpPath;
		if (!Args->TryGetStringField(TEXT("blueprint_path"), BpPath) || BpPath.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				TEXT("`blueprint_path` is required and must be a non-empty string"));
		}

		FString SymbolName;
		if (!Args->TryGetStringField(SymbolField, SymbolName) || SymbolName.IsEmpty())
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`%s` is required and must be a non-empty string"),
					SymbolField));
		}

		const EScanMode ScanMode = ParseScanMode(Args);
		if (ScanMode == EScanMode::Invalid)
		{
			FString Raw;
			Args->TryGetStringField(TEXT("scan_mode"), Raw);
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(
					TEXT("`scan_mode` must be one of \"full\", \"loaded_only\", "
					     "\"lazy\" (got '%s')"),
					*Raw));
		}
		const bool bLoadedOnly = (ScanMode == EScanMode::Loaded);

		const TArray<FName> ScopePaths = ParseScopePaths(Args);

		// ---- target resolution ------------------------------------------
		TSharedPtr<FJsonObject> ResolveErr;
		UBlueprint* TargetBP = ResolveTargetBlueprint(BpPath, ResolveErr);
		if (TargetBP == nullptr)
		{
			return ResolveErr.ToSharedRef();
		}
		UClass* TargetClass = TargetBP->GeneratedClass;
		if (TargetClass == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("PLUGIN_INTERNAL_ERROR"),
				FString::Printf(
					TEXT("Blueprint '%s' has no GeneratedClass; recompile the asset"),
					*BpPath));
		}

		const FString ClassPath = TargetClass->GetPathName();
		const FName SymbolFName(*SymbolName);

		// found_definition: does the symbol live on the class right now?
		// FindPropertyByName/FindFunctionByName both walk the inheritance
		// chain, so a C++-defined parent member counts as "found" even
		// though it's not declared on the BP itself. That's the right
		// semantic for a Find Refs UI.
		bool bFoundDefinition = false;
		if (bIsVariable)
		{
			bFoundDefinition = (TargetClass->FindPropertyByName(SymbolFName) != nullptr);
		}
		else
		{
			bFoundDefinition = (TargetClass->FindFunctionByName(SymbolFName) != nullptr);
		}

		// ---- lazy (AR-tag-only) short-circuit ---------------------------
		// No load, no graph walk — return the approximate candidate set
		// and we're done. Kept above the gather/scan so none of the
		// load-bearing machinery runs.
		if (ScanMode == EScanMode::Lazy)
		{
			return RunLazyCandidateScan(
				Args, BpPath, SymbolField, SymbolName,
				TargetClass, bFoundDefinition, ScopePaths);
		}

		// ---- gather work list -------------------------------------------
		TArray<FAssetData> Assets;
		GatherBlueprintAssets(ScopePaths, bLoadedOnly, Assets);

		// ---- scan -------------------------------------------------------
		TArray<TSharedPtr<FJsonValue>> RefArr;
		const FName TargetPackageName = TargetBP->GetOutermost()
			? TargetBP->GetOutermost()->GetFName()
			: NAME_None;

		const double StartSeconds = FPlatformTime::Seconds();
		int32 Scanned = 0;
		bool bTruncated = false;

		// Build the matcher once; copy into the lambda capture is cheap
		// (two pointer-sized fields).
		TMatcher Matcher;
		Matcher.TargetName  = SymbolFName;
		Matcher.TargetClass = TargetClass;

		for (const FAssetData& AD : Assets)
		{
			if (Scanned >= MaxBlueprintsScanned)
			{
				bTruncated = true;
				break;
			}
			if ((Scanned & (ScanProgressLogStride - 1)) == 0 && Scanned > 0)
			{
				const double Elapsed = FPlatformTime::Seconds() - StartSeconds;
				UE_LOG(LogUeMcpEditor, Verbose,
					TEXT("blueprint.find_refs scan: %d/%d, elapsed=%.2fs"),
					Scanned, Assets.Num(), Elapsed);
				if (Elapsed >= WallClockCapSeconds)
				{
					bTruncated = true;
					break;
				}
			}
			if (Cancel.IsCancellationRequested())
			{
				bTruncated = true;
				break;
			}

			// Skip the target BP — its own Get/Set on its own variable is
			// the *declaration* in the agent's mental model, not a "reference"
			// to itself. (For functions the FunctionEntry already isn't matched
			// by the matcher; this guard is the variable-case parallel.)
			// We compare on package name to avoid forcing an asset load.
			if (AD.PackageName == TargetPackageName)
			{
				++Scanned;
				continue;
			}

			// Gate behind scan_loaded_only — already filtered in
			// GatherBlueprintAssets, but the cap loop is the only place
			// we want to count work, so the redundant check stays OFF.

			UBlueprint* BP = Cast<UBlueprint>(AD.GetAsset());
			++Scanned;
			if (BP == nullptr) continue;

			const FString BpAssetPath = BlueprintAssetPath(BP);
			WalkAndCollect(BP, BpAssetPath, Matcher, RefArr);
		}

		// ---- response ---------------------------------------------------
		TSharedRef<FJsonObject> TargetObj = MakeShared<FJsonObject>();
		TargetObj->SetStringField(TEXT("blueprint_path"), BpPath);
		TargetObj->SetStringField(TEXT("class_path"),     ClassPath);
		TargetObj->SetStringField(SymbolField,            SymbolName);
		TargetObj->SetBoolField(TEXT("found_definition"), bFoundDefinition);

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetObjectField(TEXT("target"),       TargetObj);
		Out->SetStringField(TEXT("scan_mode"),    ScanModeWireName(ScanMode));
		// "full" / "loaded_only" walk K2 graphs ⇒ node-precise.
		Out->SetStringField(TEXT("accuracy"),     TEXT("exact"));
		Out->SetNumberField(TEXT("blueprints_scanned"), Scanned);
		Out->SetArrayField(TEXT("references"),    RefArr);
		Out->SetNumberField(TEXT("count"),        RefArr.Num());
		if (bTruncated)
		{
			Out->SetBoolField(TEXT("truncated"), true);
		}
		return Out;
	}

	// ------------------------------------------------------------------
	// Tool entry points — thin wrappers over the shared driver.
	// ------------------------------------------------------------------
	static TSharedRef<FJsonObject> HandleFindVariableReferences(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		return RunReferenceScan<FVariableMatcher>(Args, Cancel, /*bIsVariable*/ true);
	}

	static TSharedRef<FJsonObject> HandleFindFunctionReferences(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& Cancel)
	{
		return RunReferenceScan<FFunctionMatcher>(Args, Cancel, /*bIsVariable*/ false);
	}
}

void UeMcp::RegisterBlueprintReferencesHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpBlueprintReferencesHandlersPrivate;

	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.find_variable_references"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleFindVariableReferences);
		Dispatcher.RegisterTool(Reg);
	}
	{
		FUeMcpToolRegistration Reg;
		Reg.ToolName = FName(TEXT("blueprint.find_function_references"));
		Reg.DefaultTimeoutSeconds = DefaultTimeoutSeconds;
		Reg.bMutating = false;
		Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleFindFunctionReferences);
		Dispatcher.RegisterTool(Reg);
	}
}
