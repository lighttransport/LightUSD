using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
public class LightUSDUEPython : ModuleRules
{
    public LightUSDUEPython(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Default;
        bEnableExceptions = false;

        // Deliberately no USD, HairStrands, ControlRig, MCP, or LightUSD C++
        // dependency.  The content/Python bridge owns the integration.
        PrivateDependencyModuleNames.AddRange(new[] {
            "Core", "Projects", "PythonScriptPlugin"
        });
    }
}
}
