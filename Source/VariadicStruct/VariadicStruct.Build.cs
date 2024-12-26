// Copyright 2024 Ivan Baktenkov. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class VariadicStruct : ModuleRules
	{
		public VariadicStruct(ReadOnlyTargetRules Target) : base(Target)
		{
            DefaultBuildSettings = BuildSettingsVersion.V5;
            IWYUSupport = IWYUSupport.Full;

            PublicDependencyModuleNames.AddRange(
				new string[] {
					"Core",
					"CoreUObject",
					"StructUtils",
				}
			);

			//Compile out Engine dependent code, like NetSerialize(), using #if WITH_ENGINE.
			if (Target.bCompileAgainstEngine)
			{
				PrivateDependencyModuleNames.Add("Engine");
			}

			//I don't know what it is for...
			bAllowAutoRTFMInstrumentation = true;
		}
	}
}
