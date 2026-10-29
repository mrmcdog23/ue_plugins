// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AnimOffsetAnim.generated.h"

class ACameraActor;
class UArrowComponent;
class UMovieScene;
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

	/** Parent cameras bound in the Sequencer under this actor whenever a Sequencer is opened (runtime: sequence-spawned cameras). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor")
	bool bMoveCameraOnSequenceOpen = true;

	/**
	 * When an actor is parented under this actor for the first time, keep it where it is in the world instead of offsetting it.
	 * - static actors: attached keeping their world transform.
	 * - animated actors: their existing keys are re-expressed relative to this actor (at the existing key times only),
	 *   so the animation plays back in the same world position as before.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor")
	bool bKeepWorldOnFirstParent = true;

	/**
	 * When the artist unparents an actor (camera, cube, anything bound in the open Sequencer, spawned or possessed)
	 * from this actor, offer to fix it up:
	 * - animated actors: ask whether to bake the animation (as it looks under this actor) into world space. Only the existing key times are keyed.
	 * - actors with no transform animation: ask whether to keep the transform it had under this actor.
	 * Static mesh actors parented under this actor get the keep-transform prompt even when they are not in a Sequencer.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor")
	bool bPromptToBakeOnUnparent = true;

protected:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UArrowComponent> Arrow;

	/**
	 * Sequencer bindings whose keys have already been converted to this actor's space.
	 * Saved with the level so a respawned camera is not converted a second time.
	 */
	UPROPERTY()
	TSet<FGuid> ConvertedBindings;

	/** Runtime (PIE / MRQ / game): parent sequence-spawned cameras under this actor. */
	void AttachRuntimeCameras();

#if WITH_EDITOR
	virtual void PostActorCreated() override;
	virtual void PostLoad() override;
	virtual void BeginDestroy() override;


private:
	/** An actor we have seen parented under this actor, per Sequencer binding. */
	struct FTrackedActor
	{
		TWeakObjectPtr<AActor> Actor;

		/** Last observed transform relative to this actor while it was parented. */
		FTransform LocalToAnchor = FTransform::Identity;
	};

	void RegisterSequencerHook();
	void UnregisterSequencerHook();
	void OnSequencerCreated(TSharedRef<ISequencer> InSequencer);

	/**
	 * Walks every actor bound in the focused sequence (spawnables and possessables alike).
	 * - Cameras are parented under this actor automatically.
	 * - Any bound actor the artist parented under this actor is remembered, and when it is later
	 *   unparented the artist is offered a bake / keep-transform prompt.
	 */
	void TrackBoundActors(ISequencer& InSequencer);

	/** True if the binding has a transform track with at least one key. */
	bool IsBindingAnimated(UMovieScene* MovieScene, const FGuid& BindingId) const;

	/**
	 * First parent of an animated actor: its keys currently hold world-space values.
	 * Re-expresses them relative to this actor at each existing key time (so nothing moves),
	 * then optionally attaches the actor.
	 */
	void ConvertKeysToAnchorSpace(ISequencer& InSequencer, const FGuid& BindingId, AActor* Actor, bool bAttach);

	/** Shows the popup: "bake?" for animated actors, "keep transform?" for static ones. */
	void HandleActorUnparented(ISequencer& InSequencer, const FGuid& BindingId, AActor* Actor, const FTransform& LocalToAnchor);

	/** Rewrites the binding's transform keys (at the existing key times only) so that, once unparented, the actor moves as it did under this actor. */
	void BakeActorToWorld(ISequencer& InSequencer, const FGuid& BindingId);

	/** Asks "keep the transform it had under this actor?" and, on Yes, moves the actor back to where it sat while parented. */
	void OfferKeepTransform(AActor* Actor, const FTransform& LocalToAnchor);

	/** Starts the Sequencer-independent poll that watches static meshes parented under this actor. */
	void StartChildWatcher();

	/** One poll of the child watcher. Returns false to stop ticking. */
	bool TickChildWatcher();

	FDelegateHandle SequencerCreatedHandle;

	/** Actors this actor has parented / seen parented, per Sequencer binding. Used to tell "user unparented it" from "freshly respawned". */
	TMap<FGuid, FTrackedActor> ParentedByBinding;

	/** Bindings the artist unparented: we must not re-attach these. */
	TSet<FGuid> OptedOutBindings;

	/** Bindings seen free-floating this session. If one later appears under us, the artist just parented it. */
	TSet<FGuid> SeenUnparented;

	bool bPromptOpen = false;

	/** A static mesh currently parented under this actor, and where it sits relative to us (last observed). */
	struct FWatchedChild
	{
		TWeakObjectPtr<AActor> Actor;
		FTransform LocalToAnchor = FTransform::Identity;
	};

	bool bChildWatcherStarted = false;

	/** Static meshes seen under this actor, whether or not they are in a Sequencer. */
	TArray<FWatchedChild> WatchedChildren;

	/** Actors the Sequencer path is already handling (bake / keep prompt), so the child watcher must not prompt for them too. */
	TArray<TWeakObjectPtr<AActor>> SequencerManaged;
#endif
};
