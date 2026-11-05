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

	/** Runtime only (PIE / MRQ / game): parent sequence-spawned cameras under this actor. No longer auto-parents in the editor. */
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

	/**
	 * Unreal only lets actors be parented to an actor in the SAME level. So an anchor placed in the persistent level cannot
	 * be the parent of sublevel actors. Instead, the leader (an anchor in the persistent level) keeps one FOLLOWER anchor in each
	 * loaded sublevel. Followers copy the leader's world transform every frame; parent sublevel actors to the follower.
	 * Move / animate / key the LEADER only. A follower that is moved by hand snaps back to the leader.
	 *
	 * Editor only: automatically creates a follower in every loaded, visible, unlocked sublevel that has no anchor yet.
	 * Untick this if you delete a follower on purpose, otherwise it is created again.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera Anchor|Sublevels")
	bool bAutoCreateSublevelFollowers = true;

	/** Set on the anchors that are created in sublevels. They follow the leader and never parent runtime cameras. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera Anchor|Sublevels")
	bool bIsFollower = false;

#if WITH_EDITOR
	/** Creates a follower anchor in every loaded sublevel that has no anchor yet (the leader must be in the persistent level). */
	UFUNCTION(CallInEditor, Category = "Camera Anchor|Sublevels")
	void CreateSublevelFollowers();
#endif

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

	/** The leader a follower copies: the first non-follower anchor in the persistent level. Null if there is none. */
	AAnimOffsetAnim* FindLeader() const;

	/** Follower only: snap this actor to the leader's world transform (no-op if already there or no leader is found). */
	void SyncToLeader();

	TWeakObjectPtr<AAnimOffsetAnim> CachedLeader;
	double NextLeaderSearchTime = 0.0;

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

	/** Starts the poll that watches this Sequencer's bound actors (no-op if already watching it). */
	void StartSequencerPoll(TSharedRef<ISequencer> InSequencer);

	/** Finds a Sequencer that was already open before our creation hook was registered, and starts watching it. */
	void TryAdoptOpenSequencer();

	/**
	 * Walks every actor bound in the focused sequence (spawnables and possessables alike).
	 * - Nothing is parented automatically.
	 * - Any bound actor the artist parented under this actor is remembered, and when it is later
	 *   unparented the artist is offered a bake / keep-transform prompt.
	 * - A tracked spawnable camera that Sequencer respawns is re-attached to this actor.
	 */
	void TrackBoundActors(ISequencer& InSequencer);

	/** True if the binding has a transform track with at least one key. */
	bool IsBindingAnimated(UMovieScene* MovieScene, const FGuid& BindingId) const;

	/**
	 * The binding that actually carries this actor's transform animation: the given binding if it is keyed,
	 * otherwise any keyed binding of the actor or its root component. Invalid GUID if the actor is not animated.
	 */
	FGuid ResolveAnimatedBinding(ISequencer& InSequencer, UMovieScene* MovieScene, AActor* Actor, const FGuid& PreferredBinding) const;

	/** The Sequencer we are watching, so the child watcher can bake animated actors instead of only offering "keep transform". */
	TWeakPtr<ISequencer> ActiveSequencer;

	/**
	 * First parent of an animated actor: its keys currently hold world-space values.
	 * Re-expresses them relative to this actor at each existing key time (so nothing moves),
	 * then optionally attaches the actor.
	 */
	void ConvertKeysToAnchorSpace(ISequencer& InSequencer, const FGuid& BindingId, AActor* Actor, bool bAttach);

	/** An actor the artist just unparented from this actor, and where it sat relative to us. */
	struct FUnparentedItem
	{
		FGuid BindingId;
		TWeakObjectPtr<AActor> Actor;
		FTransform LocalToAnchor = FTransform::Identity;
	};

	/**
	 * Everything unparented in the same poll is handled together: one "bake?" prompt covering all animated actors
	 * (each is baked on its existing keys) and one "keep transform?" prompt covering all static ones.
	 */
	void HandleActorsUnparented(ISequencer& InSequencer, const TArray<FUnparentedItem>& Items);

	/** One "keep the transform they had under this actor?" prompt for several static actors. */
	void OfferKeepTransforms(const TArray<FUnparentedItem>& Items);

	/** Rewrites the binding's transform keys (at the existing key times only) so that, once unparented, the actor moves as it did under this actor. */
	void BakeActorToWorld(ISequencer& InSequencer, const FGuid& BindingId);

	/** Asks "keep the transform it had under this actor?" and, on Yes, moves the actor back to where it sat while parented. */
	void OfferKeepTransform(AActor* Actor, const FTransform& LocalToAnchor);

	/** Starts the Sequencer-independent poll that watches static meshes parented under this actor. */
	void StartChildWatcher();

	/**
	 * Editor only: if this anchor was just created by the artist in a sublevel (not a follower), delete it and
	 * make sure an anchor exists in the persistent level instead (re-using the existing leader if there is one).
	 */
	void RelocateToPersistentLevelIfNeeded();

	/**
	 * This map was opened on its own but is streamed by another map. Loads that parent map in the background, adds a leader
	 * anchor to ITS persistent level (at this anchor's transform, with the streaming offset applied), and saves it.
	 */
	bool CreateLeaderInParentMap(FName ParentPackageName, const FTransform& SublevelTransform);

	/** True while we spawn an anchor into a background-loaded map: skips the per-actor editor hooks / tickers. */
	static bool bSuppressCreationHooks;

	/** Starts a per-frame editor ticker that keeps a follower on its leader (editor viewports do not tick actors). */
	void StartFollowerSync();

	/** Leader only: spawn a follower into every loaded sublevel that has no anchor. bManual = called from the button. */
	void EnsureSublevelFollowers(bool bManual);

	bool bFollowerSyncStarted = false;
	double NextFollowerCheckTime = 0.0;

	/** One poll of the child watcher. Returns false to stop ticking. */
	bool TickChildWatcher();

	FDelegateHandle SequencerCreatedHandle;

	/** Actors this actor has parented / seen parented, per Sequencer binding. Used to tell "user unparented it" from "freshly respawned". */
	TMap<FGuid, FTrackedActor> ParentedByBinding;

	/** Bindings the artist unparented: we must not re-attach these. */
	TSet<FGuid> OptedOutBindings;

	/** Bindings seen free-floating this session. If one later appears under us, the artist just parented it. */
	TSet<FGuid> SeenUnparented;

	/** Shared by every anchor: only one bake / keep prompt may be open at a time, whichever anchor raised it. */
	static bool bPromptOpen;

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
