// Copyright Epic Games, Inc. All Rights Reserved.
// Ported from Lyra's ContextEffects (D-0022 migration); mechanical rename Lyra->Rogue.

#pragma once

#include "Components/ActorComponent.h"
#include "Feedback/ContextEffects/RogueContextEffectsInterface.h"

#include "RogueContextEffectComponent.generated.h"

#define UE_API ROGUESMOKE_API

namespace EEndPlayReason { enum Type : int; }

class UAnimSequenceBase;
class UAudioComponent;
class URogueContextEffectsLibrary;
class UNiagaraComponent;
class UObject;
class USceneComponent;
struct FFrame;
struct FHitResult;

UCLASS(MinimalAPI,  ClassGroup=(Custom), hidecategories = (Variable, Tags, ComponentTick, ComponentReplication, Activation, Cooking, AssetUserData, Collision), CollapseCategories, meta=(BlueprintSpawnableComponent) )
class URogueContextEffectComponent : public UActorComponent, public IRogueContextEffectsInterface
{
	GENERATED_BODY()

public:
	// Sets default values for this component's properties
	UE_API URogueContextEffectComponent();

protected:
	// Called when the game starts
	UE_API virtual void BeginPlay() override;

	// Called when the game ends
	UE_API virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

public:
	// AnimMotionEffect Implementation
	UFUNCTION(BlueprintCallable)
	UE_API virtual void AnimMotionEffect_Implementation(const FName Bone, const FGameplayTag MotionEffect, USceneComponent* StaticMeshComponent,
		const FVector LocationOffset, const FRotator RotationOffset, const UAnimSequenceBase* AnimationSequence,
		const bool bHitSuccess, const FHitResult HitResult, FGameplayTagContainer Contexts,
		FVector VFXScale = FVector(1), float AudioVolume = 1, float AudioPitch = 1) override;

	// Auto-Convert Physical Surface from Trace Result to Context
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	bool bConvertPhysicalSurfaceToContext = true;

	// Default Contexts
	UPROPERTY(EditAnywhere)
	FGameplayTagContainer DefaultEffectContexts;

	// Default Libraries for this Actor
	UPROPERTY(EditAnywhere)
	TSet<TSoftObjectPtr<URogueContextEffectsLibrary>> DefaultContextEffectsLibraries;

	UFUNCTION(BlueprintCallable)
	UE_API void UpdateEffectContexts(FGameplayTagContainer NewEffectContexts);

	UFUNCTION(BlueprintCallable)
	UE_API void UpdateLibraries(TSet<TSoftObjectPtr<URogueContextEffectsLibrary>> NewContextEffectsLibraries);

private:
	UPROPERTY(Transient)
	FGameplayTagContainer CurrentContexts;

	UPROPERTY(Transient)
	TSet<TSoftObjectPtr<URogueContextEffectsLibrary>> CurrentContextEffectsLibraries;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> ActiveAudioComponents;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UNiagaraComponent>> ActiveNiagaraComponents;
};

#undef UE_API
