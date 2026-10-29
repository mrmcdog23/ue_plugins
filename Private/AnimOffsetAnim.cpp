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
#include "Engine/StaticMeshActor.h"
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

// ---------------------------------------------------------------------------------------------
// Transform section helpers
// Transform section channels: 0-2 location XYZ, 3-5 rotation (Roll, Pitch, Yaw), 6-8 scale XYZ.
// ---------------------------------------------------------------------------------------------

static UMovieScene3DTransformSection* FindTransformSection(UMovieScene* MovieScene, const FGuid& BindingId)
{
	UMovieScene3DTransformTrack* Track = MovieScene ? MovieScene->FindTrack<UMovieScene3DTransformTrack>(BindingId) : nullptr;
	return (Track && Track->GetAllSections().Num() > 0)
		? Cast<UMovieScene3DTransformSection>(Track->GetAllSections()[0])
		: nullptr;
}

/** Every distinct time that has a key in any of the channels, sorted. */
static void GatherKeyTimes(const TArrayView<FMovieSceneDoubleChannel*>& Channels, TArray<FFrameNumber>& OutTimes)
{
	TSet<FFrameNumber> Unique;
	for (FMovieSceneDoubleChannel* Channel : Channels)
	{
		if (!Channel)
		{
			continue;
		}
		for (const FFrameNumber& Time : Channel->GetData().GetTimes())
		{
			Unique.Add(Time);
		}
	}
	OutTimes = Unique.Array();
	OutTimes.Sort();
}

/** The transform the channels currently describe at Time (whatever space the keys are in). */
static FTransform ReadTransformAt(const TArrayView<FMovieSceneDoubleChannel*>& Channels, FFrameNumber Time)
{
	auto Eval = [&Channels, Time](int32 Index, double Default)
		{
			double Value = Default;
			Channels[Index]->Evaluate(FFrameTime(Time), Value);
			return Value;
		};

	return FTransform(
		FRotator(Eval(4, 0.0), Eval(5, 0.0), Eval(3, 0.0)), // Pitch, Yaw, Roll
		FVector(Eval(0, 0.0), Eval(1, 0.0), Eval(2, 0.0)),
		FVector(Eval(6, 1.0), Eval(7, 1.0), Eval(8, 1.0)));
}

/** Sets the value of the key at Time, adding a key there if the channel has none. */
static void SetKeyValueAt(FMovieSceneDoubleChannel* Channel, FFrameNumber Time, double Value)
{
	const int32 Index = Channel->GetData().FindKey(Time);
	if (Index != INDEX_NONE)
	{
		Channel->GetData().GetValues()[Index].Value = Value;
	}
	else
	{
		Channel->GetData().AddKey(Time, FMovieSceneDoubleValue(Value));
	}
}


void AAnimOffsetAnim::PostActorCreated()
{
	Super::PostActorCreated();
	RegisterSequencerHook();
	StartChildWatcher();
}

