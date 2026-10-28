// Fill out your copyright notice in the Description page of Project Settings.


#include "AnimOffsetAnim.h"
#include "Camera/CameraActor.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"   // TActorIterator

#if WITH_EDITOR
#include "Containers/Ticker.h"
#include "Curves/RealCurve.h"
#include "Editor.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSpawnable.h"
#include "ScopedTransaction.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
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

		// Only sequence-spawned cameras. Placed level cameras are net-startup actors
		// (their parent, if any, was saved with the level and is already in place).
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

void AAnimOffsetAnim::OnSequencerCreated(TSharedRef<ISequencer> InSequencer)
{
	// Note: not gated on bMoveCameraOnSequenceOpen. That flag only controls camera auto-parenting
	// (checked live in TrackBoundActors), so tracking of other actors keeps working when it is off.
	if (!GEditor || GetWorld() != GEditor->GetEditorWorldContext().World())
	{
		return;
	}
	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: sequencer opened, watching bound actors."));

	TWeakObjectPtr<AAnimOffsetAnim> WeakThis(this);
	TWeakPtr<ISequencer> WeakSequencer = InSequencer;

	// Poll while this Sequencer is open, so cameras that get respawned are re-parented
	// and actors the artist parents/unparents are noticed.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis, WeakSequencer](float)
		{
			AAnimOffsetAnim* Self = WeakThis.Get();
			TSharedPtr<ISequencer> Sequencer = WeakSequencer.Pin();
			if (!Self)
			{
				return false; // actor is gone, stop ticking
			}
			if (!Sequencer.IsValid())
			{
				// Sequencer closed: forget per-session state so reopening starts clean.
				Self->ParentedByBinding.Reset();
				Self->OptedOutBindings.Reset();
				return false;
			}

			Self->TrackBoundActors(*Sequencer);
			return true; // keep running
		}), 0.1f);
}


void AAnimOffsetAnim::TrackBoundActors(ISequencer& InSequencer)
{
	// A bake/prompt is in progress (the modal dialog pumps the ticker), don't re-enter.
	if (bPromptOpen)
	{
		return;
	}

	// Nothing to do if both features are off.
	if (!bMoveCameraOnSequenceOpen && !bPromptToBakeOnUnparent)
	{
		return;
	}

	UMovieSceneSequence* Sequence = InSequencer.GetFocusedMovieSceneSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		UE_LOG(LogTemp, Warning, TEXT("No movie scene found."));
		return;
	}

	// Bindings whose actor the artist just unparented. Handled after the loop,
	// because the popup / bake scrubs the Sequencer and must not run mid-iteration.
	struct FUnparented
	{
		FGuid BindingId;
		TWeakObjectPtr<AActor> Actor;
		FTransform LocalToAnchor;
	};
	TArray<FUnparented> JustUnparented;

	for (const FMovieSceneBinding& Binding : MovieScene->GetBindings())
	{
		const FGuid BindingId = Binding.GetObjectGuid();

		// The artist already took this actor out from under us: leave it alone.
		if (OptedOutBindings.Contains(BindingId))
		{
			continue;
		}

		// FindBoundObjects resolves both spawnables and possessables (placed level actors).
		for (TWeakObjectPtr<> Object : InSequencer.FindBoundObjects(BindingId, InSequencer.GetFocusedTemplateID()))
		{
			UObject* Bound = Object.Get();
			if (!Bound)
			{
				continue;
			}

			// Any actor, not just cameras. Components and other non-actor bindings are ignored silently.
			AActor* Actor = Cast<AActor>(Bound);
			if (!Actor)
			{
				continue;
			}

			// Never track ourselves, or one of our own ancestors (attaching that would be a cycle).
			if (Actor == this || IsAttachedTo(Actor))
			{
				continue;
			}

			AActor* CurrentParent = Actor->GetAttachParentActor();
			if (CurrentParent == this)
			{
				// Covers cameras we attached and anything the artist parented by hand.
				// Keep refreshing the transform relative to us, so we know where it sat if it gets unparented.
				FTrackedActor& Tracked = ParentedByBinding.FindOrAdd(BindingId);
				Tracked.Actor = Actor;
				Tracked.LocalToAnchor = Actor->GetActorTransform().GetRelativeTransform(GetActorTransform());
				continue;
			}

			// If it is the very same actor we saw under us earlier, and it is no longer under us,
			// the artist unparented it. (A respawned spawnable is a different object, so it falls through.)
			const FTrackedActor* Previous = ParentedByBinding.Find(BindingId);
			if (Previous && Previous->Actor.Get() == Actor)
			{
				const FTransform LocalToAnchor = Previous->LocalToAnchor; // copy before Remove()
				OptedOutBindings.Add(BindingId);
				ParentedByBinding.Remove(BindingId);

				// Only offer a fix-up if it is now free-floating. If they dragged it under some other actor, just respect that.
				if (!CurrentParent)
				{
					JustUnparented.Add({ BindingId, Actor, LocalToAnchor });
				}
				continue;
			}

			// New or respawned actor that we have never seen under us.
			// Only cameras are parented automatically; every other actor is only
			// tracked once the artist parents it themselves (handled above).
			if (bMoveCameraOnSequenceOpen && Actor->IsA<ACameraActor>())
			{
				Actor->Modify(); // a possessed camera is a level actor: this marks the level dirty
				Actor->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);

				FTrackedActor& Tracked = ParentedByBinding.FindOrAdd(BindingId);
				Tracked.Actor = Actor;
				Tracked.LocalToAnchor = Actor->GetActorTransform().GetRelativeTransform(GetActorTransform());

				UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: attached '%s' (%s)"),
					*Actor->GetName(), *Actor->GetClass()->GetName());
			}
		}
	}

	for (const FUnparented& Item : JustUnparented)
	{
		if (AActor* Actor = Item.Actor.Get())
		{
			HandleActorUnparented(InSequencer, Item.BindingId, Actor, Item.LocalToAnchor);
		}
	}
}


