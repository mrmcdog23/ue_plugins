// Fill out your copyright notice in the Description page of Project Settings.


#include "AnimOffsetAnim.h"
#include "Camera/CameraActor.h"
#include "Components/ArrowComponent.h"
#include "Components/SceneComponent.h"
#include "EngineUtils.h"   // TActorIterator
#include "Engine/Level.h"   // ULevel::bIsVisible / IsPersistentLevel

#if WITH_EDITOR
#include "Containers/Ticker.h"
#include "Curves/RealCurve.h"
#include "Editor.h"
#include "Engine/LevelStreaming.h"
#include "Engine/Selection.h"
#include "FileHelpers.h"   // FEditorFileUtils (UnrealEd)
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Engine/World.h"
#include "LevelUtils.h"   // FLevelUtils::IsLevelLocked (UnrealEd)
#include "Animation/SkeletalMeshActor.h"
#include "Engine/StaticMeshActor.h"
#include "ISequencer.h"
#include "ISequencerModule.h"
#include "LevelEditorSequencerIntegration.h" // module "Sequencer"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "MovieScene.h"
#include "MovieSceneSequence.h"
#include "MovieSceneSpawnable.h"
#include "ScopedTransaction.h"
#include "Channels/MovieSceneDoubleChannel.h"
#include "Sections/MovieScene3DTransformSection.h"
#include "Tracks/MovieScene3DTransformTrack.h"
#include "DragAndDrop/ActorDragDropOp.h"             // UnrealEd
#include "Framework/Application/IInputProcessor.h"   // Slate
#include "Framework/Application/SlateApplication.h"
#include "Input/Events.h"
#include "InputCoreTypes.h"
#include "SceneOutlinerDragDrop.h"                   // module "SceneOutliner"
#include "Widgets/Text/STextBlock.h"
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
	// A follower (sublevel anchor) only copies the leader. The leader in the persistent level parents the cameras.
	if (bIsFollower)
	{
		SyncToLeader();
		return;
	}

	if (bMoveCameraOnSequenceOpen)
	{
		AttachRuntimeCameras();
	}
}


AAnimOffsetAnim* AAnimOffsetAnim::FindLeader() const
{
	const UWorld* World = GetWorld();
	const ULevel* Persistent = World ? World->PersistentLevel.Get() : nullptr;
	if (!Persistent)
	{
		return nullptr;
	}

	for (AActor* Candidate : Persistent->Actors)
	{
		AAnimOffsetAnim* Anchor = Cast<AAnimOffsetAnim>(Candidate);
		if (Anchor && Anchor != this && !Anchor->bIsFollower && IsValid(Anchor))
		{
			return Anchor;
		}
	}
	return nullptr;
}


void AAnimOffsetAnim::SyncToLeader()
{
	if (!bIsFollower)
	{
		return;
	}

	AAnimOffsetAnim* Leader = CachedLeader.Get();
	if (!Leader || Leader->bIsFollower)
	{
		// Scanning the persistent level is not free, so look for a leader at most once a second.
		const double Now = FPlatformTime::Seconds();
		if (Now < NextLeaderSearchTime)
		{
			return;
		}
		NextLeaderSearchTime = Now + 1.0;

		Leader = FindLeader();
		CachedLeader = Leader;
		if (!Leader)
		{
			return;
		}
	}

	// World space copy: works with sublevel offsets too, because actor transforms are already world space.
	const FTransform Target = Leader->GetActorTransform();
	if (!GetActorTransform().Equals(Target, 1e-4))
	{
		SetActorTransform(Target, false, nullptr, ETeleportType::TeleportPhysics);
	}
}


// ---------------------------------------------------------------------------------------------
// Level helper (persistent level + streaming sublevels)
// ---------------------------------------------------------------------------------------------

/**
 * True if the actor is alive and sits in a level that is currently loaded and visible in its world.
 * An actor in a hidden or still-streaming sublevel is not "live": its components are unregistered,
 * so its attachments cannot be trusted (a hidden sublevel must not look like "the artist unparented it").
 */
static bool IsActorInLiveLevel(const AActor* Actor)
{
	if (!IsValid(Actor) || Actor->IsActorBeingDestroyed() || !Actor->GetWorld())
	{
		return false;
	}
	const ULevel* Level = Actor->GetLevel();
	return Level && (Level->IsPersistentLevel() || Level->bIsVisible);
}


void AAnimOffsetAnim::AttachRuntimeCameras()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld() || !IsActorInLiveLevel(this))
	{
		return;
	}

	// Explicit level camera. It may live in a sublevel that is not streamed in yet (soft pointer resolves to null until then).
	// Only claim it if no anchor owns it already, so anchors in several levels don't steal it back and forth every tick.
	if (ACameraActor* Target = TargetCamera.Get())
	{
		const AActor* TargetParent = Target->GetAttachParentActor();
		if (IsActorInLiveLevel(Target) && (!TargetParent || !TargetParent->IsA<AAnimOffsetAnim>()))
		{
			Target->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
		}
	}

	for (TActorIterator<ACameraActor> It(World); It; ++It)
	{
		ACameraActor* Camera = *It;

		// Only sequence-spawned cameras. Placed level cameras are net-startup actors
		// (their parent, if any, was saved with the level and is already in place).
		// Skip cameras already under ANY anchor (this one, or another anchor in the persistent level / a sublevel),
		// otherwise two anchors would re-parent the same camera every tick. The first anchor to tick claims it.
		const AActor* CameraParent = Camera->GetAttachParentActor();
		if (Camera->IsNetStartupActor()
			|| (CameraParent && CameraParent->IsA<AAnimOffsetAnim>())
			|| !IsActorInLiveLevel(Camera))
		{
			continue;
		}

		// KeepRelative: Sequencer writes the keyed values as the relative transform,
		// which is exactly how they were authored while attached in the editor.
		Camera->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
	}
}


#if WITH_EDITOR