void AAnimOffsetAnim::PostLoad()
{
	Super::PostLoad();
	RegisterSequencerHook();
	StartChildWatcher();
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
				Self->SeenUnparented.Reset();
				Self->SequencerManaged.Reset();
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

	// Animated bindings being parented for the first time: their keys must be converted to our space.
	// Also handled after the loop, because the conversion scrubs the Sequencer.
	struct FJustParented
	{
		FGuid BindingId;
		TWeakObjectPtr<AActor> Actor;
		bool bAttach = false; // true: we still have to attach it after converting (auto-parented camera)
	};
	TArray<FJustParented> JustParented;

	for (const FMovieSceneBinding& Binding : AsConst(*MovieScene).GetBindings()) // const overload: the non-const GetBindings() is deprecated
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
				// The artist just parented an actor we had been seeing free-floating. Static actors keep their
				// world transform on their own; animated ones have world-space keys that now read as offsets.
				if (bKeepWorldOnFirstParent
					&& !ParentedByBinding.Contains(BindingId)
					&& SeenUnparented.Contains(BindingId)
					&& !ConvertedBindings.Contains(BindingId)
					&& IsBindingAnimated(MovieScene, BindingId))
				{
					FJustParented& Item = JustParented.AddDefaulted_GetRef();
					Item.BindingId = BindingId;
					Item.Actor = Actor;
					Item.bAttach = false;
				}
				SeenUnparented.Remove(BindingId);

				// Covers cameras we attached and anything the artist parented by hand.
				SequencerManaged.AddUnique(TWeakObjectPtr<AActor>(Actor));
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
					FUnparented& Item = JustUnparented.AddDefaulted_GetRef();
					Item.BindingId = BindingId;
					Item.Actor = Actor;
					Item.LocalToAnchor = LocalToAnchor;
				}
				continue;
			}

			// Free-floating: remember it, so that if it shows up under us later we know it was parented just now.
			if (!CurrentParent)
			{
				SeenUnparented.Add(BindingId);
			}

			// New or respawned actor that we have never seen under us.
			// Only cameras are parented automatically; every other actor is only
			// tracked once the artist parents it themselves (handled above).
			if (bMoveCameraOnSequenceOpen && Actor->IsA<ACameraActor>())
			{
				Actor->Modify(); // a possessed camera is a level actor: this marks the level dirty

				const bool bAnimated = IsBindingAnimated(MovieScene, BindingId);

				// First time under us and animated: convert the keys first, then attach (done after the loop).
				if (bKeepWorldOnFirstParent && bAnimated && !ConvertedBindings.Contains(BindingId))
				{
					FJustParented& Item = JustParented.AddDefaulted_GetRef();
					Item.BindingId = BindingId;
					Item.Actor = Actor;
					Item.bAttach = true;
					continue;
				}

				// Static: keep where it is. Animated and already converted (a respawn): its keys are already
				// relative to us, so KeepRelative is correct. Setting off: previous behaviour.
				const bool bKeepWorld = bKeepWorldOnFirstParent && !bAnimated;
				Actor->AttachToActor(this, bKeepWorld
					? FAttachmentTransformRules::KeepWorldTransform
					: FAttachmentTransformRules::KeepRelativeTransform);

				FTrackedActor& Tracked = ParentedByBinding.FindOrAdd(BindingId);
				Tracked.Actor = Actor;
				Tracked.LocalToAnchor = Actor->GetActorTransform().GetRelativeTransform(GetActorTransform());

				UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: attached '%s' (%s)"),
					*Actor->GetName(), *Actor->GetClass()->GetName());
			}
		}
	}

	for (const FJustParented& Item : JustParented)
	{
		if (AActor* Actor = Item.Actor.Get())
		{
			ConvertKeysToAnchorSpace(InSequencer, Item.BindingId, Actor, Item.bAttach);
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


void AAnimOffsetAnim::ConvertKeysToAnchorSpace(ISequencer& InSequencer, const FGuid& BindingId, AActor* Actor, bool bAttach)
{
	auto AttachIfNeeded = [this, Actor, bAttach]()
		{
			if (bAttach && Actor && Actor->GetAttachParentActor() != this)
			{
				Actor->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
			}
		};

	UMovieSceneSequence* Sequence = InSequencer.GetFocusedMovieSceneSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	UMovieScene3DTransformSection* Section = FindTransformSection(MovieScene, BindingId);
	if (!Section)
	{
		AttachIfNeeded();
		return;
	}

	TArrayView<FMovieSceneDoubleChannel*> Channels = Section->GetChannelProxy().GetChannels<FMovieSceneDoubleChannel>();
	if (Channels.Num() < 9)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: unexpected transform channel layout, key conversion skipped."));
		AttachIfNeeded();
		return;
	}

	TArray<FFrameNumber> KeyTimes;
	GatherKeyTimes(Channels, KeyTimes);
	if (KeyTimes.Num() == 0)
	{
		AttachIfNeeded();
		return;
	}

	const FFrameTime OriginalTime = InSequencer.GetLocalTime().Time;

	// 1) At each existing key: the keys still hold the world transform the artist authored,
	//    and this actor may itself be animated, so scrub to get where we are at that time.
	TArray<FTransform> NewRelative;
	NewRelative.Reserve(KeyTimes.Num());

	for (const FFrameNumber& Time : KeyTimes)
	{
		const FTransform World = ReadTransformAt(Channels, Time);

		InSequencer.SetLocalTime(FFrameTime(Time));
		InSequencer.ForceEvaluate();
		const FTransform AnchorWorld = GetActorTransform();

		NewRelative.Add(World.GetRelativeTransform(AnchorWorld));
	}

	// 2) Rewrite the values on the existing key times (one undo step). No extra keys are made per frame.
	{
		const FScopedTransaction Transaction(NSLOCTEXT("CameraAnchor", "ConvertTransaction", "Keep World Position On Parent"));
		Section->Modify();
		Modify();

		double PreviousEuler[3] = { 0.0, 0.0, 0.0 };

		for (int32 i = 0; i < KeyTimes.Num(); ++i)
		{
			const FTransform& Xform = NewRelative[i];
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
				SetKeyValueAt(Channels[Channel], KeyTimes[i], Values[Channel]);
			}
		}

		for (int32 Channel = 0; Channel < 9; ++Channel)
		{
			Channels[Channel]->AutoSetTangents();
		}

		ConvertedBindings.Add(BindingId);
	}

	AttachIfNeeded();

	InSequencer.SetLocalTime(OriginalTime);
	InSequencer.NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
	InSequencer.ForceEvaluate();

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: kept world position for '%s' (%d keys converted)."),
		*Actor->GetName(), KeyTimes.Num());
}


