// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AnimOffsetActor.generated.h"

class ACameraActor;
class UArrowComponent;


UCLASS()
class TESTING_API AAnimOffsetActor : public AActor
{
	GENERATED_BODY()
	
public:	
	// Sets default values for this actor's properties
	AAnimOffsetActor();

	/** Camera to offset. If empty, the first CameraActor found in the level is used. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor")
	TSoftObjectPtr<ACameraActor> TargetCamera;

	/** Apply the offset automatically whenever a Level Sequence is opened. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor")
	bool bMoveCameraOnSequenceOpen = true;

	/** Also apply the actor's rotation offset (not just location). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor")
	bool bApplyRotationOffset = true;

	/** Actor transform that counts as "zero offset". Set automatically on placement. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor|State")
	FTransform OriginTransform;

	/** Camera transform that the offset is added to. Captured the first time it is applied. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor|State")
	FTransform CameraBaseTransform;

	UPROPERTY(VisibleAnywhere, Category = "Camera Anchor|State")
	bool bHasCameraBase = false;

	/** Apply the offset to the camera now. */
	UFUNCTION(CallInEditor, Category = "Camera Anchor")
	void ApplyOffsetNow();

	/** Treat this actor's current transform as zero offset. */
	UFUNCTION(CallInEditor, Category = "Camera Anchor")
	void SetOriginToCurrent();

	/** Treat the camera's current transform as its base (use after you reposition the camera yourself). */
	UFUNCTION(CallInEditor, Category = "Camera Anchor")
	void RecaptureCameraBase();

protected:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UArrowComponent> Arrow;

	ACameraActor* ResolveCamera() const;


#if WITH_EDITOR
	virtual void PostActorCreated() override;
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;

private:
	void RegisterSequencerHook();
	void UnregisterSequencerHook();
	void OnSequencerCreated(TSharedRef<class ISequencer> InSequencer);

	FDelegateHandle SequencerCreatedHandle;
#endif
};

