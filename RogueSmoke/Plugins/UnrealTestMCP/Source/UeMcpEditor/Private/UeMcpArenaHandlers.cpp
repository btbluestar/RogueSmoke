// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.

#include "UeMcpArenaHandlers.h"

#include "Components/BrushComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/PlatformTime.h"
#include "Math/RandomStream.h"
#include "Misc/PackageName.h"
#include "NavMesh/NavMeshBoundsVolume.h"
#include "NavMesh/RecastNavMesh.h"
#include "NavigationSystem.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "UeMcpDispatcher.h"
#include "UeMcpWorldResolver.h"

namespace UeMcpArenaHandlersPrivate
{
	/** Default dispatcher timeout. Navmesh + save can take real time. */
	static constexpr double RebuildDefaultTimeoutSeconds = 120.0;

	/** Default arena asset path per v2 pivot §Fixture content pack. */
	static const TCHAR* DefaultArenaPath =
		TEXT("/Game/Plugins/UnrealTestMCP/TestFixtures/Maps/FTEST_Arena");

	/** Ground plane extent (cm). 2000×2000 in XY, ~50 in Z. */
	static constexpr float GroundHalfExtentXY = 1000.0f;
	static constexpr float GroundHalfThickness = 25.0f;
	static constexpr float GroundRefScaleUnit = 100.0f; // engine cube is 100u.

	/** Cover box layout. */
	static constexpr int32 MinCoverBoxes = 3;
	static constexpr int32 MaxCoverBoxes = 5;
	static constexpr float CoverBoxHeight = 200.0f;
	static constexpr float CoverMinSpacing = 400.0f;
	/** Keep boxes within the arena minus margin so they don't sit on the edge. */
	static constexpr float CoverEdgeMargin = 200.0f;

	/**
	 * Normalise an arena asset path. Accepts `/Game/Foo/Map`,
	 * `/Game/Foo/Map.Map`, `Map.umap`, etc. Strips `.umap` and any
	 * `.AssetName` suffix so `SaveMap` gets a package path.
	 * Returns `false` when the result doesn't start with `/Game/`.
	 */
	static bool NormaliseArenaPath(const FString& Raw, FString& OutPath)
	{
		OutPath = Raw.TrimStartAndEnd();
		if (OutPath.IsEmpty())
		{
			return false;
		}
		OutPath.ReplaceInline(TEXT("\\"), TEXT("/"));
		if (OutPath.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			OutPath = OutPath.LeftChop(5);
		}
		int32 DotIdx = INDEX_NONE;
		if (OutPath.FindLastChar(TEXT('.'), DotIdx))
		{
			int32 SlashIdx = INDEX_NONE;
			OutPath.FindLastChar(TEXT('/'), SlashIdx);
			if (SlashIdx < DotIdx)
			{
				OutPath = OutPath.Left(DotIdx);
			}
		}
		return OutPath.StartsWith(TEXT("/Game/"));
	}

	/**
	 * Place the ground plane using a `StaticMeshCube` scaled to the
	 * configured footprint. Returns the spawned actor (never null on
	 * success — asserts on spawn failure via `ensure`).
	 */
	static AStaticMeshActor* SpawnGround(UWorld* World)
	{
		// Engine cube is 100×100×100 units centred at origin. We scale to
		// the desired footprint; the actor's world transform carries the
		// scale, no geometry munging required.
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
			nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (CubeMesh == nullptr)
		{
			return nullptr;
		}

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Name = TEXT("FTEST_Arena_Ground");

		AStaticMeshActor* Ground = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(),
			FVector(0.0f, 0.0f, -GroundHalfThickness), FRotator::ZeroRotator,
			SpawnParams);
		if (!ensure(Ground != nullptr))
		{
			return nullptr;
		}

		Ground->SetActorLabel(TEXT("FTEST_Arena_Ground"));
		Ground->SetMobility(EComponentMobility::Static);

		// Scale so the cube becomes 2000×2000×50.
		const FVector Scale(
			(GroundHalfExtentXY * 2.0f) / GroundRefScaleUnit,
			(GroundHalfExtentXY * 2.0f) / GroundRefScaleUnit,
			(GroundHalfThickness * 2.0f) / GroundRefScaleUnit);
		Ground->SetActorScale3D(Scale);

