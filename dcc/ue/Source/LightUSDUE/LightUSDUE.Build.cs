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
        PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "lightusd_core.lib"));
        PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "tydra.lib"));
        if (Target.Platform == UnrealTargetPlatform.Linux)
        {
            PublicAdditionalLibraries.Clear();
            foreach (string Library in new[] {
                "lightusd_render_c", "lightusd_c", "tydra", "lightusd_rb_solver",
                "lightusd_asset_import", "lightusd_core", "lightusd_obj_import",
                "lightusd_tinyvdb", "lightusd_core_c"
                // Note: lightusd_validation[_c] is staged by build_plugin.sh but
                // deliberately not linked: the module only calls the lightusd-c /
                // lightusd-render-c APIs, never the validation ABI.
            })
            {
                PublicAdditionalLibraries.Add(System.IO.Path.Combine(ThirdParty, "lib", "lib" + Library + ".a"));
            }
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