bool AAnimOffsetAnim::bPromptOpen = false;
bool AAnimOffsetAnim::bSuppressCreationHooks = false;

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
	if (bSuppressCreationHooks)
	{
		return; // spawned into a background-loaded parent map: nothing to hook
	}
	RegisterSequencerHook();
	RegisterOutlinerDropHook();
	StartChildWatcher();
	StartFollowerSync();

	// Deferred: SpawnActor() sets bIsFollower only AFTER PostActorCreated, so a follower cannot be told apart yet.
	// Dragging an anchor into the viewport first creates a TRANSIENT preview actor whose flag is cleared on drop,
	// so keep polling while it is transient and relocate once it is a real actor.
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		TWeakObjectPtr<AAnimOffsetAnim> WeakThis(this);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float)
			{
				AAnimOffsetAnim* Self = WeakThis.Get();
				if (!Self || Self->IsTemplate())
				{
					return false; // preview was cancelled / destroyed
				}
				UWorld* World = Self->GetWorld();
				if (!World)
				{
					return true; // not ready yet
				}
				if (World->IsGameWorld())
				{
					return false;
				}
				if (Self->HasAnyFlags(RF_Transient))
				{
					return true; // still a drag preview
				}
				Self->RelocateToPersistentLevelIfNeeded();
				return false; // one shot once it is real
			}), 0.0f);
	}
}

void AAnimOffsetAnim::PostLoad()
{
	Super::PostLoad();
	RegisterSequencerHook();
	RegisterOutlinerDropHook();
	StartChildWatcher();
	StartFollowerSync();
}

void AAnimOffsetAnim::BeginDestroy()
{
	UnregisterOutlinerDropHook();
	UnregisterSequencerHook();
	Super::BeginDestroy();
}

void AAnimOffsetAnim::OnSequencerCreated(TSharedRef<ISequencer> InSequencer)
{
	// Note: not gated on any flag. Tracking of bound actors (parent / unparent detection) always runs.
	if (!GEditor || GetWorld() != GEditor->GetEditorWorldContext().World())
	{
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: sequencer opened, watching bound actors."));
	StartSequencerPoll(InSequencer);
}


void AAnimOffsetAnim::StartSequencerPoll(TSharedRef<ISequencer> InSequencer)
{
	// Already watching this exact Sequencer: don't start a second poll.
	if (ActiveSequencer.Pin() == InSequencer.ToSharedPtr())
	{
		return;
	}

	// Without this the child watcher has no Sequencer, so it can only offer "keep transform"
	// and never the "bake animation" prompt for animated actors.
	ActiveSequencer = InSequencer;

	TWeakObjectPtr<AAnimOffsetAnim> WeakThis(this);
	TWeakPtr<ISequencer> WeakSequencer = InSequencer;

	// Poll while this Sequencer is open, so actors the artist parents/unparents are noticed.
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


void AAnimOffsetAnim::TryAdoptOpenSequencer()
{
	if (ActiveSequencer.IsValid())
	{
		return;
	}

	// RegisterOnSequencerCreated only fires for Sequencers created AFTER we registered. If the level sequence
	// was already open (actor placed / level loaded / Live Coding reload while it was open) we never heard about it,
	// so look for it explicitly.
	// NOTE: check that GetSequencers() exists in your engine version (see LevelEditorSequencerIntegration.h).
	for (const TWeakPtr<ISequencer>& Weak : FLevelEditorSequencerIntegration::Get().GetSequencers())
	{
		if (TSharedPtr<ISequencer> Seq = Weak.Pin())
		{
			UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: found an already-open sequencer, watching bound actors."));
			StartSequencerPoll(Seq.ToSharedRef());
			break;
		}
	}
}


void AAnimOffsetAnim::TrackBoundActors(ISequencer& InSequencer)
{
	// A bake/prompt is in progress (the modal dialog pumps the ticker), don't re-enter.
	if (bPromptOpen)
	{
		return;
	}

	// Nothing to do if the prompts are off.
	if (!bPromptToBakeOnUnparent)
	{
		return;
	}

	// This anchor sits in a hidden / loading sublevel: its attachment data is not reliable right now.
	if (!IsActorInLiveLevel(this))
	{
		return;
	}

	SyncToLeader(); // follower: children are measured against the leader's current transform

	UMovieSceneSequence* Sequence = InSequencer.GetFocusedMovieSceneSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;
	if (!MovieScene)
	{
		UE_LOG(LogTemp, Warning, TEXT("No movie scene found."));
		return;
	}

	// Bindings whose actor the artist just unparented. Handled after the loop,
	// because the popup / bake scrubs the Sequencer and must not run mid-iteration.
	TArray<FUnparentedItem> JustUnparented;

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

			// Bound actor lives in a sublevel that is hidden / still streaming: hiding a level unregisters its components,
			// which would otherwise look like the artist unparented it. Ignore it until the level is visible again.
			if (!IsActorInLiveLevel(Actor))
			{
				continue;
			}

			AActor* CurrentParent = Actor->GetAttachParentActor();

			if (OptedOutBindings.Contains(BindingId))
			{
				if (CurrentParent != this)
				{
					// The artist took this actor out from under us: leave it alone (and never auto-parent it again).
					if (!CurrentParent)
					{
						SeenUnparented.Add(BindingId);
					}
					continue;
				}

				// The artist has parented it under us again: track it again, and treat it as a first parent
				// so its (world space) keys are converted, otherwise it would offset and never be baked on the next unparent.
				OptedOutBindings.Remove(BindingId);
				SeenUnparented.Add(BindingId);
			}

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
					FUnparentedItem& Item = JustUnparented.AddDefaulted_GetRef();
					Item.BindingId = BindingId;
					Item.Actor = Actor;
					Item.LocalToAnchor = LocalToAnchor;
				}
				continue;
			}

			// A spawnable camera we had seen under us was respawned by Sequencer (spawnables come back unparented).
			// Put it back where the artist had it. Cameras the artist never parented are never touched.
			const bool bRespawnedTracked = Previous
				&& Previous->Actor.Get() != Actor
				&& !CurrentParent
				&& Actor->IsA<ACameraActor>();

			if (bRespawnedTracked)
			{
				Actor->Modify();
				// Its keys are already relative to us, so KeepRelative is correct.
				Actor->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);

				FTrackedActor& Tracked = ParentedByBinding.FindOrAdd(BindingId);
				Tracked.Actor = Actor;
				Tracked.LocalToAnchor = Actor->GetActorTransform().GetRelativeTransform(GetActorTransform());
				continue;
			}

			// Free-floating: remember it, so that if it shows up under us later we know it was parented just now.
			if (!CurrentParent)
			{
				SeenUnparented.Add(BindingId);
			}
			// No auto-parenting: every actor, cameras included, is only tracked once the artist parents it themselves.
		}
	}

	for (const FJustParented& Item : JustParented)
	{
		if (AActor* Actor = Item.Actor.Get())
		{
			ConvertKeysToAnchorSpace(InSequencer, Item.BindingId, Actor, Item.bAttach);
		}
	}

	if (JustUnparented.Num() > 0)
	{
		HandleActorsUnparented(InSequencer, JustUnparented);
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
		SyncToLeader(); // follower: take the leader's transform at this time right now, not on the next tick
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
	const FText Message = NSLOCTEXT("CameraAnchor", "KeepPrompt",
		"Bake with keyframes only?\n\n"
		"Yes\n"
		"No");
	const FText Title = NSLOCTEXT("CameraAnchor", "KeepTitle", "Keep Transform?");

	// NOTE: on UE 5.0-5.2 the title parameter is a pointer: pass &Title instead.
	if (FMessageDialog::Open(EAppMsgType::YesNo, Message, Title) == EAppReturnType::Yes)
	{
		SyncToLeader();

		// Child-in-anchor-space * anchor-in-world = where it was sitting while parented.
		const FTransform DesiredWorld = LocalToAnchor * GetActorTransform();

		const FScopedTransaction Transaction(NSLOCTEXT("CameraAnchor", "KeepTransaction", "Keep Transform After Unparent"));
		Actor->Modify();
		Actor->SetActorTransform(DesiredWorld, false, nullptr, ETeleportType::TeleportPhysics);
	}
}


