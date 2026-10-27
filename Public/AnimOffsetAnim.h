// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AnimOffsetAnim.generated.h"

class ACameraActor;
class UArrowComponent;
class ISequencer;

UCLASS()
class CONTROLCHAOS_API AAnimOffsetAnim : public AActor
{
	GENERATED_BODY()
	
public:	
	virtual void Tick(float DeltaSeconds) override;
	// Sets default values for this actor's properties
	AAnimOffsetAnim();

	/** Optional level camera (not spawned by the sequence) to parent under this actor as well. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor")
	TSoftObjectPtr<ACameraActor> TargetCamera;

	/** Parent spawned cameras under this actor whenever a Sequencer is opened. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor")
	bool bMoveCameraOnSequenceOpen = true;

protected:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UArrowComponent> Arrow;

	/** Runtime (PIE / MRQ / game): parent sequence-spawned cameras under this actor. */
	void AttachRuntimeCameras();

#if WITH_EDITOR
virtual void PostActorCreated() override;
virtual void PostLoad() override;
virtual void BeginDestroy() override;


private:
	void RegisterSequencerHook();
	void UnregisterSequencerHook();
	void OnSequencerCreated(TSharedRef<ISequencer> InSequencer);

	/** Parents any spawned cameras (and TargetCamera, if set) under this actor. */
	void AttachSpawnedCameras(ISequencer& InSequencer);
	FDelegateHandle SequencerCreatedHandle;
#endif
};
