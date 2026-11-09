using UnrealBuildTool;

namespace UnrealBuildTool.Rules
{
public class LightUSDUE : ModuleRules
{
    public LightUSDUE(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        CppStandard = CppStandardVersion.Default;
        bEnableExceptions = false;

        PublicDependencyModuleNames.AddRange(new[] {
            "Core", "CoreUObject", "Engine", "Projects"
        });

        PrivateDependencyModuleNames.AddRange(new[] {
            "AssetTools", "UnrealEd", "DeveloperSettings", "Json",
            "JsonUtilities", "UnrealUSDWrapper", "USDUtilities", "USDClasses",
            "USDSchemas", "USDStage",
            "USDStageImporter", "USDExporter", "PythonScriptPlugin", "HairStrandsCore",
            "HairStrandsEditor", "ControlRig", "ModelContextProtocolEngine",
            "ModelContextProtocolEditor"
        });

        string ThirdParty = System.IO.Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", Target.Platform.ToString());
        PublicIncludePaths.Add(System.IO.Path.Combine(ThirdParty, "include"));
        PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "lightusd_c.lib"));
        PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "lightusd_next.lib"));
        PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "tydra_next.lib"));
        if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Clear();
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "liblightusd_c.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "libtydra_next.a"));
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "liblightusd_next.a"));
            PublicSystemLibraries.AddRange(new[] { "dl", "pthread" });
        }
        else if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            // Windows uses the stable C ABI DLL.  The DLL contains the
            // LightUSD/Tydra implementation, so do not link MinGW-built C++
            // static archives into the MSVC/Unreal module.
            PublicAdditionalLibraries.Clear();
            PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "lightusd_c.lib"));
            RuntimeDependencies.Add(System.IO.Path.Combine("$(PluginDir)", "Binaries", "Win64", "lightusd_c.dll"));
        }
        PublicDefinitions.Add("LIGHTUSD_UE_STATIC_C_API=1");
    }
}
}