		if (UStaticMeshComponent* SMC = Ground->GetStaticMeshComponent())
		{
			SMC->SetStaticMesh(CubeMesh);
		}
		return Ground;
	}

	/** Spawn the directional light per spec (pitch -50°, yaw 45°, 10 lux). */
	static ADirectionalLight* SpawnDirectionalLight(UWorld* World)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Name = TEXT("FTEST_Arena_SunLight");

		ADirectionalLight* Light = World->SpawnActor<ADirectionalLight>(
			ADirectionalLight::StaticClass(),
			FVector(0.0f, 0.0f, 1000.0f), FRotator(-50.0f, 45.0f, 0.0f),
			SpawnParams);
		if (!ensure(Light != nullptr))
		{
			return nullptr;
		}
		Light->SetActorLabel(TEXT("FTEST_Arena_SunLight"));
		// Directional-light mobility defaults to Static; Movable avoids
		// requiring a full lighting bake. Keeps the arena usable the
		// instant the save finishes.
		Light->SetMobility(EComponentMobility::Movable);
		if (UDirectionalLightComponent* LC = Cast<UDirectionalLightComponent>(
			Light->GetLightComponent()))
		{
			// Unitless directional intensity scales linearly with the
			// post-process auto-exposure. A value of 10 renders dark
			// under the UE 5.x default tonemapper when paired with
			// manual exposure bias 1.0 — what we used to have. Now
			// we use auto-exposure (see SpawnPostProcess) with a
			// clamped range, so a value on the order of 10 lumens is
			// visible. Keeping 10 as the concrete number here.
			LC->SetIntensity(10.0f);
		}
		return Light;
	}

	/** Spawn the skylight (intensity 1.0). */
	static ASkyLight* SpawnSkyLight(UWorld* World)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Name = TEXT("FTEST_Arena_SkyLight");

		ASkyLight* Sky = World->SpawnActor<ASkyLight>(
			ASkyLight::StaticClass(),
			FVector(0.0f, 0.0f, 200.0f), FRotator::ZeroRotator,
			SpawnParams);
		if (!ensure(Sky != nullptr))
		{
			return nullptr;
		}
		Sky->SetActorLabel(TEXT("FTEST_Arena_SkyLight"));
		if (USkyLightComponent* SLC = Sky->GetLightComponent())
		{
			// Mobility lives on the component, not the actor.
			SLC->SetMobility(EComponentMobility::Movable);
			SLC->SetIntensity(1.0f);
			// Movable skylight recaptures on demand; force one now.
			SLC->SetRealTimeCaptureEnabled(true);
		}
		return Sky;
	}

	/** Spawn BP_Sky_Sphere as a visual (best-effort; non-fatal on miss). */
	static AActor* SpawnSkySphere(UWorld* World, ADirectionalLight* Sun)
	{
		// StaticLoadClass needs an explicit base UClass* (LoadClass<T> is
		// a header-only wrapper that UE 5.7 trips on for some reason here).
		UClass* SkySphereCls = StaticLoadClass(
			AActor::StaticClass(), nullptr,
			TEXT("/Engine/EngineSky/BP_Sky_Sphere.BP_Sky_Sphere_C"));
		if (SkySphereCls == nullptr)
		{
			return nullptr;
		}
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Name = TEXT("FTEST_Arena_SkySphere");

		AActor* Sphere = World->SpawnActor<AActor>(
			SkySphereCls, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (Sphere == nullptr)
		{
			return nullptr;
		}
		Sphere->SetActorLabel(TEXT("FTEST_Arena_SkySphere"));
		// BP_Sky_Sphere exposes a `Directional Light Actor` variable via a
		// property; set it reflectively when present, ignore when not.
		if (Sun != nullptr)
		{
			if (FProperty* Prop = Sphere->GetClass()->FindPropertyByName(
				TEXT("Directional Light Actor")))
			{
				if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
				{
					ObjProp->SetObjectPropertyValue_InContainer(Sphere, Sun);
				}
			}
		}
		return Sphere;
	}

	/** Unbound post-process volume with default exposure compensation. */
	static APostProcessVolume* SpawnPostProcess(UWorld* World)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Name = TEXT("FTEST_Arena_PostProcess");

		APostProcessVolume* PP = World->SpawnActor<APostProcessVolume>(
			APostProcessVolume::StaticClass(),
			FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
		if (!ensure(PP != nullptr))
		{
			return nullptr;
		}
		PP->SetActorLabel(TEXT("FTEST_Arena_PostProcess"));
		PP->bUnbound = true;
		// Auto-exposure with clamped range.
		//
		// Previously set this to AEM_Manual with bias=1.0 to "kill
		// white-wash in screenshots." In practice that clipped the
		// arena black — the directional light's unitless intensity of
		// 10 doesn't match manual-exposure's lux-scale expectation, so
		// every surface under-exposed to near-zero.
		//
		// Histogram auto-exposure with a narrow clamp is the right
		// shape: the renderer adapts to scene luminance (so the arena
		// looks lit), and the clamp prevents the violent swings that
		// make screenshot-based tests flaky. Min=0.5/Max=1.5 keeps the
		// exposure essentially locked within a stop either side of
		// "balanced," which is plenty for a fixture arena.
		PP->Settings.bOverride_AutoExposureMethod = true;
		PP->Settings.AutoExposureMethod = EAutoExposureMethod::AEM_Histogram;
		PP->Settings.bOverride_AutoExposureBias = true;
		PP->Settings.AutoExposureBias = 1.0f;
		PP->Settings.bOverride_AutoExposureMinBrightness = true;
		PP->Settings.AutoExposureMinBrightness = 0.5f;
		PP->Settings.bOverride_AutoExposureMaxBrightness = true;
		PP->Settings.AutoExposureMaxBrightness = 1.5f;
		PP->Settings.bOverride_AutoExposureSpeedUp = true;
		PP->Settings.AutoExposureSpeedUp = 20.0f;  // adapt fast; screenshots run <1s after build
		PP->Settings.bOverride_AutoExposureSpeedDown = true;
		PP->Settings.AutoExposureSpeedDown = 20.0f;
		return PP;
	}

	/**
	 * Spawn a NavMeshBoundsVolume covering the ground. The NavMesh
	 * system wires in a RecastNavMesh automatically on the first
	 * registered bounds volume.
	 */
	static ANavMeshBoundsVolume* SpawnNavBounds(UWorld* World)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride =
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnParams.Name = TEXT("FTEST_Arena_NavBounds");

		ANavMeshBoundsVolume* NavBounds = World->SpawnActor<ANavMeshBoundsVolume>(
			ANavMeshBoundsVolume::StaticClass(),
			FVector(0.0f, 0.0f, 100.0f), FRotator::ZeroRotator, SpawnParams);
		if (!ensure(NavBounds != nullptr))
		{
			return nullptr;
		}
		NavBounds->SetActorLabel(TEXT("FTEST_Arena_NavBounds"));
		// AVolume uses its brush extent for bounds. Scale the brush so it
		// hugs the ground; taller than ground + cover so AI space fits.
		if (UBrushComponent* Brush = NavBounds->GetBrushComponent())
		{
			// Default brush is unit-scale; scale to match the ground plus
			// ~500cm of Z headroom.
			const FVector Scale(
				(GroundHalfExtentXY * 2.0f) / 200.0f,
				(GroundHalfExtentXY * 2.0f) / 200.0f,
				500.0f / 200.0f);
			NavBounds->SetActorScale3D(Scale);
		}
		return NavBounds;
	}

	/**
	 * Place 3-5 cover boxes at deterministic XY positions, rejection-
	 * sampled to keep ≥`CoverMinSpacing` apart. Seeded `FRandomStream`
	 * so a caller gets the same layout for the same seed.
	 */
	static int32 SpawnCoverBoxes(UWorld* World, int32 Seed)
	{
		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(
			nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (CubeMesh == nullptr)
		{
			return 0;
		}

		FRandomStream Rng(Seed);

		const int32 Count = MinCoverBoxes + Rng.RandRange(0, MaxCoverBoxes - MinCoverBoxes);
		const float MaxHalf = GroundHalfExtentXY - CoverEdgeMargin;

		TArray<FVector2D> Placed;
		Placed.Reserve(Count);
		int32 AttemptsLeft = 200;
		int32 Spawned = 0;

		while (Placed.Num() < Count && AttemptsLeft-- > 0)
		{
			FVector2D Candidate(
				Rng.FRandRange(-MaxHalf, MaxHalf),
				Rng.FRandRange(-MaxHalf, MaxHalf));
			bool bOk = true;
			for (const FVector2D& P : Placed)
			{
				if (FVector2D::Distance(P, Candidate) < CoverMinSpacing)
				{
					bOk = false;
					break;
				}
			}
			if (!bOk)
			{
				continue;
			}
			Placed.Add(Candidate);

			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride =
				ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			SpawnParams.Name = *FString::Printf(
				TEXT("FTEST_Arena_Cover_%d"), Spawned);

			AStaticMeshActor* Box = World->SpawnActor<AStaticMeshActor>(
				AStaticMeshActor::StaticClass(),
				FVector(Candidate.X, Candidate.Y, CoverBoxHeight * 0.5f),
				FRotator::ZeroRotator, SpawnParams);
			if (Box == nullptr)
			{
				continue;
			}
			Box->SetActorLabel(FString::Printf(TEXT("FTEST_Arena_Cover_%d"), Spawned));
			Box->SetMobility(EComponentMobility::Static);
			// Default cube is 100u in each axis. Scale to 200×200×200.
			Box->SetActorScale3D(FVector(2.0f, 2.0f, CoverBoxHeight / GroundRefScaleUnit));
			if (UStaticMeshComponent* SMC = Box->GetStaticMeshComponent())
			{
				SMC->SetStaticMesh(CubeMesh);
			}
			++Spawned;
		}
		return Spawned;
	}

	/**
	 * Override World Settings' default game mode to
	 * `FunctionalTestGameMode` so FunctionalTest-driven tests can
	 * execute inside the arena without a per-test-level override.
	 */
	static bool ApplyFunctionalTestGameMode(UWorld* World)
	{
		AWorldSettings* Settings = World->GetWorldSettings();
		if (Settings == nullptr)
		{
			return false;
		}
		UClass* GM = StaticLoadClass(
			AGameModeBase::StaticClass(), nullptr,
			TEXT("/Script/FunctionalTesting.FunctionalTestGameMode"));
		if (GM == nullptr)
		{
			return false;
		}
		Settings->DefaultGameMode = GM;
		return true;
	}

	/**
	 * `plugin.rebuild_test_arena` handler body. See header for the
	 * full contract. All work stays on the game thread — the
	 * dispatcher's executor guarantees that.
	 */
	static TSharedRef<FJsonObject> HandleRebuildTestArena(
		const TSharedRef<FJsonObject>& Args, FUeMcpCancelToken& /*Cancel*/)
	{
		check(IsInGameThread());

		if (GEditor == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("EDITOR_NOT_READY"),
				TEXT("GEditor is null; plugin.rebuild_test_arena cannot run"));
		}
		if (GEditor->IsPlayingSessionInEditor())
		{
			return UeMcp::MakeInlineError(
				TEXT("PIE_ACTIVE"),
				TEXT("Cannot rebuild the test arena while PIE is running; stop PIE first"));
		}

		// --- Args parse ---------------------------------------------------
		FString RawPath;
		if (!Args->TryGetStringField(TEXT("arena_path"), RawPath) || RawPath.IsEmpty())
		{
			RawPath = DefaultArenaPath;
		}
		FString ArenaPath;
		if (!NormaliseArenaPath(RawPath, ArenaPath))
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`arena_path` must resolve to a `/Game/...` asset; got '%s'"),
					*RawPath));
		}

		FString OnConflict = TEXT("overwrite");
		Args->TryGetStringField(TEXT("on_conflict"), OnConflict);
		OnConflict = OnConflict.ToLower();
		if (OnConflict != TEXT("overwrite") && OnConflict != TEXT("error"))
		{
			return UeMcp::MakeInlineError(
				TEXT("SCHEMA_ERROR"),
				FString::Printf(TEXT("`on_conflict` must be 'overwrite' or 'error'; got '%s'"),
					*OnConflict));
		}

		int32 Seed = 12345;
		{
			int32 SeedRaw = 0;
			if (Args->TryGetNumberField(TEXT("seed"), SeedRaw))
			{
				// Negative seeds are allowed by FRandomStream but we want a
				// positive, well-formed int for determinism. Reject <= 0.
				if (SeedRaw <= 0)
				{
					return UeMcp::MakeInlineError(
						TEXT("SCHEMA_ERROR"),
						FString::Printf(TEXT("`seed` must be a positive integer; got %d"),
							SeedRaw));
				}
				Seed = SeedRaw;
			}
		}

		// --- Conflict policy ---------------------------------------------
		if (OnConflict == TEXT("error") && FPackageName::DoesPackageExist(ArenaPath))
		{
			return UeMcp::MakeInlineError(
				TEXT("ALREADY_EXISTS"),
				FString::Printf(TEXT("Arena map already exists at '%s'; pass on_conflict='overwrite' to replace"),
					*ArenaPath));
		}

		const double StartSeconds = FPlatformTime::Seconds();

		// --- Fresh map ---------------------------------------------------
		UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(/*bSaveExistingMap*/ false);
		if (World == nullptr)
		{
			return UeMcp::MakeInlineError(
				TEXT("LEVEL_LOAD_FAILED"),
				TEXT("UEditorLoadingAndSavingUtils::NewBlankMap returned null"));
		}

		int32 ActorsPlaced = 0;

		// Ground + lights + sky + PP + nav bounds.
		if (SpawnGround(World) != nullptr) { ++ActorsPlaced; }
		ADirectionalLight* Sun = SpawnDirectionalLight(World);
		if (Sun != nullptr) { ++ActorsPlaced; }
		if (SpawnSkyLight(World) != nullptr) { ++ActorsPlaced; }
		if (SpawnSkySphere(World, Sun) != nullptr) { ++ActorsPlaced; }
		if (SpawnPostProcess(World) != nullptr) { ++ActorsPlaced; }
		if (SpawnNavBounds(World) != nullptr) { ++ActorsPlaced; }

		// Cover boxes.
		const int32 CoverCount = SpawnCoverBoxes(World, Seed);
		ActorsPlaced += CoverCount;

		// World Settings → FunctionalTestGameMode.
		const bool bGMApplied = ApplyFunctionalTestGameMode(World);

		// --- Save --------------------------------------------------------
		const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, ArenaPath);
		if (!bSaved)
		{
			return UeMcp::MakeInlineError(
				TEXT("LEVEL_SAVE_FAILED"),
				FString::Printf(TEXT("SaveMap returned false for '%s'"), *ArenaPath));
		}

		// --- Nav build ---------------------------------------------------
		// Trigger a navmesh build. In editor the build is async — the
		// bounds volume registration usually already kicked it off; this
		// ensures a deterministic rebuild after Save swaps the package.
		bool bNavBuiltRequested = false;
		if (UNavigationSystemV1* NavSys = UNavigationSystemV1::GetNavigationSystem(World))
		{
			NavSys->Build();
			bNavBuiltRequested = true;
		}

		// --- Lighting ----------------------------------------------------
		// Preview lighting is already live. A full `BuildLighting` call
		// takes minutes and blocks the game thread; skip it and rely on
		// Movable directional + RealTimeCapture skylight for the test
		// surface. Flag it so callers know what they got.
		const bool bLightingBuilt = false; // preview only, by design.

		const double EndSeconds = FPlatformTime::Seconds();

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetBoolField(TEXT("built"), true);
		Data->SetStringField(TEXT("arena_path"), ArenaPath);
		Data->SetNumberField(TEXT("actors_placed"), ActorsPlaced);
		Data->SetNumberField(TEXT("cover_box_count"), CoverCount);
		Data->SetBoolField(TEXT("navmesh_built"), bNavBuiltRequested);
		Data->SetBoolField(TEXT("lighting_built"), bLightingBuilt);
		Data->SetBoolField(TEXT("game_mode_applied"), bGMApplied);
		Data->SetNumberField(TEXT("seed"), Seed);
		Data->SetNumberField(TEXT("elapsed_ms"),
			FMath::RoundToDouble((EndSeconds - StartSeconds) * 1000.0));
		return Data;
	}
}

void UeMcp::RegisterArenaHandlers(FUeMcpDispatcher& Dispatcher)
{
	using namespace UeMcpArenaHandlersPrivate;

	FUeMcpToolRegistration Reg;
	Reg.ToolName = FName(TEXT("plugin.rebuild_test_arena"));
	Reg.DefaultTimeoutSeconds = RebuildDefaultTimeoutSeconds;
	Reg.bMutating = true;
	Reg.Handler = FUeMcpToolHandler::CreateStatic(&HandleRebuildTestArena);
	Dispatcher.RegisterTool(Reg);
}
