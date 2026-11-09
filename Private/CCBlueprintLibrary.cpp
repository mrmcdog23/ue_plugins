// Copyright No8 Sound and Vision. All Rights Reserved.

#include "CCBlueprintLibrary.h"
#include <Widgets/Notifications/SNotificationList.h>
#include <Framework/Notifications/NotificationManager.h>
#include "EngineUtils.h"
#include "Misc/OutputDevice.h"
#include "Misc/MessageDialog.h"
#include "Editor/EditorEngine.h"
#include "Styling/SlateStyle.h"
#include "Interfaces/IPluginManager.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "Misc/Paths.h"


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
void UCCBlueprintLibrary::PopupMessage(FString Message, FString iconName, float Duration, bool bSuccess)
{
    // Send Editor Notification
    FNotificationInfo ToastNotification(FText::GetEmpty());
    ToastNotification.bFireAndForget = true;
    ToastNotification.ExpireDuration = Duration;
    ToastNotification.bUseThrobber = false;
    ToastNotification.Text = FText::FromString(Message);

    TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ControlChaos"));
    if (Plugin.IsValid())
    {
        const FString IconPath = Plugin->GetBaseDir() / FString::Printf(TEXT("Resources/%s.png"), *iconName);
        UE_LOG(LogTemp, Warning, TEXT("Icon exists: %d"), FPaths::FileExists(IconPath));

        static TSharedPtr<FSlateDynamicImageBrush> CustomBrush;

        if (!CustomBrush.IsValid())
        {
            CustomBrush = MakeShared<FSlateDynamicImageBrush>(
                FName(*IconPath), FVector2D(32.f, 32.f));
        }

        ToastNotification.Image = CustomBrush.Get();
    }

    /*
    if (bSuccess)
    {
        ToastNotification.Image = &CustomBrush;
    }
    else
    {
        ToastNotification.Image = &CustomBrush;
    }
    */
    FSlateNotificationManager::Get().AddNotification(ToastNotification);
}



