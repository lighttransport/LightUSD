#pragma once

#include "ModelContextProtocolEditorToolLibrary.h"

#include "LightUSDUEMCPTools.generated.h"

/** MCP bridge for driving the already-tested LightUSD Python facade from UE. */
UCLASS()
class LIGHTUSDUE_API ULightUSDUEMCPTools : public UModelContextProtocolEditorToolLibrary
{
    GENERATED_BODY()

public:
    /** Execute a short Unreal Python expression/script and return its textual result.
     * Set _lightusd_mcp_result in the script to return structured JSON to the MCP caller.
     */
    UFUNCTION(BlueprintCallable, Category="LightUSD|MCP")
    static FString LightUSDRunPython(const FString& Code);
};
