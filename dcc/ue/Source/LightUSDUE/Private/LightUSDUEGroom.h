#pragma once

#include "CoreMinimal.h"

struct FLightUSDUEOptions;
struct FLightUSDUEResult;
struct lightusd_stage;
class UGroomAsset;
class UGroomCache;

namespace LightUSDUEGroom
{
bool ImportBasisCurves(lightusd_stage* Stage, const FLightUSDUEOptions& Options,
    FLightUSDUEResult& Result);
bool ExportGroom(UGroomAsset* Groom, const FString& Filename, FLightUSDUEResult& Result);
bool ExportGroomCache(UGroomCache* Cache, UGroomAsset* Groom, const FString& Filename,
    FLightUSDUEResult& Result);
}
