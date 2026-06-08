// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpProjectHandlers.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Interfaces/IPluginManager.h"
#include "CoreGlobals.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "ShaderCompiler.h"

#include "UeMcpDispatcher.h"
#include "UeMcpGameThreadExecutor.h"

namespace UeMcpProjectHandlersPrivate
{
	static constexpr int32 WireProtocolVersion = 1;

	/** Plugin name that owns this binary. Used as the IPluginManager lookup
	 *  key — must match `UnrealTestMCP.uplugin`'s file stem. */
	static const TCHAR* const PluginName = TEXT("UnrealTestMCP");

	/** Read plugin VersionName from the loaded `.uplugin` descriptor.
	 *  IPluginManager keeps this in a hash map after module init, so the
	 *  per-call lookup is cheap. Returns "unknown" if the plugin can't be
	 *  found — a real symptom worth surfacing rather than masking with a
	 *  stale fallback string. */
	static FString ResolvePluginVersion()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
		if (!Plugin.IsValid())
		{
			return TEXT("unknown");
		}
		return Plugin->GetDescriptor().VersionName;
	}

	/** Stringify current PIE mode, if any. Null when PIE isn't active. */
	static TSharedPtr<FJsonValue> BuildPieMode()
	{
		if (GEditor == nullptr || !GEditor->IsPlayingSessionInEditor())
		{
			return MakeShared<FJsonValueNull>();
		}
		// PlayWorld may be null on the editor game instance pointer path;
		// GEditor->bIsSimulatingInEditor distinguishes simulate from play.
		const bool bIsSimulate = GEditor->bIsSimulatingInEditor;
		return MakeShared<FJsonValueString>(bIsSimulate ? TEXT("sie") : TEXT("pie"));
	}

	/** Build the editor-busy flags object. All cheap boolean reads. */
	static TSharedRef<FJsonObject> BuildEditorBusy()
	{
		TSharedRef<FJsonObject> Busy = MakeShared<FJsonObject>();

		const bool bCompiling =
			(GShaderCompilingManager != nullptr) && GShaderCompilingManager->IsCompiling();

		Busy->SetBoolField(TEXT("compiling"), bCompiling);
		Busy->SetBoolField(TEXT("gc"), IsGarbageCollecting());
		Busy->SetBoolField(TEXT("saving"), GIsSavingPackage);
		Busy->SetBoolField(TEXT("async_loading"), IsAsyncLoading());
		return Busy;
	}

	/** Discover which well-known test plugins are enabled. */
	static TSharedRef<FJsonObject> BuildEnabledTestPlugins()
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		bool bFunctionalTestingEditor = false;
		bool bCqTest = false;

		const TArray<TSharedRef<IPlugin>> All = IPluginManager::Get().GetDiscoveredPlugins();
		for (const TSharedRef<IPlugin>& Plugin : All)
		{
			if (!Plugin->IsEnabled())
			{
				continue;
			}
			const FString& Name = Plugin->GetName();
			if (Name.Equals(TEXT("FunctionalTestingEditor"), ESearchCase::IgnoreCase))
			{
				bFunctionalTestingEditor = true;
			}
			else if (Name.Equals(TEXT("CQTest"), ESearchCase::IgnoreCase))
			{
				bCqTest = true;
			}
		}

		Out->SetBoolField(TEXT("functional_testing_editor"), bFunctionalTestingEditor);
		Out->SetBoolField(TEXT("cqtest"), bCqTest);
		return Out;
	}

	/** Build an error response payload embedding just a code+message. */
	static TSharedRef<FJsonObject> MakeInlineError(
		const FString& Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("error"), Code);
		Out->SetStringField(TEXT("message"), Message);
		return Out;
	}
}

