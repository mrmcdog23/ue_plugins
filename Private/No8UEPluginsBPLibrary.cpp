// Copyright No8 Sound and Vision. All Rights Reserved.

#include "No8UEPluginsBPLibrary.h"
PRAGMA_DISABLE_DEPRECATION_WARNINGS
#include "LevelSequence.h"

#include "LevelSequenceActor.h"
PRAGMA_ENABLE_DEPRECATION_WARNINGS

DEFINE_LOG_CATEGORY(LogTTFTools);
#include "Subsystems/EditorAssetSubsystem.h"
#include <Widgets/Notifications/SNotificationList.h>
#include <Framework/Notifications/NotificationManager.h>


UNo8UEPluginsBPLibrary::UNo8UEPluginsBPLibrary(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
{
}


/**
 * Send a toast msg to the user in popup form
 *
 * @param Message The text to display as the main message.
 * @param Duration The amount of time to display.
 * @param bSuccess It the task is a success or fail
 */
void UNo8UEPluginsBPLibrary::PopupMessage(FString Message, float Duration, bool bSuccess)
{
    // Send Editor Notification
    FNotificationInfo ToastNotification(FText::GetEmpty());
    ToastNotification.bFireAndForget = true;
    ToastNotification.ExpireDuration = Duration;
    ToastNotification.bUseThrobber = false;
    ToastNotification.Text = FText::FromString(Message);
    if (bSuccess)
    {
        ToastNotification.Image = FAppStyle::GetBrush(TEXT("Symbols.Check"));
    }
    else
    {
        ToastNotification.Image = FAppStyle::GetBrush(TEXT("Icons.Error"));
    }
    FSlateNotificationManager::Get().AddNotification(ToastNotification);
}


FString UNo8UEPluginsBPLibrary::GetFtrackIdFromBinding(const FMovieSceneBinding& Binding, const ALevelSequenceActor* SequenceActor)
{
    const FString Test = "tesyting";


    // This will store the located objects
    TArray<UObject*, TInlineAllocator<1>> FoundObjects;
    //ULevelSequence* LevelSequence = Cast<ULevelSequence>(SequenceActor->GetSequence());
    // Locate the bound objects using the GUID

    //FGuid BindingGuid = Binding.GetObjectGuid();

    //LevelSequence->LocateBoundObjects(BindingGuid, SequenceActor, FoundObjects);
    /*
    UE_LOG(LogTemp, Log, TEXT("Binding Name: %s"), *Binding.GetName());

    for (UObject* BoundObj : BoundObjects)
    {
        if (AActor* Actor = Cast<AActor>(BoundObj))
        {
            UE_LOG(LogTemp, Log, TEXT(" - Bound Actor: %s"), *Actor->GetName());
        }
        else
        {
            UE_LOG(LogTemp, Log, TEXT(" - Bound Object: %s"), *BoundObj->GetName());
        }

    */
    return Test;
}


FString UNo8UEPluginsBPLibrary::GetFtrackId(const FString AssetPath)
{
    FString Test;
    // Get the asset subsystem
    UEditorAssetSubsystem* EditorAssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
    if (!EditorAssetSubsystem)
        return Test;

    // Load the asset
    UObject* Asset = EditorAssetSubsystem->LoadAsset(AssetPath);
    if (!Asset) {
        UE_LOG(LogTemp, Warning, TEXT("Could not load asset at path: %s"), *AssetPath);
        return Test;
    }
    // Example: Metadata key you want to get
    FName Key = TEXT("No8.ftrack_id");
    return EditorAssetSubsystem->GetMetadataTag(Asset, Key);
}