bool AAnimOffsetAnim::IsBindingAnimated(UMovieScene* MovieScene, const FGuid& BindingId) const
{
	UMovieScene3DTransformTrack* Track = MovieScene ? MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingId) : nullptr;
	if (!Track)
	{
		return false;
	}

	for (UMovieSceneSection* Section : Track->GetAllSections())
	{
		if (!Section)
		{
			continue;
		}
		for (FMovieSceneDoubleChannel* Channel : Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>())
		{
			if (Channel && Channel->GetNumKeys() > 0)
			{
				return true;
			}
		}
	}
	return false;
}


void AAnimOffsetAnim::HandleActorUnparented(ISequencer& InSequencer, const FGuid& BindingId, AActor* Actor, const FTransform& LocalToAnchor)
{
	if (!bPromptToBakeOnUnparent || !Actor)
	{
		return;
	}

	UMovieSceneSequence* Sequence = InSequencer.GetFocusedMovieSceneSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	const bool bAnimated = IsBindingAnimated(MovieScene, BindingId);

	bPromptOpen = true;

	if (bAnimated)
	{
		const FText Message = FText::Format(
			NSLOCTEXT("CameraAnchor", "BakePrompt",
				"'{0}' was unparented from '{1}'.\n\n"
				"Do you want to bake its animation as it looks under '{1}'?\n\n"
				"Yes: re-keys it in world space every frame, so it keeps moving exactly as it did while parented.\n"
				"No: leaves the existing keys untouched (it will jump)."),
			FText::FromString(Actor->GetActorLabel()),
			FText::FromString(GetActorLabel()));
		const FText Title = NSLOCTEXT("CameraAnchor", "BakeTitle", "Bake Animation?");

		// NOTE: on UE 5.0-5.2 the title parameter is a pointer: pass &Title instead.
		if (FMessageDialog::Open(EAppMsgType::YesNo, Message, Title) == EAppReturnType::Yes)
		{
			BakeActorToWorld(InSequencer, BindingId);
		}
	}
	else
	{
		const FText Message = FText::Format(
			NSLOCTEXT("CameraAnchor", "KeepPrompt",
				"'{0}' was unparented from '{1}' and has no transform animation.\n\n"
				"Do you want to keep the transform it had under '{1}'?\n\n"
				"Yes: moves it to where it was sitting while parented (world space, using '{1}' as it is now).\n"
				"No: leaves it exactly where it is now."),
			FText::FromString(Actor->GetActorLabel()),
			FText::FromString(GetActorLabel()));
		const FText Title = NSLOCTEXT("CameraAnchor", "KeepTitle", "Keep Transform?");

		if (FMessageDialog::Open(EAppMsgType::YesNo, Message, Title) == EAppReturnType::Yes)
		{
			// Child-in-anchor-space * anchor-in-world = where it was sitting while parented.
			const FTransform DesiredWorld = LocalToAnchor * GetActorTransform();

			const FScopedTransaction Transaction(NSLOCTEXT("CameraAnchor", "KeepTransaction", "Keep Transform After Unparent"));
			Actor->Modify();
			Actor->SetActorTransform(DesiredWorld, false, nullptr, ETeleportType::TeleportPhysics);
		}
	}

	bPromptOpen = false;

	// A camera is no longer meant to live under this actor, so stop re-parenting cameras at runtime too.
	// (After a bake / keep the transform is world space, so re-parenting in PIE/MRQ would apply the offset twice.)
	// Other actors are never auto-parented at runtime, so there is nothing to switch off for them.
	if (Actor->IsA<ACameraActor>())
	{
		Modify();
		bMoveCameraOnSequenceOpen = false;
	}
}


