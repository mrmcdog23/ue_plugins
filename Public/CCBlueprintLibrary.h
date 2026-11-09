// Copyright No8 Sound and Vision. All Rights Reserved.

#pragma once
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CCBlueprintLibrary.generated.h"


UCLASS()
class UCCBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_UCLASS_BODY()

public:

	/** Send a toast msg to the user */
	UFUNCTION(BlueprintCallable, Category = "No8|EditorScripting")
	static void PopupMessage(FString Message, FString iconName, float Duration, bool bSuccess);

};


DECLARE_LOG_CATEGORY_EXTERN(LogTTFTools, Log, All);
