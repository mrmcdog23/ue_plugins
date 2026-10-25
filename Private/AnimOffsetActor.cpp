// Fill out your copyright notice in the Description page of Project Settings.
#include "AnimOffsetActor.h"
#include "Camera/CameraActor.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"

#if WITH_EDITOR
#include "Containers/Ticker.h"
#include "Editor.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSpawnable.h"
#endif


AAnimOffsetActor::AAnimOffsetActor()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Arrow = CreateDefaultSubobject<UArrowComponent>(TEXT("Arrow"));
	Arrow->SetupAttachment(Root);
	Arrow->ArrowSize = 2.0f;
	Arrow->bIsScreenSizeScaled = true;
}


#if WITH_EDITOR

void AAnimOffsetActor::PostActorCreated()
{
	Super::PostActorCreated();
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

	// LoadModulePtr (not GetModulePtr): Sequencer is often not loaded yet during PostLoad.
	if (ISequencerModule* SequencerModulePtr = FModuleManager::LoadModulePtr<ISequencerModule>("Sequencer"))
	{
		SequencerCreatedHandle = SequencerModulePtr->RegisterOnSequencerCreated(
			FOnSequencerCreated::FDelegate::CreateUObject(this, &AAnimOffsetActor::OnSequencerCreated));
	}
}

void AAnimOffsetActor::UnregisterSequencerHook()
{
	if (SequencerCreatedHandle.IsValid())
	{
		if (ISequencerModule* SequencerModulePtr = FModuleManager::Get().GetModulePtr<ISequencerModule>("Sequencer"))
		{
			SequencerModulePtr->UnregisterOnSequencerCreated(SequencerCreatedHandle);
		}
		SequencerCreatedHandle.Reset();
	}
}

void AAnimOffsetActor::OnSequencerCreated(TSharedRef<ISequencer> InSequencer)
{
	if (!bMoveCameraOnSequenceOpen || !GEditor || GetWorld() != GEditor->GetEditorWorldContext().World())
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: sequencer opened, watching for spawned cameras."));

	TWeakObjectPtr<AAnimOffsetActor> WeakThis(this);
	TWeakPtr<ISequencer> WeakSequencer = InSequencer;

	// Poll while this Sequencer is open, so cameras that get respawned are re-parented.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, WeakSequencer](float)
		{
			AAnimOffsetActor* Self = WeakThis.Get();
			TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin();
			if (!Self || !Sequencer.IsValid())
			{
				return false; // actor or Sequencer is gone, stop ticking
			}

			Self->AttachSpawnedCameras(*Sequencer);
			return true; // keep running
		}), 0.1f);
}


void AAnimOffsetActor::AttachSpawnedCameras(ISequencer& InSequencer)
{
	UE_LOG(LogTemp, Warning, TEXT("AttachSpawnedCameras: attached level camera"));
	UMovieSceneSequence* Sequence = InSequencer.GetFocusedMovieSceneSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		UE_LOG(LogTemp, Warning, TEXT("No movie scene found."));
		return;
	}

	UE_LOG(LogTemp, Verbose, TEXT("CameraAnchor: '%s' spawnables=%d possessables=%d bindings=%d"),
		*Sequence->GetName(), MovieScene->GetSpawnableCount(),
		MovieScene->GetPossessableCount(), MovieScene->GetBindings().Num());

	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		const FGuid BindingId = Binding.GetObjectGuid();

		for (TWeakObjectPtr<> Object : InSequencer.FindBoundObjects(BindingId, InSequencer.GetFocusedTemplateID()))
		{
			UObject* Bound = Object.Get();
			if (!Bound)
			{
				continue;
			}

			ACameraActor* Camera = Cast<ACameraActor>(Bound);
			if (!Camera)
			{
				continue; // not a camera actor, ignore silently
			}

			if (Camera->GetAttachParentActor() != this)
			{
				Camera->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
				UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: attached '%s' (%s)"),
					*Camera->GetName(), *Camera->GetClass()->GetName());
			}
		}
	}
}
#endif // WITH_EDITOR