void AAnimOffsetAnim::BakeActorToWorld(ISequencer& InSequencer, const FGuid& BindingId)
{
	UMovieSceneSequence* Sequence = InSequencer.GetFocusedMovieSceneSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		return;
	}

	UMovieScene3DTransformTrack* Track = MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingId);
	UMovieScene3DTransformSection* Section = (Track && Track->GetAllSections().Num() > 0)
		? Cast<UMovieScene3DTransformSection>(Track->GetAllSections()[0])
		: nullptr;
	if (!Section)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: no transform track to bake."));
		return;
	}

	// Transform section channels: 0-2 location XYZ, 3-5 rotation (Roll, Pitch, Yaw), 6-8 scale XYZ.
	TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	if (Channels.Num() < 9)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: unexpected transform channel layout, bake aborted."));
		return;
	}

	// Finds the live actor for this binding and makes sure it is under this actor,
	// so Sequencer's keyed values are applied relative to us while we sample.
	auto GetAttachedActor = [this, &InSequencer, &BindingId]() -> AActor*
		{
			for (TWeakObjectPtr<> Object : InSequencer.FindBoundObjects(BindingId, InSequencer.GetFocusedTemplateID()))
			{
				if (AActor* Actor = Cast<AActor>(Object.Get()))
				{
					if (Actor != this && Actor->GetAttachParentActor() != this)
					{
						Actor->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
					}
					return Actor;
				}
			}
			return nullptr;
		};

	const FFrameRate TickRes = MovieScene->GetTickResolution();
	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const TRange<FFrameNumber> Range = MovieScene->GetPlaybackRange();
	const int32 StartFrame = FFrameRate::TransformTime(FFrameTime(Range.GetLowerBoundValue()), TickRes, DisplayRate).CeilToFrame().Value;
	const int32 EndFrame = FFrameRate::TransformTime(FFrameTime(Range.GetUpperBoundValue()), TickRes, DisplayRate).FloorToFrame().Value;

	const FFrameTime OriginalTime = InSequencer.GetLocalTime().Time;

	// Re-parent before sampling so the very first evaluated frame is already applied relative to us.
	GetAttachedActor();

	// 1) Sample: scrub every frame with the actor parented, record its WORLD transform.
	TArray<FFrameNumber> SampleTimes;
	TArray<FTransform> SampleTransforms;

	for (int32 Frame = StartFrame; Frame <= EndFrame; ++Frame)
	{
		const FFrameTime Time = FFrameRate::TransformTime(FFrameTime(FFrameNumber(Frame)), DisplayRate, TickRes);
		InSequencer.SetLocalTime(Time);
		InSequencer.ForceEvaluate();

		if (AActor* Actor = GetAttachedActor())
		{
			SampleTimes.Add(Time.RoundToFrame());
			SampleTransforms.Add(Actor->GetActorTransform());
		}
	}

	// 2) Unparent, keeping the world position.
	if (AActor* Actor = GetAttachedActor())
	{
		Actor->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	}

	if (SampleTimes.Num() == 0)
	{
		InSequencer.SetLocalTime(OriginalTime);
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: nothing sampled, bake aborted."));
		return;
	}

	// 3) Replace the keys with the sampled world-space values (one undo step).
	{
		const FScopedTransaction Transaction(NSLOCTEXT("CameraAnchor", "BakeTransaction", "Bake Animation To World"));
		Section->Modify();

		for (int32 Channel = 0; Channel < 9; ++Channel)
		{
			Channels[Channel]->Reset();
		}

		double PreviousEuler[3] = { 0.0, 0.0, 0.0 };

		for (int32 i = 0; i < SampleTimes.Num(); ++i)
		{
			const FTransform& Xform = SampleTransforms[i];
			const FVector Loc = Xform.GetLocation();
			const FRotator Rot = Xform.GetRotation().Rotator();
			const FVector Scale = Xform.GetScale3D();

			double Euler[3] = { Rot.Roll, Rot.Pitch, Rot.Yaw };
			if (i > 0)
			{
				// Unwrap so a 359 -> 1 degree wrap doesn't become a full spin between keys.
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					Euler[Axis] = PreviousEuler[Axis] + FMath::UnwindDegrees(Euler[Axis] - PreviousEuler[Axis]);
				}
			}
			for (int32 Axis = 0; Axis < 3; ++Axis)
			{
				PreviousEuler[Axis] = Euler[Axis];
			}

			const double Values[9] = { Loc.X, Loc.Y, Loc.Z, Euler[0], Euler[1], Euler[2], Scale.X, Scale.Y, Scale.Z };
			for (int32 Channel = 0; Channel < 9; ++Channel)
			{
				FMovieSceneDoubleValue Key(Values[Channel]);
				Key.InterpMode = RCIM_Linear; // a key on every frame, so linear reproduces the sampled motion
				Channels[Channel]->GetData().AddKey(SampleTimes[i], Key);
			}
		}
	}

	InSequencer.SetLocalTime(OriginalTime);
	InSequencer.NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: baked %d frames to world space."), SampleTimes.Num());
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
#endif // WITH_EDITOR
