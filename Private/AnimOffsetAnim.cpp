// Fill out your copyright notice in the Description page of Project Settings.


#include "AnimOffsetAnim.h"
#include "Camera/CameraActor.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"   // TActorIterator

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


// Sets default values
AAnimOffsetAnim::AAnimOffsetAnim()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork; // after Sequencer has spawned/evaluated

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Arrow = CreateDefaultSubobject<UArrowComponent>(TEXT("Arrow"));
	Arrow->SetupAttachment(Root);
	Arrow->ArrowSize = 2.0f;
	Arrow->bIsScreenSizeScaled = true;

}


void AAnimOffsetAnim::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// Editor viewport worlds don't tick this (ShouldTickIfViewportsOnly is false),
	// so this only runs in PIE / MRQ / game, where the Sequencer editor hook never fires.
	if (bMoveCameraOnSequenceOpen)
	{
		AttachRuntimeCameras();
	}
}


void AAnimOffsetAnim::AttachRuntimeCameras()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return;
	}

	// Explicit level camera
	if (ACameraActor* Target = TargetCamera.Get())
	{
		if (Target->GetAttachParentActor() != this)
		{
			Target->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
		}
	}

	for (TActorIterator<ACameraActor> It(World); It; ++It)
	{
		ACameraActor* Camera = *It;

		// Only sequence-spawned cameras. Placed level cameras are net-startup actors.
		if (Camera->IsNetStartupActor() || Camera->GetAttachParentActor() == this)
		{
			continue;
		}

		// KeepRelative: Sequencer writes the keyed values as the relative transform,
		// which is exactly how they were authored while attached in the editor.
		Camera->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
	}
}


#if WITH_EDITOR
void AAnimOffsetAnim::PostActorCreated()
{
	Super::PostActorCreated();
	RegisterSequencerHook();
}

void AAnimOffsetAnim::PostLoad()
{
	Super::PostLoad();
	RegisterSequencerHook();
}

void AAnimOffsetAnim::BeginDestroy()
{
	//UnregisterSequencerHook();
	Super::BeginDestroy();
}
#endif // WITH_EDITOR

void AAnimOffsetAnim::OnSequencerCreated(TSharedRef<ISequencer> InSequencer)
{
	if (!bMoveCameraOnSequenceOpen || !GEditor || GetWorld() != GEditor->GetEditorWorldContext().World())
	{
		return;
	}
	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: sequencer opened, watching for spawned cameras."));

	TWeakObjectPtr<AAnimOffsetAnim> WeakThis(this);
	TWeakPtr<ISequencer> WeakSequencer = InSequencer;

	// Poll while this Sequencer is open, so cameras that get respawned are re-parented.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, WeakSequencer](float)
		{
			AAnimOffsetAnim* Self = WeakThis.Get();
			TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin();
			if (!Self || !Sequencer.IsValid())
			{
				return false; // actor or Sequencer is gone, stop ticking
			}

			Self->AttachSpawnedCameras(*Sequencer);
			return true; // keep running
		}), 0.1f);
}


void AAnimOffsetAnim::AttachSpawnedCameras(ISequencer& InSequencer)
{
	UMovieSceneSequence* Sequence = InSequencer.GetFocusedMovieSceneSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		UE_LOG(LogTemp, Warning, TEXT("No movie scene found."));
		return;
	}

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


void AAnimOffsetAnim::RegisterSequencerHook()
{
	if (HasAnyFlags(RF_ClassDefaultObject) || SequencerCreatedHandle.IsValid())
	{
		return;
	}

	// LoadModulePtr (not GetModulePtr): Sequencer is often not loaded yet during PostLoad.
	if (ISequencerModule* SequencerModulePtr = FModuleManager::LoadModulePtr<ISequencerModule>("Sequencer"))
	{
		SequencerCreatedHandle = SequencerModulePtr->RegisterOnSequencerCreated(
			FOnSequencerCreated::FDelegate::CreateUObject(this, &AAnimOffsetAnim::OnSequencerCreated));
	}
}


void AAnimOffsetAnim::UnregisterSequencerHook()
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