// The child watcher handles static mesh actors, skeletal mesh actors and cameras (ACineCameraActor derives from ACameraActor,
// so it is covered too). To cover every kind of actor, make this return true for any non-null actor.
static bool IsWatchedChildType(const AActor* Actor)
{
	return Actor
		&& (Actor->IsA<AStaticMeshActor>()
			|| Actor->IsA<ASkeletalMeshActor>()
			|| Actor->IsA<ACameraActor>());
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

	// Anchor itself is in a hidden / still-streaming sublevel: wait until it is live.
	if (!IsActorInLiveLevel(this))
	{
		return true;
	}

	SyncToLeader();

	// Picks up a Sequencer that was already open before we registered (the creation event never fires for it).
	TryAdoptOpenSequencer();

	// Leader: keep one follower alive in every loaded sublevel (checked once a second).
	if (!bIsFollower && bAutoCreateSublevelFollowers)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now >= NextFollowerCheckTime)
		{
			NextFollowerCheckTime = Now + 1.0;
			EnsureSublevelFollowers(false);
		}
	}

	TArray<AActor*> ChildActors;
	GetAttachedActors(ChildActors);

	// Add new static meshes, and refresh where each one sits relative to us.
	for (AActor* Child : ChildActors)
	{
		if (!IsWatchedChildType(Child) || !IsActorInLiveLevel(Child))
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

		// The child sits in a sublevel that was hidden (or is mid-unload). That drops it from our attached list
		// but is NOT an unparent: keep watching, it is picked up again when the level is shown.
		if (IsValid(Actor) && Actor->GetLevel()
			&& !Actor->GetLevel()->IsPersistentLevel() && !Actor->GetLevel()->bIsVisible)
		{
			continue;
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

	TArray<FUnparentedItem> LostItems;
	for (int32 Index = 0; Index < LostActors.Num(); ++Index)
	{
		FUnparentedItem& Item = LostItems.AddDefaulted_GetRef();
		Item.Actor = LostActors[Index];
		Item.LocalToAnchor = LostTransforms[Index];
	}

	// If a Sequencer is open, let it decide per actor: animated ones are baked, static ones get "keep transform".
	if (TSharedPtr<ISequencer> Sequencer = ActiveSequencer.Pin())
	{
		HandleActorsUnparented(*Sequencer, LostItems);
	}
	else
	{
		bPromptOpen = true;
		OfferKeepTransforms(LostItems); // one prompt for everything unparented in this poll
		bPromptOpen = false;
	}

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


FGuid AAnimOffsetAnim::ResolveAnimatedBinding(ISequencer& InSequencer, UMovieScene* MovieScene, AActor* Actor, const FGuid& PreferredBinding) const
{
	if (!MovieScene || !Actor)
	{
		return FGuid();
	}
	if (PreferredBinding.IsValid() && IsBindingAnimated(MovieScene, PreferredBinding))
	{
		return PreferredBinding;
	}

	// The animation may live on the actor's root component binding, or the actor may be bound under another GUID.
	const UObject* ActorRoot = Actor->GetRootComponent();
	for (const FMovieSceneBinding& Binding : AsConst(*MovieScene).GetBindings())
	{
		const FGuid Id = Binding.GetObjectGuid();
		if (!IsBindingAnimated(MovieScene, Id))
		{
			continue;
		}
		for (TWeakObjectPtr<> Object : InSequencer.FindBoundObjects(Id, InSequencer.GetFocusedTemplateID()))
		{
			const UObject* Bound = Object.Get();
			if (Bound && (Bound == Actor || Bound == ActorRoot))
			{
				return Id;
			}
		}
	}
	return FGuid();
}


void AAnimOffsetAnim::OfferKeepTransforms(const TArray<FUnparentedItem>& Items)
{
	TArray<const FUnparentedItem*> Valid;
	FString Names;
	for (const FUnparentedItem& Item : Items)
	{
		if (AActor* Actor = Item.Actor.Get())
		{
			Valid.Add(&Item);
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + FString::Printf(TEXT("'%s'"), *Actor->GetActorLabel());
		}
	}
	if (Valid.Num() == 0)
	{
		return;
	}

	const FText Message = FText::Format(
		NSLOCTEXT("CameraAnchor", "KeepPromptMulti",
			"{0} unparented from '{1}' and have no transform animation.\n\n"
			"Do you want to keep the transform they had under '{1}'?\n\n"
			"Yes: moves each one to where it was sitting while parented (world space, using '{1}' as it is now).\n"
			"No: leaves them exactly where they are now."),
		FText::FromString(Names),
		FText::FromString(GetActorLabel()));
	const FText Title = NSLOCTEXT("CameraAnchor", "KeepTitleMulti", "Keep Transform?");

	// NOTE: on UE 5.0-5.2 the title parameter is a pointer: pass &Title instead.
	if (FMessageDialog::Open(EAppMsgType::YesNo, Message, Title) == EAppReturnType::Yes)
	{
		SyncToLeader();

		const FScopedTransaction Transaction(NSLOCTEXT("CameraAnchor", "KeepTransactionMulti", "Keep Transform After Unparent"));
		for (const FUnparentedItem* Item : Valid)
		{
			if (AActor* Actor = Item->Actor.Get())
			{
				Actor->Modify();
				Actor->SetActorTransform(Item->LocalToAnchor * GetActorTransform(), false, nullptr, ETeleportType::TeleportPhysics);
			}
		}
	}
}


void AAnimOffsetAnim::HandleActorsUnparented(ISequencer& InSequencer, const TArray<FUnparentedItem>& Items)
{
	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: %d actor(s) unparented."), Items.Num());

	if (!bPromptToBakeOnUnparent || Items.Num() == 0)
	{
		return;
	}

	UMovieSceneSequence* Sequence = InSequencer.GetFocusedMovieSceneSequence();
	UMovieScene* MovieScene = Sequence ? Sequence->GetMovieScene() : nullptr;

	// Split up front, while every actor pointer is still valid.
	TArray<FUnparentedItem> Animated;
	TArray<FUnparentedItem> Static;
	FString AnimatedNames;
	bool bAnyCamera = false;

	for (const FUnparentedItem& Item : Items)
	{
		AActor* Actor = Item.Actor.Get();
		if (!Actor)
		{
			continue;
		}
		bAnyCamera |= Actor->IsA<ACameraActor>();

		const FGuid AnimatedBinding = ResolveAnimatedBinding(InSequencer, MovieScene, Actor, Item.BindingId);
		if (AnimatedBinding.IsValid())
		{
			Animated.Add_GetRef(Item).BindingId = AnimatedBinding;
			AnimatedNames += (AnimatedNames.IsEmpty() ? TEXT("") : TEXT(", ")) + FString::Printf(TEXT("'%s'"), *Actor->GetActorLabel());
		}
		else
		{
			Static.Add(Item);
		}
	}

	bPromptOpen = true;

	if (Animated.Num() > 0)
	{
		const FText Message = NSLOCTEXT("CameraAnchor", "BakePromptMulti", "Bake with keyframes only?");
		const FText Title = NSLOCTEXT("CameraAnchor", "BakeTitleMulti", "Bake Animation?");

		// NOTE: on UE 5.0-5.2 the title parameter is a pointer: pass &Title instead.
		if (FMessageDialog::Open(EAppMsgType::YesNo, Message, Title) == EAppReturnType::Yes)
		{
			// Each bake finds its actor again through the binding, so one respawning cannot skip the rest.
			for (const FUnparentedItem& Item : Animated)
			{
				BakeActorToWorld(InSequencer, Item.BindingId);
			}
			InSequencer.NotifyMovieSceneDataChanged(EMovieSceneDataChangeType::TrackValueChanged);
		}
	}

	if (Static.Num() > 0)
	{
		OfferKeepTransforms(Static);
	}

	bPromptOpen = false;

	// Their keys are no longer relative to us, so if they are parented again later they must be converted again.
	for (const FUnparentedItem& Item : Items)
	{
		if (ConvertedBindings.Contains(Item.BindingId))
		{
			Modify();
			ConvertedBindings.Remove(Item.BindingId);
		}
	}

	// A camera is no longer meant to live under this actor, so stop re-parenting cameras at runtime too.
	// (After a bake / keep the transform is world space, so re-parenting in PIE/MRQ would apply the offset twice.)
	// Other actors are never auto-parented at runtime, so there is nothing to switch off for them.
	if (bAnyCamera)
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
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: No movie scene found."));
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

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: Work out baking..."));
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

	const FFrameTime OriginalTime = InSequencer.GetLocalTime().Time;

	// 1) At each existing key the channels hold the transform relative to this actor, so read it straight from the
	//    keys (no re-attaching and no relying on the actor having been evaluated), and scrub only to learn where
	//    this actor itself is at that time. World = key (relative to us) * our world transform.
	TArray<FFrameNumber> SampleTimes;
	TArray<FTransform> SampleTransforms;

	for (const FFrameNumber& Time : KeyTimes)
	{
		const FTransform LocalToAnchor = ReadTransformAt(Channels, Time);

		InSequencer.SetLocalTime(FFrameTime(Time));
		InSequencer.ForceEvaluate();
		SyncToLeader(); // follower: take the leader's transform at this time right now, not on the next tick

		SampleTimes.Add(Time);
		SampleTransforms.Add(LocalToAnchor * GetActorTransform());
	}

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: baking %d keys."), SampleTimes.Num());

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

	// No NotifyMovieSceneDataChanged here: the caller does it once after all actors are baked,
	// because it can respawn spawnables and would invalidate the ones still waiting their turn.
	InSequencer.SetLocalTime(OriginalTime);
	InSequencer.ForceEvaluate(); // the new world keys now drive the (unparented) actor

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: baked %d keys to world space."), SampleTimes.Num());
}


void AAnimOffsetAnim::StartFollowerSync()
{
	if (HasAnyFlags(RF_ClassDefaultObject) || bFollowerSyncStarted)
	{
		return;
	}
	bFollowerSyncStarted = true;

	// Every frame (interval 0): a follower must track the leader while the artist scrubs / drags it, not 0.1 s later.
	TWeakObjectPtr<AAnimOffsetAnim> WeakThis(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakThis](float)
		{
			AAnimOffsetAnim* Self = WeakThis.Get();
			if (!Self)
			{
				return false;
			}

			UWorld* World = Self->GetWorld();
			if (!GEditor || !World)
			{
				return true; // not ready yet
			}
			if (World->IsGameWorld())
			{
				return false; // PIE copy: AActor::Tick does the syncing there
			}
			if (Self->bIsFollower && World == GEditor->GetEditorWorldContext().World())
			{
				Self->SyncToLeader();
			}
			return true;
		}), 0.0f);
}


