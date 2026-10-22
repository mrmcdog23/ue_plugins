// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class testing : ModuleRules
{
	public testing(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
            {
            "Core", "CoreUObject", "Engine", "InputCore", "LevelSequence", "MovieScene"
            });

        PrivateDependencyModuleNames.AddRange(
            new string[]
            {
                "UnrealEd",
                "CoreUObject",
                "Engine",
                "Slate",
                "SlateCore",
            }
        );
        PrivateIncludePaths.AddRange(
            new string[] {
                System.IO.Path.GetFullPath(Target.RelativeEnginePath) + "/Source/Editor/Blutility/Private/",
                System.IO.Path.GetFullPath(Target.RelativeEnginePath) + "/Plugins/MovieScene/LevelSequenceEditor/Source/LevelSequenceEditor/",
                System.IO.Path.GetFullPath(Target.RelativeEnginePath) + "/Plugins/MovieScene/LevelSequenceEditor/Source/LevelSequenceEditor/Private/",
                System.IO.Path.GetFullPath(Target.RelativeEnginePath) + "/Plugins/Interchange/Runtime/Source/Parsers/Fbx/Private/",
                System.IO.Path.GetFullPath(Target.RelativeEnginePath) + "/Plugins/MovieScene/SequencerScripting/Source/SequencerScriptingEditor/Private/",
                System.IO.Path.GetFullPath(Target.RelativeEnginePath) + "/Plugins/Experimental/ImagePlate/Source/ImagePlate/Private/"

            }
            );

        // To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
    }
}