void UeMcp::RegisterProjectHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpProjectHandlersPrivate;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("project.status"));
	Reg.DefaultTimeoutSeconds = 2.0;
	Reg.bMutating = false;
	Reg.Handler.BindLambda(
		[](const TSharedRef<FJsonObject>& /*Args*/, FUeMcpCancelToken& /*Cancel*/) -> TSharedRef<FJsonObject>
		{
			check(IsInGameThread());

			if (GEditor == nullptr)
			{
				return MakeInlineError(
					TEXT("EDITOR_NOT_READY"),
					TEXT("GEditor is null; project.status cannot read editor state"));
			}

			TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();

			// Engine / plugin / protocol identity.
			Data->SetStringField(TEXT("ue_version"), FEngineVersion::Current().ToString());
			Data->SetStringField(TEXT("plugin_version"), ResolvePluginVersion());
			Data->SetNumberField(TEXT("wire_protocol_version"), WireProtocolVersion);

			// Project identity.
			Data->SetStringField(TEXT("project_name"), FApp::GetProjectName());
			{
				FString ProjectDir = FPaths::ProjectDir();
				FPaths::NormalizeDirectoryName(ProjectDir);
				// Normalize all backslashes to forward slashes so the wire
				// payload is stable across Windows/Linux hosts.
				ProjectDir.ReplaceInline(TEXT("\\"), TEXT("/"));
				Data->SetStringField(TEXT("project_dir"), ProjectDir);
			}

			// Current level path. Guard against a null world (no map open).
			{
				FString LevelPath;
				const FWorldContext& WC = GEditor->GetEditorWorldContext(false);
				if (UWorld* World = WC.World())
				{
					LevelPath = World->GetPathName();
				}
				if (LevelPath.IsEmpty())
				{
					Data->SetField(TEXT("current_level"), MakeShared<FJsonValueNull>());
				}
				else
				{
					Data->SetStringField(TEXT("current_level"), LevelPath);
				}
			}

			// PIE active + mode.
			const bool bPie = GEditor->IsPlayingSessionInEditor();
			Data->SetBoolField(TEXT("pie_active"), bPie);
			Data->SetField(TEXT("pie_mode"), BuildPieMode());

			// Editor-busy sub-object.
			Data->SetObjectField(TEXT("editor_busy"), BuildEditorBusy());

			// Enabled well-known test plugins.
			Data->SetObjectField(TEXT("enabled_test_plugins"), BuildEnabledTestPlugins());

			return Data;
		});

	Dispatcher.RegisterTool(Reg);

	// ==================================================================
	// editor.set_unattended — toggle GIsRunningUnattendedScript (Wave D P5.1)
	//
	// Core/Private/Misc/MessageDialog.cpp:172 — when this flag is true
	// (or FApp::IsUnattended()), FMessageDialog::Open() returns the
	// caller-supplied default value (or a conservative Cancel/No
	// fallback) WITHOUT showing a modal window. Covers the big
	// code-driven pain: "Save changes to <level>?" prompts.
	//
	// LIMITS (not a silver bullet — documented for callers):
	//   * Slate-native modal *windows* (Restore Packages, SaveAs
	//     name-collision widget) are NOT FMessageDialog instances and
	//     this toggle does not suppress them. For those:
	//       - Restore Packages: iteration loop's autosave-wipe prevents
	//         the modal from ever firing.
	//       - SaveAs name collisions: use timestamped names + native
	//         blueprint.create (pre-checks existence).
	//   * Effect is global: any editor path that hits FMessageDialog
	//     while this is on will ALSO skip UI. Scope enabling to the
	//     duration of the test run; set enabled=false when done.
	//   * The flag persists across the editor session; we don't reset
	//     it at tool-call boundaries. Callers own the lifetime.
	//
	// Returns `prior` so a script can capture + restore.
	// ==================================================================
	{
		FUeMcpToolRegistration Reg2;
		Reg2.ToolName = FName(TEXT("editor.set_unattended"));
		Reg2.DefaultTimeoutSeconds = 2.0;
		Reg2.bMutating = true;
		Reg2.Handler.BindLambda(
			[](const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/) -> TSharedRef<FJsonObject>
			{
				check(IsInGameThread());

				bool bEnabled = true;
				Args->TryGetBoolField(TEXT("enabled"), bEnabled);

				const bool bPrior = GIsRunningUnattendedScript;
				GIsRunningUnattendedScript = bEnabled;

				TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
				Data->SetBoolField(TEXT("enabled"), bEnabled);
				Data->SetBoolField(TEXT("prior"),   bPrior);
				Data->SetBoolField(TEXT("app_is_unattended"), FApp::IsUnattended());

				// Rollback hint — replay set_unattended with the prior flag.
				// Only emit when the toggle actually flipped the global; a
				// no-op set carries no state to restore. The inverse tool is
				// itself (idempotent on `enabled`); the `enabled` natural key
				// is a plain bool, trivially stable. See
				// docs/handler-conventions.md §4 and the canonical pattern in
				// UeMcpReflectionHandlers.cpp (set_property → prior value).
				if (bPrior != bEnabled)
				{
					TSharedRef<FJsonObject> Rollback = MakeShared<FJsonObject>();
					Rollback->SetStringField(TEXT("tool"), TEXT("editor.set_unattended"));
					TSharedRef<FJsonObject> RollbackArgs = MakeShared<FJsonObject>();
					RollbackArgs->SetBoolField(TEXT("enabled"), bPrior);
					Rollback->SetObjectField(TEXT("args"), RollbackArgs);
					Data->SetObjectField(TEXT("rollback"), Rollback);
				}
				return Data;
			});

		Dispatcher.RegisterTool(Reg2);
	}
}