void AAnimOffsetAnim::OfferKeepTransform(AActor* Actor, const FTransform& LocalToAnchor)
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

	// NOTE: on UE 5.0-5.2 the title parameter is a pointer: pass &Title instead.
	if (FMessageDialog::Open(EAppMsgType::YesNo, Message, Title) == EAppReturnType::Yes)
	{
		// Child-in-anchor-space * anchor-in-world = where it was sitting while parented.
		const FTransform DesiredWorld = LocalToAnchor * GetActorTransform();

		const FScopedTransaction Transaction(NSLOCTEXT("CameraAnchor", "KeepTransaction", "Keep Transform After Unparent"));
		Actor->Modify();
		Actor->SetActorTransform(DesiredWorld, false, nullptr, ETeleportType::TeleportPhysics);
	}
}


// The child watcher only handles static mesh actors. To cover every kind of actor, make this return true for any non-null actor.
static bool IsWatchedChildType(const AActor* Actor)
{
	return Actor && Actor->IsA<AStaticMeshActor>();
}


void AAnimOffsetAnim::StartChildWatcher()
{
	if (HasAnyFlags(RF_ClassDefaultObject) || bChildWatcherStarted)
	{
		return;
	}
	bChildWatcherStarted = true;

	TWeakObjectPtr<AAnimOffsetAnim> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float)
		{
			AAnimOffsetAnim* Self = WeakThis.Get();
			return Self ? Self->TickChildWatcher() : false; // actor is gone: stop
		}), 0.1f);
}


