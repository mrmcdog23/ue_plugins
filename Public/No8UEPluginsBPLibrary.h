// Copyright No8 Sound and Vision. All Rights Reserved.

#pragma once
#include "MovieScene.h"
#include "MovieSceneBindingProxy.h"
#include "LevelSequenceActor.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "No8UEPluginsBPLibrary.generated.h"


UCLASS()
class UNo8UEPluginsBPLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

public:
	/** Send a toast msg to the user */
	UFUNCTION(BlueprintCallable, Category = "No8|EditorScripting")
	static void PopupMessage(FString Message, float Duration, bool bSuccess);


	UFUNCTION(CallInEditor, Category = "No8|EditorScripting")
	static FString GetFtrackIdFromBinding(const FMovieSceneBinding& Binding, const ALevelSequenceActor* SequenceActor);

	UFUNCTION(BlueprintCallable, Category = "No8|EditorScripting")
	static FString GetFtrackId(const FString AssetPath);

};


DECLARE_LOG_CATEGORY_EXTERN(LogTTFTools, Log, All);