void AAnimOffsetAnim::RelocateToPersistentLevelIfNeeded()
{
	UWorld* World = GetWorld();
	ULevel* MyLevel = GetLevel();

	// A sublevel opened ON ITS OWN is the persistent level of that editor world, so there is nowhere to relocate to.
	// Look for maps that stream this one and tell the artist to open one of them instead.
	if (GEditor && World && !World->IsGameWorld() && MyLevel && IsValid(this)
		&& !IsTemplate() && !HasAnyFlags(RF_Transient) && !bIsFollower
		&& MyLevel->IsPersistentLevel() && World->GetStreamingLevels().Num() == 0)
	{
		const FName ThisPackage = World->GetOutermost()->GetFName();
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();

		TArray<FName> Referencers;
		Registry.GetReferencers(ThisPackage, Referencers);

		TArray<FString> ParentMaps;
		TArray<FName> ParentPackages;
		for (const FName& Referencer : Referencers)
		{
			if (Referencer == ThisPackage)
			{
				continue;
			}
			TArray<FAssetData> Assets;
			Registry.GetAssetsByPackageName(Referencer, Assets);
			for (const FAssetData& Asset : Assets)
			{
				if (Asset.AssetClassPath == UWorld::StaticClass()->GetClassPathName())
				{
					ParentMaps.AddUnique(Asset.AssetName.ToString());
					ParentPackages.AddUnique(Referencer);
					break;
				}
			}
		}

		if (ParentMaps.Num() > 0)
		{
			const FString Parents = FString::Join(ParentMaps, TEXT(", "));
			const FString ChosenParent = ParentMaps[0];

			bool bDone = CreateLeaderInParentMap(ParentPackages[0], GetActorTransform());
			if (bDone)
			{
				// This one is exactly what the leader would have auto-created for this sublevel: make it the follower.
				bIsFollower = true;
				SetActorLabel(FString::Printf(TEXT("%s_Follower_%s"), TEXT("AnimOffsetAnim"), *World->GetName()));
				MarkPackageDirty();
			}

			FNotificationInfo Info(FText::FromString(bDone
				? FString::Printf(TEXT("Camera Anchor: created in '%s' and saved. This anchor is now the sublevel follower - save this map too."), *ChosenParent)
				: FString::Printf(TEXT("Camera Anchor: not created. Open '%s' to place the anchor in the persistent level."), *Parents)));
			Info.ExpireDuration = 10.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
		return;
	}

	if (!GEditor || !World || World->IsGameWorld() || !MyLevel || !IsValid(this)
		|| IsTemplate() || HasAnyFlags(RF_Transient)
		|| bIsFollower                      // followers are SUPPOSED to live in sublevels
		|| MyLevel->IsPersistentLevel())    // already where it belongs
	{
		UE_LOG(LogTemp, Log, TEXT("CameraAnchor: relocate skipped for '%s' (follower=%d, persistent=%d, transient=%d, template=%d)"),
			*GetName(), bIsFollower, MyLevel && MyLevel->IsPersistentLevel(), HasAnyFlags(RF_Transient), IsTemplate());
		if (World && MyLevel)
		{
			UE_LOG(LogTemp, Log, TEXT("CameraAnchor: level info -> actor level='%s', persistent='%s', current level='%s', world='%s', streaming levels=%d"),
				*GetNameSafe(MyLevel->GetOuter()),
				*GetNameSafe(World->PersistentLevel ? World->PersistentLevel->GetOuter() : nullptr),
				*GetNameSafe(World->GetCurrentLevel() ? World->GetCurrentLevel()->GetOuter() : nullptr),
				*World->GetName(), World->GetStreamingLevels().Num());
		}
		return;
	}

	ULevel* Persistent = World->PersistentLevel.Get();
	if (!Persistent)
	{
		return;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("CameraAnchor", "MoveToPersistent", "Move Camera Anchor to Persistent Level"));

	// Re-use a leader that already exists in the persistent level instead of making a second one.
	AAnimOffsetAnim* Leader = nullptr;
	for (AActor* Candidate : Persistent->Actors)
	{
		AAnimOffsetAnim* Anchor = Cast<AAnimOffsetAnim>(Candidate);
		if (Anchor && Anchor != this && !Anchor->bIsFollower && IsValid(Anchor))
		{
			Leader = Anchor;
			break;
		}
	}

	if (!Leader)
	{
		FActorSpawnParameters Params;
		Params.OverrideLevel = Persistent;
		Params.ObjectFlags = RF_Transactional;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		Leader = World->SpawnActor<AAnimOffsetAnim>(GetClass(), GetActorTransform(), Params);
		if (!Leader)
		{
			UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: could not create an anchor in the persistent level; keeping the sublevel one."));
			return;
		}

		// Carry over the artist's settings and name.
		Leader->TargetCamera = TargetCamera;
		Leader->bMoveCameraOnSequenceOpen = bMoveCameraOnSequenceOpen;
		Leader->bKeepWorldOnFirstParent = bKeepWorldOnFirstParent;
		Leader->bPromptToBakeOnUnparent = bPromptToBakeOnUnparent;
		Leader->bAutoCreateSublevelFollowers = bAutoCreateSublevelFollowers;
		Leader->SetActorLabel(GetActorLabel());
		Leader->MarkPackageDirty();
	}

	UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: '%s' was created in a sublevel; deleted it and used '%s' in the persistent level instead."),
		*GetActorLabel(), *Leader->GetActorLabel());

	// Hand the selection to the persistent anchor, then remove this one.
	if (GEditor->GetSelectedActors()->IsSelected(this))
	{
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Leader, true, true);
	}
	World->EditorDestroyActor(this, true);

	// This sublevel no longer has an anchor, so it gets a follower (the leader must be the one in the persistent level).
	if (Leader->bAutoCreateSublevelFollowers)
	{
		Leader->EnsureSublevelFollowers(false);
	}
}


