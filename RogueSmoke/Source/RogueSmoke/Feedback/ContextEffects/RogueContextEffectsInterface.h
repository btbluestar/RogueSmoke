// Copyright Epic Games, Inc. All Rights Reserved.
// Ported from Lyra's ContextEffects (D-0022 migration); mechanical rename Lyra->Rogue.
// NOTE: C++ UInterface — implemented/bound in C++ only; AngelScript never touches this directly.

#pragma once

#include "Engine/HitResult.h"
#include "GameplayTagContainer.h"
#include "UObject/Interface.h"

#include "RogueContextEffectsInterface.generated.h"

#define UE_API ROGUESMOKE_API

class UAnimSequenceBase;
class UObject;
class USceneComponent;
struct FFrame;

/**
 *
 */
UENUM()
enum EEffectsContextMatchType: int
{
	ExactMatch,
	BestMatch
};

/**
 *
 */
 UINTERFACE(MinimalAPI, Blueprintable)
 class URogueContextEffectsInterface : public UInterface
 {
	 GENERATED_BODY()

 };

 class IRogueContextEffectsInterface : public IInterface
 {
	 GENERATED_BODY()

 public:

	/** */
 	UFUNCTION(BlueprintCallable, BlueprintNativeEvent)
	UE_API void AnimMotionEffect(const FName Bone
		, const FGameplayTag MotionEffect
		, USceneComponent* StaticMeshComponent
		, const FVector LocationOffset
		, const FRotator RotationOffset
		, const UAnimSequenceBase* AnimationSequence
		, const bool bHitSuccess
		, const FHitResult HitResult
		, FGameplayTagContainer Contexts
		, FVector VFXScale = FVector(1)
		, float AudioVolume = 1
		, float AudioPitch = 1);
 };

#undef UE_API
