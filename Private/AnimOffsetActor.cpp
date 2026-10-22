// Fill out your copyright notice in the Description page of Project Settings.
#include "AnimOffsetActor.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimSequenceBase.h"


// Sets default values
AAnimOffsetActor::AAnimOffsetActor()
{
 	// Set this actor to call Tick() every frame.  You can turn this off to improve performance if you don't need it.
	PrimaryActorTick.bCanEverTick = false;

	// Separate root so the mesh can be shifted without moving the actor.
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Mesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Root);

}

// Called when the game starts or when spawned
void AAnimOffsetActor::BeginPlay()
{
	Super::BeginPlay();
	
}

// Called every frame
void AAnimOffsetActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

}