bool AAnimOffsetAnim::CreateLeaderInParentMap(FName ParentPackageName, const FTransform& SublevelTransform)
{
	const FName ThisPackage = GetWorld() ? GetWorld()->GetOutermost()->GetFName() : NAME_None;

	UPackage* ParentPackage = LoadPackage(nullptr, *ParentPackageName.ToString(), LOAD_None);
	UWorld* ParentWorld = ParentPackage ? UWorld::FindWorldInPackage(ParentPackage) : nullptr;
	if (!ParentWorld)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: could not load parent map '%s'."), *ParentPackageName.ToString());
		return false;
	}

	// Bring the map up just far enough to spawn into it (same approach editor commandlets use), without opening it.
	const bool bWasInitialized = ParentWorld->bIsWorldInitialized;
	if (!bWasInitialized)
	{
		ParentWorld->AddToRoot();
		ParentWorld->WorldType = EWorldType::Editor;
		ParentWorld->InitWorld(UWorld::InitializationValues()
			.InitializeScenes(false).AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
			.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false).EnableTraceCollision(false)
			.SetTransactional(false).CreateFXSystem(false));
	}

	ULevel* ParentPersistent = ParentWorld->PersistentLevel.Get();
	bool bSuccess = false;

	if (ParentPersistent)
	{
		// Already has a leader? Then only the follower conversion is needed, nothing to create or save.
		bool bHasLeader = false;
		for (AActor* Existing : ParentPersistent->Actors)
		{
			const AAnimOffsetAnim* Anchor = Cast<AAnimOffsetAnim>(Existing);
			if (Anchor && !Anchor->bIsFollower)
			{
				bHasLeader = true;
				break;
			}
		}

		if (bHasLeader)
		{
			bSuccess = true;
		}
		else
		{
			// Our transform is in this sublevel's own space; the parent may stream it with an offset.
			FTransform LeaderTransform = SublevelTransform;
			for (ULevelStreaming* Streaming : ParentWorld->GetStreamingLevels())
			{
				if (Streaming && Streaming->GetWorldAssetPackageFName() == ThisPackage)
				{
					LeaderTransform = SublevelTransform * Streaming->LevelTransform;
					break;
				}
			}

			FActorSpawnParameters Params;
			Params.OverrideLevel = ParentPersistent;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

			bSuppressCreationHooks = true;
			AAnimOffsetAnim* Leader = ParentWorld->SpawnActor<AAnimOffsetAnim>(GetClass(), LeaderTransform, Params);
			bSuppressCreationHooks = false;

			if (Leader)
			{
				Leader->TargetCamera = TargetCamera;
				Leader->bMoveCameraOnSequenceOpen = bMoveCameraOnSequenceOpen;
				Leader->bKeepWorldOnFirstParent = bKeepWorldOnFirstParent;
				Leader->bPromptToBakeOnUnparent = bPromptToBakeOnUnparent;
				Leader->bAutoCreateSublevelFollowers = bAutoCreateSublevelFollowers;
				Leader->SetActorLabel(TEXT("AnimOffsetAnim"));
				ParentPackage->MarkPackageDirty();

				bSuccess = FEditorFileUtils::PromptForCheckoutAndSave({ ParentPackage }, false, false) != FEditorFileUtils::PR_Failure;
				UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: created leader in '%s' (saved=%d)."), *ParentPackageName.ToString(), bSuccess);
			}
		}
	}

	if (!bWasInitialized)
	{
		ParentWorld->CleanupWorld();
		ParentWorld->RemoveFromRoot();
	}
	return bSuccess;
}