bool AAnimOffsetAnim::TickChildWatcher()
{
	if (IsRunningCommandlet())
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!GEditor || !World)
	{
		return true; // not ready yet
	}
	if (World->IsGameWorld())
	{
		return false; // PIE copy of this actor: the editor-only watcher is not needed there
	}
	if (World != GEditor->GetEditorWorldContext().World() || bPromptOpen)
	{
		return true;
	}

	TArray<AActor*> ChildActors;
	GetAttachedActors(ChildActors);

	// Add new static meshes, and refresh where each one sits relative to us.
	for (AActor* Child : ChildActors)
	{
		if (!IsWatchedChildType(Child))
		{
			continue;
		}

		const FTransform LocalToAnchor = Child->GetActorTransform().GetRelativeTransform(GetActorTransform());

		FWatchedChild* Existing = WatchedChildren.FindByPredicate([Child](const FWatchedChild& Watched)
			{
				return Watched.Actor.Get() == Child;
			});
		if (Existing)
		{
			Existing->LocalToAnchor = LocalToAnchor;
		}
		else
		{
			FWatchedChild& Added = WatchedChildren.AddDefaulted_GetRef();
			Added.Actor = Child;
			Added.LocalToAnchor = LocalToAnchor;
		}
	}

	// Anything we were watching that is no longer our child was unparented (or deleted).
	TArray<TWeakObjectPtr<AActor>> LostActors;
	TArray<FTransform> LostTransforms;

	for (int32 Index = WatchedChildren.Num() - 1; Index >= 0; --Index)
	{
		AActor* Actor = WatchedChildren[Index].Actor.Get();
		if (Actor && ChildActors.Contains(Actor))
		{
			continue; // still ours
		}

		// Only a real "unparent" if the actor still exists, is free-floating (not re-parented elsewhere),
		// and the Sequencer path isn't already handling it.
		if (IsValid(Actor) && !Actor->IsActorBeingDestroyed() && !Actor->GetAttachParentActor()
			&& !SequencerManaged.Contains(TWeakObjectPtr<AActor>(Actor)))
		{
			LostActors.Add(Actor);
			LostTransforms.Add(WatchedChildren[Index].LocalToAnchor);
		}
		WatchedChildren.RemoveAt(Index);
	}

	if (!bPromptToBakeOnUnparent || LostActors.Num() == 0)
	{
		return true;
	}

	bPromptOpen = true;
	for (int32 Index = 0; Index < LostActors.Num(); ++Index)
	{
		if (AActor* Actor = LostActors[Index].Get())
		{
			OfferKeepTransform(Actor, LostTransforms[Index]);
		}
	}
	bPromptOpen = false;

	return true;
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
				"Yes: re-keys it in world space on its existing keys, so it keeps moving as it did while parented.\n"
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
		OfferKeepTransform(Actor, LocalToAnchor);
	}

	bPromptOpen = false;

	// Its keys are no longer relative to us, so if it is parented again later they must be converted again.
	if (ConvertedBindings.Contains(BindingId))
	{
		Modify();
		ConvertedBindings.Remove(BindingId);
	}

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

	UMovieScene3DTransformSection* Section = FindTransformSection(MovieScene, BindingId);
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

	// Only the times that already have a key are baked, not every frame.
	TArray<FFrameNumber> KeyTimes;
	GatherKeyTimes(Channels, KeyTimes);
	if (KeyTimes.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: no keys to bake."));
		return;
	}

	// Remember each original key so its interpolation mode survives the rewrite.
	TArray<TMap<FFrameNumber, FMovieSceneDoubleValue>> OriginalKeys;
	OriginalKeys.SetNum(9);
	for (int32 Channel = 0; Channel < 9; ++Channel)
	{
		TArrayView<const FFrameNumber> Times = Channels[Channel]->GetData().GetTimes();
		TArrayView<FMovieSceneDoubleValue> Values = Channels[Channel]->GetData().GetValues();
		for (int32 i = 0; i < Times.Num(); ++i)
		{
			OriginalKeys[Channel].Add(Times[i], Values[i]);
		}
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

	const FFrameTime OriginalTime = InSequencer.GetLocalTime().Time;

	// Re-parent before sampling so the very first evaluated key is already applied relative to us.
	GetAttachedActor();

	// 1) Sample: scrub to each existing key time with the actor parented, record its WORLD transform.
	TArray<FFrameNumber> SampleTimes;
	TArray<FTransform> SampleTransforms;

	for (const FFrameNumber& Time : KeyTimes)
	{
		InSequencer.SetLocalTime(FFrameTime(Time));
		InSequencer.ForceEvaluate();

		if (AActor* Actor = GetAttachedActor())
		{
			SampleTimes.Add(Time);
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

	// 3) Replace the keys with the sampled world-space values, on the original key times only (one undo step).
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
				FMovieSceneDoubleValue Key(Values[Channel]); // cubic, auto tangents by default
				if (const FMovieSceneDoubleValue* Original = OriginalKeys[Channel].Find(SampleTimes[i]))
				{
					Key.InterpMode = Original->InterpMode; // keep linear / constant keys as they were
				}
				Channels[Channel]->GetData().AddKey(SampleTimes[i], Key);
			}
		}

		for (int32 Channel = 0; Channel < 9; ++Channel)
		{
			Channels[Channel]->AutoSetTangents();
		}
	}

	InSequencer.SetLocalTime(OriginalTime);
	InSequencer.NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: baked %d keys to world space."), SampleTimes.Num());
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
