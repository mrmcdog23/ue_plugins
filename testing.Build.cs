// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class testing : ModuleRules
{
	public testing(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
            {
                "Core",
                "CoreUObject",
                "Engine", 
                "InputCore",
                "LevelSequence", 
                "MovieScene", 
                "Sequencer"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "UnrealEd",
                "CoreUObject",
                "Engine",
                "Slate",
                "SlateCore",
                "Sequencer",
            }
        );
        PrivateIncludePaths.AddRange(
            new string[] {}
            );

        PublicIncludePaths.AddRange(
            new string[] {
                System.IO.Path.GetFullPath(Target.RelativeEnginePath) + "/Source/Editor/Sequencer/Public/"
                }
            );


        // To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
    }
}
