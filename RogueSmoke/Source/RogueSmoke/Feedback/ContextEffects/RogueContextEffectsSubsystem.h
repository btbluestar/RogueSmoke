// Copyright Epic Games, Inc. All Rights Reserved.
// Ported from Lyra's ContextEffects (D-0022 migration); mechanical rename Lyra->Rogue.

#pragma once

#include "Engine/DeveloperSettings.h"
#include "GameplayTagContainer.h"
#include "Subsystems/WorldSubsystem.h"

#include "RogueContextEffectsSubsystem.generated.h"

#define UE_API ROGUESMOKE_API

enum EPhysicalSurface : int;

class AActor;
class UAudioComponent;
class URogueContextEffectsLibrary;
class UNiagaraComponent;
class USceneComponent;
struct FFrame;
struct FGameplayTag;
struct FGameplayTagContainer;

/**
 *
 */
UCLASS(MinimalAPI, config = Game, defaultconfig, meta = (DisplayName = "RogueContextEffects"))
class URogueContextEffectsSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	//
	UPROPERTY(config, EditAnywhere)
	TMap<TEnumAsByte<EPhysicalSurface>, FGameplayTag> SurfaceTypeToContextMap;
};

/**
 *
 */
UCLASS(MinimalAPI)
class URogueContextEffectsSet : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(Transient)
	TSet<TObjectPtr<URogueContextEffectsLibrary>> RogueContextEffectsLibraries;
};


/**
 *
 */
UCLASS(MinimalAPI)
class URogueContextEffectsSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	/** */
	UFUNCTION(BlueprintCallable, Category = "ContextEffects")
	UE_API void SpawnContextEffects(
		const AActor* SpawningActor
		, USceneComponent* AttachToComponent
		, const FName AttachPoint
		, const FVector LocationOffset
		, const FRotator RotationOffset
		, FGameplayTag Effect
		, FGameplayTagContainer Contexts
		, TArray<UAudioComponent*>& AudioOut
		, TArray<UNiagaraComponent*>& NiagaraOut
		, FVector VFXScale = FVector(1)
		, float AudioVolume = 1
		, float AudioPitch = 1);

	/** */
	UFUNCTION(BlueprintCallable, Category = "ContextEffects")
	UE_API bool GetContextFromSurfaceType(TEnumAsByte<EPhysicalSurface> PhysicalSurface, FGameplayTag& Context);

	/** */
	UFUNCTION(BlueprintCallable, Category = "ContextEffects")
	UE_API void LoadAndAddContextEffectsLibraries(AActor* OwningActor, TSet<TSoftObjectPtr<URogueContextEffectsLibrary>> ContextEffectsLibraries);

	/** */
	UFUNCTION(BlueprintCallable, Category = "ContextEffects")
	UE_API void UnloadAndRemoveContextEffectsLibraries(AActor* OwningActor);

private:

	UPROPERTY(Transient)
	TMap<TObjectPtr<AActor>, TObjectPtr<URogueContextEffectsSet>> ActiveActorEffectsMap;

};

#undef UE_API
