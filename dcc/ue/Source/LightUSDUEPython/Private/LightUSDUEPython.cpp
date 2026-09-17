#include "Modules/ModuleManager.h"

/**
 * Python-first LightUSD module.
 *
 * The useful API lives in Content/Python/lightusd_ue.  Keeping this module
 * intentionally empty avoids binding the plugin to UE's USD, HairStrands,
 * ControlRig, MCP, or LightUSD C++ ABI.  UBT still requires an editor module
 * so the plugin can be discovered and package Python content consistently.
 */
class FLightUSDUEPythonModule final : public IModuleInterface
{
};

IMPLEMENT_MODULE(FLightUSDUEPythonModule, LightUSDUEPython)
