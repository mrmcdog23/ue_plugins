// Copyright No8 Sound and Vision. All Rights Reserved.

#include "CCBlueprintLibrary.h"
#include <Widgets/Notifications/SNotificationList.h>
#include <Framework/Notifications/NotificationManager.h>
#include "EngineUtils.h"
#include "Misc/OutputDevice.h"
#include "Misc/MessageDialog.h"
#include "Editor/EditorEngine.h"



UCCBlueprintLibrary::UCCBlueprintLibrary(const FObjectInitializer& ObjectInitializer)
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
void UCCBlueprintLibrary::PopupMessage(FString Message, float Duration, bool bSuccess)
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



