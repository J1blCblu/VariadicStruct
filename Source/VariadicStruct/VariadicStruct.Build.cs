// Copyright 2024 Ivan Baktenkov. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class VariadicStruct : ModuleRules
	{
		public VariadicStruct(ReadOnlyTargetRules Target) : base(Target)
		{
			DefaultBuildSettings = BuildSettingsVersion.V5;
			IWYUSupport = IWYUSupport.Full;

			PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", });

			// StructUtils were migrated to CoreUObject in 5.5.0.
			if (Target.Version.MajorVersion == 5 && Target.Version.MinorVersion < 5)
			{
				PublicDependencyModuleNames.Add("StructUtils");
			}

			// Compile out Engine dependent code, like NetSerialize(), using #if WITH_ENGINE.
			if (Target.bCompileAgainstEngine)
			{
				PrivateDependencyModuleNames.Add("Engine");
			}
		}
	}
}
