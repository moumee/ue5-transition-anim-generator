using UnrealBuildTool;

public class MotionInbetweeningHello : ModuleRules
{
	public MotionInbetweeningHello(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.Add("Core");
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"UnrealEd",
			"AssetTools",
			"AssetRegistry",
			"Slate",
			"SlateCore",
			"ToolMenus",
			"ContentBrowser",
			"DesktopPlatform",
			"DeveloperSettings",
			"Settings",
			"Projects",
			"Json",
			"PythonScriptPlugin"
		});
		RuntimeDependencies.Add("$(PluginDir)/Scripts/*.py", StagedFileType.NonUFS);
	}
}