// ---------------------------------------------------------------------------------------------
// Outliner drop: a sublevel actor dropped on the leader is parented to the follower of its own level.
//
// Unreal rejects that drop (actors can only be parented inside one level), so the drop itself does nothing.
// A Slate pre-processor sees the mouse release while the drag is still alive, works out which Outliner row is under the cursor,
// and, if that row is a leader anchor, queues the parenting for the next tick. One listener is shared by all anchors.
// ---------------------------------------------------------------------------------------------

namespace
{
	/** The actors being dragged out of the World Outliner (empty for any other drag, e.g. an asset). */
	void GatherDraggedActors(const TSharedPtr<FDragDropOperation>& Operation, TArray<TWeakObjectPtr<AActor>>& OutActors)
	{
		if (!Operation.IsValid())
		{
			return;
		}

		TSharedPtr<FActorDragDropOp> ActorOp;
		if (Operation->IsOfType<FSceneOutlinerDragDropOp>())
		{
			ActorOp = StaticCastSharedPtr<FSceneOutlinerDragDropOp>(Operation)->GetSubOp<FActorDragDropOp>();
		}
		else if (Operation->IsOfType<FActorDragDropOp>())
		{
			ActorOp = StaticCastSharedPtr<FActorDragDropOp>(Operation);
		}

		if (ActorOp.IsValid())
		{
			for (const TWeakObjectPtr<AActor>& Dragged : ActorOp->Actors)
			{
				if (Dragged.IsValid())
				{
					OutActors.Add(Dragged);
				}
			}
		}
	}

