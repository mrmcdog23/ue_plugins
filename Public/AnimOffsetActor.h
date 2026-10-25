// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AnimOffsetActor.generated.h"

class ACameraActor;
class UArrowComponent;
class ISequencer;

/**
 * Place this actor in the level. Whenever a Sequencer is opened, any spawned cameras
 * in the focused sequence are parented under this actor, so moving/rotating the actor
 * moves the whole camera animation.
 */
UCLASS()
class TESTING_API AAnimOffsetActor : public AActor
{
	GENERATED_BODY()

public:
	AAnimOffsetActor();

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
