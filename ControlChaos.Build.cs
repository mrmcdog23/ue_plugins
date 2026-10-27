// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ControlChaos : ModuleRules
{
	public ControlChaos(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicIncludePaths.AddRange(
            new string[] {
                System.IO.Path.GetFullPath(Target.RelativeEnginePath) + "/Source/Editor/Sequencer/Public/"
                }
            );


        PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);


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


        DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