	/** Text of every STextBlock in the widget's subtree (the Outliner row's label is one of them). */
	void CollectTexts(SWidget& Widget, TArray<FString>& OutTexts)
	{
		if (Widget.GetType() == FName(TEXT("STextBlock")))
		{
			OutTexts.Add(static_cast<STextBlock&>(Widget).GetText().ToString());
		}
		if (FChildren* Children = Widget.GetChildren())
		{
			Children->ForEachWidget([&OutTexts](SWidget& Child) { CollectTexts(Child, OutTexts); });
		}
	}

	/**
	 * Which leader's Outliner row is under the cursor, if any.
	 * Walks up from the widget under the cursor to the first ancestor that contains an actor's label. If that is a row it holds exactly
	 * one label (its own). If the cursor is over empty space, that ancestor is the whole list and holds many, which is rejected.
	 */
	AAnimOffsetAnim* FindLeaderUnderCursor(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent, UWorld* World,
		const TArray<AAnimOffsetAnim*>& Leaders)
	{
		FWidgetPath Path = SlateApp.LocateWindowUnderMouse(
			MouseEvent.GetScreenSpacePosition(), SlateApp.GetInteractiveTopLevelWindows(), false, MouseEvent.GetUserIndex());
		if (!Path.IsValid())
		{
			return nullptr;
		}

		TSet<FString> ActorLabels;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			ActorLabels.Add(It->GetActorLabel());
		}

		for (int32 Index = Path.Widgets.Num() - 1; Index >= 0; --Index)
		{
			TArray<FString> Texts;
			CollectTexts(Path.Widgets[Index].Widget.Get(), Texts);

			TSet<FString> Matched;
			for (const FString& Text : Texts)
			{
				if (ActorLabels.Contains(Text))
				{
					Matched.Add(Text);
				}
			}

			if (Matched.Num() == 0)
			{
				continue; // keep climbing
			}
			if (Matched.Num() > 1)
			{
				return nullptr; // the whole list, so the cursor is not on one row
			}

			const FString& RowLabel = *Matched.CreateConstIterator();
			for (AAnimOffsetAnim* Leader : Leaders)
			{
				if (Leader->GetActorLabel() == RowLabel)
				{
					return Leader;
				}
			}
			return nullptr; // some other actor's row
		}
		return nullptr;
	}

	class FOutlinerDropProcessor : public IInputProcessor
	{
	public:
		virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

		virtual const TCHAR* GetDebugName() const override { return TEXT("AnimOffsetAnimOutlinerDrop"); }

		virtual bool HandleMouseButtonUpEvent(FSlateApplication& SlateApp, const FPointerEvent& MouseEvent) override
		{
			if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !SlateApp.IsDragDropping())
			{
				return false;
			}

			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			if (!World)
			{
				return false;
			}

			TArray<TWeakObjectPtr<AActor>> Dragged;
			GatherDraggedActors(SlateApp.GetDragDroppingContent(), Dragged);

			// Anchors are never reparented by this feature.
			Dragged.RemoveAll([World](const TWeakObjectPtr<AActor>& Weak)
				{
					const AActor* Actor = Weak.Get();
					return !Actor || Actor->GetWorld() != World || !Actor->GetLevel() || Actor->IsA<AAnimOffsetAnim>();
				});

			// Only interesting when at least one dragged actor is in a sublevel: that is the case Unreal rejects.
			const bool bAnySublevelActor = Dragged.ContainsByPredicate([](const TWeakObjectPtr<AActor>& Weak)
				{
					return !Weak->GetLevel()->IsPersistentLevel();
				});
			if (!bAnySublevelActor)
			{
				return false;
			}

			TArray<AAnimOffsetAnim*> Leaders;
			for (TActorIterator<AAnimOffsetAnim> It(World); It; ++It)
			{
				AAnimOffsetAnim* Anchor = *It;
				if (IsValid(Anchor) && !Anchor->IsTemplate() && !Anchor->bIsFollower
					&& Anchor->GetLevel() && Anchor->GetLevel()->IsPersistentLevel())
				{
					Leaders.Add(Anchor);
				}
			}
			if (Leaders.Num() == 0)
			{
				return false;
			}

			AAnimOffsetAnim* Target = FindLeaderUnderCursor(SlateApp, MouseEvent, World, Leaders);
			if (!Target)
			{
				return false;
			}

			UE_LOG(LogTemp, Log, TEXT("CameraAnchor: %d actor(s) dropped on '%s' in the Outliner, parenting them to the sublevel anchors."),
				Dragged.Num(), *Target->GetActorLabel());

			// Run after Slate has finished this (rejected) drop, not in the middle of it.
			TWeakObjectPtr<AAnimOffsetAnim> WeakTarget(Target);
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakTarget, Dragged](float)
				{
					if (AAnimOffsetAnim* Leader = WeakTarget.Get())
					{
						Leader->ParentDroppedActors(Dragged);
					}
					return false; // one shot
				}), 0.0f);

			return false; // never consume the event: Slate still finishes its own drop handling
		}
	};

	TSharedPtr<FOutlinerDropProcessor> GOutlinerDropProcessor;
	int32 GOutlinerDropUsers = 0;
}


void AAnimOffsetAnim::RegisterOutlinerDropHook()
{
	if (HasAnyFlags(RF_ClassDefaultObject) || bOutlinerDropHookRegistered || !FSlateApplication::IsInitialized())
	{
		return;
	}
	bOutlinerDropHookRegistered = true;

	if (++GOutlinerDropUsers == 1)
	{
		GOutlinerDropProcessor = MakeShared<FOutlinerDropProcessor>();
		FSlateApplication::Get().RegisterInputPreProcessor(GOutlinerDropProcessor);
	}
}


