// Fill out your copyright notice in the Description page of Project Settings.
#include "AnimOffsetActor.h"
#include "Animation/AnimSequenceBase.h"
#include "Camera/CameraActor.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"

#include "ISequencerModule.h"
#include "ISequencer.h"
#include "Modules/ModuleManager.h"
#include "Editor.h"


// Sets default values
AAnimOffsetActor::AAnimOffsetActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Arrow = CreateDefaultSubobject<UArrowComponent>(TEXT("Arrow"));
	Arrow->SetupAttachment(Root);
	Arrow->ArrowSize = 2.0f;
	Arrow->bIsScreenSizeScaled = true;
}


ACameraActor* AAnimOffsetActor::ResolveCamera() const
{
	if (ACameraActor* Camera = TargetCamera.Get())
	{
		return Camera;
	}

	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ACameraActor> It(World); It; ++It)
		{
			return *It;
		}
	}
	return nullptr;
}


void AAnimOffsetActor::SetOriginToCurrent()
{
	OriginTransform = GetActorTransform();
	OriginTransform.SetScale3D(FVector::OneVector);
}


void AAnimOffsetActor::RecaptureCameraBase()
{
	if (ACameraActor* Camera = ResolveCamera())
	{
		CameraBaseTransform = Camera->GetActorTransform();
		bHasCameraBase = true;
	}
}


void AAnimOffsetActor::ApplyOffsetNow()
{
	ACameraActor* Camera = ResolveCamera();
	if (!Camera)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: no camera found in level."));
		return;
	}

	// Capture the camera's starting point once so repeated applies don't stack.
	if (!bHasCameraBase)
	{
		RecaptureCameraBase();
	}
	
	const FVector LocationOffset = GetActorLocation() - OriginTransform.GetLocation();
	const FVector NewLocation = CameraBaseTransform.GetLocation() + LocationOffset;

	FQuat NewRotation = CameraBaseTransform.GetRotation();
	if (bApplyRotationOffset)
	{
		// Rotation the actor has turned by since its origin, applied on top of the camera's base rotation.
		const FQuat RotationOffset = GetActorQuat() * OriginTransform.GetRotation().Inverse();
		NewRotation = RotationOffset * CameraBaseTransform.GetRotation();
	}

#if WITH_EDITOR
	Camera->Modify();
#endif
	Camera->SetActorLocationAndRotation(NewLocation, NewRotation);
}


void AAnimOffsetActor::PostActorCreated()
{
	Super::PostActorCreated();
	SetOriginToCurrent(); // wherever it is placed counts as zero offset
	RegisterSequencerHook();
}

void AAnimOffsetActor::PostLoad()
{
	Super::PostLoad();
	RegisterSequencerHook();
}

void AAnimOffsetActor::BeginDestroy()
{
	UnregisterSequencerHook();
	Super::BeginDestroy();
}

void AAnimOffsetActor::RegisterSequencerHook()
{
	if (HasAnyFlags(RF_ClassDefaultObject) || SequencerCreatedHandle.IsValid())
	{
		return;
	}
	ISequencerModule* SequencerModulePtr = FModuleManager::Get().GetModulePtr<ISequencerModule>("Sequencer");
	if (SequencerModulePtr)
	{
		SequencerCreatedHandle = SequencerModulePtr->RegisterOnSequencerCreated(
			FOnSequencerCreated::FDelegate::CreateUObject(this, &AAnimOffsetActor::OnSequencerCreated));
	}
}


void AAnimOffsetActor::UnregisterSequencerHook()
{
	if (SequencerCreatedHandle.IsValid())
	{
		ISequencerModule* SequencerModulePtr = FModuleManager::Get().GetModulePtr<ISequencerModule>("Sequencer");
		if (SequencerModulePtr)
		{
			SequencerModulePtr->UnregisterOnSequencerCreated(SequencerCreatedHandle);
		}
		SequencerCreatedHandle.Reset();
	}
}

void AAnimOffsetActor::OnSequencerCreated(TSharedRef<ISequencer> InSequencer)
{
	if (!bMoveCameraOnSequenceOpen || !GetWorld() || GetWorld() != GEditor->GetEditorWorldContext().World())
	{
		return;
	}

	ApplyOffsetNow();
}