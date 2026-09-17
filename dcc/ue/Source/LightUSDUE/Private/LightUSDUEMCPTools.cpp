#include "LightUSDUEMCPTools.h"

#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"

FString ULightUSDUEMCPTools::LightUSDRunPython(const FString& Code)
{
    IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
    if (!Python)
    {
        return TEXT("{\"succeeded\":false,\"error\":\"PythonScriptPlugin is unavailable\"}");
    }

    if (!Python->IsPythonInitialized() && !Python->ForceEnablePythonAtRuntime())
    {
        return TEXT("{\"succeeded\":false,\"error\":\"Python could not be initialized\"}");
    }

    FPythonCommandEx Command;
    const FString EscapedCode = Code.ReplaceCharWithEscapedChar();
    Command.Command = FString::Printf(
        TEXT("(exec(\"%s\") or str(globals().get('_lightusd_mcp_result', '')))"),
        *EscapedCode);
    Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;

    if (!Python->ExecPythonCommandEx(Command))
    {
        return TEXT("{\"succeeded\":false,\"error\":\"Unreal Python execution failed; see the UE log\"}");
    }

    // ExecPythonCommandEx returns repr() for string results, so remove only the
    // wrapper quotes while leaving JSON content untouched.
    bool bRemoved = false;
    Command.CommandResult.TrimCharInline(FString::ElementType('\''), &bRemoved);
    return Command.CommandResult;
}