void AAnimOffsetAnim::UnregisterOutlinerDropHook()
{
	if (!bOutlinerDropHookRegistered)
	{
		return;
	}
	bOutlinerDropHookRegistered = false;

	if (--GOutlinerDropUsers <= 0)
	{
		GOutlinerDropUsers = 0;
		if (GOutlinerDropProcessor.IsValid())
		{
			if (FSlateApplication::IsInitialized())
			{
				FSlateApplication::Get().UnregisterInputPreProcessor(GOutlinerDropProcessor);
			}
			GOutlinerDropProcessor.Reset();
		}
	}
}


AAnimOffsetAnim* AAnimOffsetAnim::FindAnchorInLevel(ULevel* Level) const
{
	if (!Level)
	{
		return nullptr;
	}
	for (AActor* Existing : Level->Actors)
	{
		AAnimOffsetAnim* Anchor = Cast<AAnimOffsetAnim>(Existing);
		if (Anchor && Anchor != this && IsValid(Anchor) && !Anchor->IsTemplate())
		{
			return Anchor;
		}
	}
	return nullptr;
}


void AAnimOffsetAnim::ParentDroppedActors(const TArray<TWeakObjectPtr<AActor>>& Actors)
{
	UWorld* World = GetWorld();
	if (!GEditor || !World || World->IsGameWorld() || bIsFollower || !GetLevel() || !GetLevel()->IsPersistentLevel())
	{
		return;
	}

	const FScopedTransaction Transaction(NSLOCTEXT("CameraAnchor", "ParentDropped", "Parent To Sublevel Anchor"));

	int32 Parented = 0;
	for (const TWeakObjectPtr<AActor>& Weak : Actors)
	{
		AActor* Actor = Weak.Get();
		if (!IsValid(Actor) || Actor->IsA<AAnimOffsetAnim>() || !IsActorInLiveLevel(Actor))
		{
			continue;
		}

		// Same level as the leader: the normal parent. Sublevel: the anchor that lives in the actor's own level.
		AAnimOffsetAnim* Target = nullptr;
		if (Actor->GetLevel() == GetLevel())
		{
			Target = this;
		}
		else
		{
			Target = FindAnchorInLevel(Actor->GetLevel());
			if (!Target && bAutoCreateSublevelFollowers)
			{
				EnsureSublevelFollowers(true);
				Target = FindAnchorInLevel(Actor->GetLevel());
			}
		}

		if (!Target)
		{
			UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: '%s' was not parented: its level has no anchor (is the level visible and unlocked?)."),
				*Actor->GetActorLabel());
			continue;
		}
		if (Actor->GetAttachParentActor() == Target || Target->IsAttachedTo(Actor))
		{
			continue; // already there, or it would be a cycle
		}

		Target->SyncToLeader(); // make sure a follower is exactly on the leader before keeping the world transform
		Actor->Modify();

		// Same as an Outliner parent: keep where it is in the world. Keys of an animated actor are re-expressed by the
		// anchor's Sequencer watcher (bKeepWorldOnFirstParent), exactly as when the artist parents it by hand.
		if (Actor->AttachToActor(Target, FAttachmentTransformRules::KeepWorldTransform))
		{
			++Parented;
		}
	}

	if (Parented > 0)
	{
		FNotificationInfo Info(FText::Format(
			NSLOCTEXT("CameraAnchor", "ParentedNotice", "Camera Anchor: parented {0} actor(s) to the anchor of their level."),
			FText::AsNumber(Parented)));
		Info.ExpireDuration = 4.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}


void AAnimOffsetAnim::CreateSublevelFollowers()
{
	EnsureSublevelFollowers(true);
}


void AAnimOffsetAnim::EnsureSublevelFollowers(bool bManual)
{
	UWorld* World = GetWorld();
	ULevel* MyLevel = GetLevel();

	// Only a real leader in the persistent level of the editor world: not a follower, not a drag-preview / template actor.
	const bool bCanLead = GEditor && World && !World->IsGameWorld()
		&& !bIsFollower && !IsTemplate() && !HasAnyFlags(RF_Transient)
		&& MyLevel && MyLevel->IsPersistentLevel();

	if (!bCanLead)
	{
		if (bManual)
		{
			UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: sublevel followers can only be created from an anchor placed in the persistent level."));
		}
		return;
	}

	int32 Created = 0;

	for (ULevelStreaming* Streaming : World->GetStreamingLevels())
	{
		ULevel* Level = Streaming ? Streaming->GetLoadedLevel() : nullptr;
		if (!Level || !Level->bIsVisible || FLevelUtils::IsLevelLocked(Level))
		{
			continue;
		}

		// One anchor per sublevel is enough. An anchor the artist placed there themselves is left alone (it works on its own).
		bool bHasAnchor = false;
		for (AActor* Existing : Level->Actors)
		{
			if (Existing && Existing->IsA<AAnimOffsetAnim>())
			{
				bHasAnchor = true;
				break;
			}
		}
		if (bHasAnchor)
		{
			continue;
		}

		FActorSpawnParameters Params;
		Params.OverrideLevel = Level; // spawn INTO the sublevel, so actors there can be parented to it
		Params.ObjectFlags = RF_Transactional;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

		AAnimOffsetAnim* Follower = World->SpawnActor<AAnimOffsetAnim>(GetClass(), GetActorTransform(), Params);
		if (!Follower)
		{
			continue;
		}

		Follower->bIsFollower = true;
		Follower->SetActorLabel(FString::Printf(TEXT("%s_Follower_%s"), *GetActorLabel(), *Streaming->GetWorldAssetPackageFName().ToString()));
		Follower->MarkPackageDirty();
		++Created;

		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: created follower '%s' in sublevel '%s'."),
			*Follower->GetActorLabel(), *Streaming->GetWorldAssetPackageFName().ToString());
	}

	if (bManual && Created == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("CameraAnchor: no sublevel needed a follower (all loaded, visible, unlocked sublevels already have an anchor)."));
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
#endif // WITH_EDITOR
