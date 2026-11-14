using UnrealBuildTool;
public class MotionTransitionRuntime : ModuleRules
{
    public MotionTransitionRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine" });
    }
}
