#include "LightUSDUEGroom.h"

#include "LightUSDUE.h"
#include "HairAttributes.h"
#include "HairDescription.h"
#include "HairStrandsImporter.h"
#include "GroomBuilder.h"
#include "GroomBlueprintLibrary.h"
#include "GroomAsset.h"
#include "GroomCache.h"
#include "GroomBindingAsset.h"
#include "GroomImportOptions.h"
#include "GroomEdit.h"
#include "Math/Float16.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/MetaData.h"
#include <limits>

THIRD_PARTY_INCLUDES_START
extern "C" {
#include "lightusd-c.h"
}
THIRD_PARTY_INCLUDES_END

namespace
{
static FString SV(lightusd_sv Value)
{
    return FString(Value.data ? UTF8_TO_TCHAR(Value.data) : TEXT(""));
}

struct FArrayView
{
    const void* Data = nullptr;
    uint64 Count = 0;
    uint32 Components = 0;
    uint32 ComponentSize = 0;
    uint8 Storage = 0;
};

static bool GetArray(lightusd_prim Prim, const char* Name, FArrayView& Out)
{
    lightusd_value_view View = {};
    if (lightusd_attr_get(Prim, Name, &View) != LIGHTUSD_OK || !View.is_array || !View.data)
        return false;
    Out.Data = View.data;
    Out.Count = View.count;
    Out.Components = View.components;
    Out.ComponentSize = static_cast<uint32>(View.nbytes / (View.count * View.components));
    Out.Storage = View.storage;
    return true;
}

static bool GetExactTimeSample(lightusd_prim Prim, const char* Name, double TimeCode,
    FArrayView& Out)
{
    const size_t SampleCount = lightusd_attr_timesample_count(Prim, Name);
    if (SampleCount == 0)
        return false;
    TArray<double> Times;
    Times.SetNum(SampleCount);
    if (lightusd_attr_timesample_times(Prim, Name, Times.GetData(), SampleCount) != LIGHTUSD_OK)
        return false;
    for (size_t SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
    {
        if (!FMath::IsNearlyEqual(Times[SampleIndex], TimeCode, 1.e-6))
            continue;
        lightusd_value_view View = {};
        if (lightusd_attr_timesample_at(Prim, Name, SampleIndex, nullptr, &View) != LIGHTUSD_OK ||
            !View.is_array || !View.data)
            return false;
        Out = { View.data, View.count, View.components,
            static_cast<uint32>(View.nbytes / (View.count * View.components)), View.storage };
        return true;
    }
    return false;
}

static double Number(const FArrayView& View, uint64 Index)
{
    const uint8* Bytes = static_cast<const uint8*>(View.Data) + Index * View.ComponentSize;
    if (View.Storage == LIGHTUSD_COMP_FLOAT16)
        return static_cast<float>(*reinterpret_cast<const FFloat16*>(Bytes));
    if (View.Storage == LIGHTUSD_COMP_FLOAT64) return *reinterpret_cast<const double*>(Bytes);
    if (View.Storage == LIGHTUSD_COMP_FLOAT32) return *reinterpret_cast<const float*>(Bytes);
    if (View.Storage == LIGHTUSD_COMP_INT32) return *reinterpret_cast<const int32*>(Bytes);
    if (View.Storage == LIGHTUSD_COMP_INT64) return static_cast<double>(*reinterpret_cast<const int64*>(Bytes));
    if (View.Storage == LIGHTUSD_COMP_UINT32) return *reinterpret_cast<const uint32*>(Bytes);
    if (View.Storage == LIGHTUSD_COMP_UINT64) return static_cast<double>(*reinterpret_cast<const uint64*>(Bytes));
    return 0.0;
}

static void StabilizeDegenerateBounds(FArrayView& Points, TArray<FVector3f>& OwnedPoints,
    FLightUSDUEResult& Result)
{
    if (Points.Count == 0 || Points.Components < 3)
        return;

    FVector3f MinPoint(FLT_MAX);
    FVector3f MaxPoint(-FLT_MAX);
    for (uint64 PointIndex = 0; PointIndex < Points.Count; ++PointIndex)
    {
        const uint64 Base = PointIndex * Points.Components;
        const FVector3f Point(static_cast<float>(Number(Points, Base)),
            static_cast<float>(Number(Points, Base + 1)),
            static_cast<float>(Number(Points, Base + 2)));
        MinPoint.X = FMath::Min(MinPoint.X, Point.X);
        MinPoint.Y = FMath::Min(MinPoint.Y, Point.Y);
        MinPoint.Z = FMath::Min(MinPoint.Z, Point.Z);
        MaxPoint.X = FMath::Max(MaxPoint.X, Point.X);
        MaxPoint.Y = FMath::Max(MaxPoint.Y, Point.Y);
        MaxPoint.Z = FMath::Max(MaxPoint.Z, Point.Z);
    }

    const FVector3f Extent = MaxPoint - MinPoint;
    const bool bDegenerate = Extent.X <= KINDA_SMALL_NUMBER ||
        Extent.Y <= KINDA_SMALL_NUMBER || Extent.Z <= KINDA_SMALL_NUMBER;
    if (!bDegenerate)
        return;

    // UE 5.8's groom voxelizer indexes a zero-resolution axis. Keep the USD
    // values untouched, but use a tiny private copy for HairStrands import.
    OwnedPoints.SetNumUninitialized(Points.Count);
    for (uint64 PointIndex = 0; PointIndex < Points.Count; ++PointIndex)
    {
        const uint64 Base = PointIndex * Points.Components;
        OwnedPoints[static_cast<int32>(PointIndex)] = FVector3f(
            static_cast<float>(Number(Points, Base)),
            static_cast<float>(Number(Points, Base + 1)),
            static_cast<float>(Number(Points, Base + 2)));
    }
    const int32 StabilizedPoint = OwnedPoints.Num() > 1 ? 1 : 0;
    if (Extent.X <= KINDA_SMALL_NUMBER)
        OwnedPoints[StabilizedPoint].X += 1.e-3f;
    if (Extent.Y <= KINDA_SMALL_NUMBER)
        OwnedPoints[StabilizedPoint].Y += 1.e-3f;
    if (Extent.Z <= KINDA_SMALL_NUMBER)
        OwnedPoints[StabilizedPoint].Z += 1.e-3f;
    Points.Data = OwnedPoints.GetData();
    Points.Count = OwnedPoints.Num();
    Points.Components = 3;
    Points.ComponentSize = sizeof(float);
    Points.Storage = LIGHTUSD_COMP_FLOAT32;
    const FString Warning = TEXT("Degenerate groom bounds were epsilon-stabilized for UE HairStrands voxelization.");
    if (!Result.Warnings.Contains(Warning))
        Result.Warnings.Add(Warning);
}

static bool TessellateNurbs(const FArrayView& Points, const FArrayView& Counts,
    const FArrayView& Orders, const FArrayView& Knots, const FArrayView& Ranges,
    const FArrayView& Weights, TArray<FVector3f>& OutPoints, TArray<int32>& OutCounts)
{
    if (Points.Components != 3 || Orders.Count == 0 || Knots.Count == 0 ||
        Ranges.Components < 2)
        return false;
    uint64 PointOffset = 0;
    uint64 KnotOffset = 0;
    for (uint64 Curve = 0; Curve < Counts.Count; ++Curve)
    {
        const int32 ControlCount = static_cast<int32>(Number(Counts, Curve));
        const int32 Order = FMath::Clamp(static_cast<int32>(Number(Orders, Curve)), 2, ControlCount);
        if (ControlCount < Order || KnotOffset + static_cast<uint64>(ControlCount + Order) > Knots.Count)
            return false;
        const double Start = Number(Ranges, Curve * Ranges.Components);
        const double End = Number(Ranges, Curve * Ranges.Components + 1);
        const int32 Samples = FMath::Max(8, ControlCount * 4);
        OutCounts.Add(Samples);
        const uint64 CurveKnotOffset = KnotOffset;
        auto knot = [&](int32 Index) { return Number(Knots, CurveKnotOffset + static_cast<uint64>(Index)); };
        auto evaluate = [&](double Time)
        {
            int32 Span = Order - 1;
            for (int32 Index = Order - 1; Index < ControlCount; ++Index)
            {
                if (Time >= knot(Index) && (Time < knot(Index + 1) || Index == ControlCount - 1))
                {
                    Span = Index;
                    break;
                }
            }
            TArray<FVector4d> Work;
            Work.SetNum(Order);
            for (int32 J = 0; J < Order; ++J)
            {
                const int32 ControlIndex = Span - Order + 1 + J;
                const uint64 Base = (PointOffset + static_cast<uint64>(ControlIndex)) * Points.Components;
                const double Weight = Weights.Count > PointOffset + static_cast<uint64>(ControlIndex)
                    ? Number(Weights, PointOffset + static_cast<uint64>(ControlIndex)) : 1.0;
                Work[J] = FVector4d(Number(Points, Base) * Weight,
                    Number(Points, Base + 1) * Weight,
                    Number(Points, Base + 2) * Weight, Weight);
            }
            for (int32 R = 1; R < Order; ++R)
            {
                for (int32 J = Order - 1; J >= R; --J)
                {
                    const int32 KnotIndex = Span - Order + 1 + J;
                    const double Denominator = knot(KnotIndex + Order - R) - knot(KnotIndex);
                    const double Alpha = Denominator != 0.0
                        ? (Time - knot(KnotIndex)) / Denominator : 0.0;
                    Work[J] = Work[J - 1] * (1.0 - Alpha) + Work[J] * Alpha;
                }
            }
            const FVector4d& Result = Work[Order - 1];
            const double InvWeight = Result.W != 0.0 ? 1.0 / Result.W : 1.0;
            OutPoints.Add(FVector3f(static_cast<float>(Result.X * InvWeight),
                static_cast<float>(Result.Y * InvWeight), static_cast<float>(Result.Z * InvWeight)));
        };
        for (int32 Sample = 0; Sample < Samples; ++Sample)
        {
            const double Alpha = Samples > 1 ? static_cast<double>(Sample) / (Samples - 1) : 0.0;
            evaluate(FMath::Lerp(Start, End, Alpha));
        }
        PointOffset += static_cast<uint64>(ControlCount);
        KnotOffset += static_cast<uint64>(ControlCount + Order);
    }
    return OutPoints.Num() > 0;
}

static bool BuildHairDescription(lightusd_prim Prim, double TimeCode,
    FHairDescription& OutDescription, FLightUSDUEResult& Result, bool bNurbs,
    bool bPromoteGuides = false)
{
    FArrayView Points, Counts, Widths, Groups, Guides, IDs, RootUV, Colors, Roughness;
    lightusd_value* PointValue = nullptr;
    lightusd_value* CountValue = nullptr;
    lightusd_value* WidthValue = nullptr;
    const bool bSampled = FMath::IsFinite(TimeCode);
    FArrayView ExactPoints;
    if (bSampled && GetExactTimeSample(Prim, "points", TimeCode, ExactPoints))
    {
        Points = ExactPoints;
    }
    else if (bSampled && lightusd_attr_interpolate(Prim, "points", TimeCode, 1, &PointValue) == LIGHTUSD_OK)
    {
        lightusd_value_view View = {};
        lightusd_value_get_view(PointValue, &View);
        Points = { View.data, View.count, View.components,
            static_cast<uint32>(View.nbytes / (View.count * View.components)), View.storage };
    }
    else if (!GetArray(Prim, "points", Points))
    {
        if (PointValue) lightusd_value_destroy(PointValue);
        PointValue = nullptr;
    }
    if (bSampled && lightusd_attr_interpolate(Prim, "curveVertexCounts", TimeCode, 0, &CountValue) == LIGHTUSD_OK)
    {
        lightusd_value_view View = {};
        lightusd_value_get_view(CountValue, &View);
        Counts = { View.data, View.count, View.components,
            static_cast<uint32>(View.nbytes / (View.count * View.components)), View.storage };
    }
    else if (!GetArray(Prim, "curveVertexCounts", Counts))
    {
        if (PointValue) lightusd_value_destroy(PointValue);
        if (CountValue) lightusd_value_destroy(CountValue);
        return false;
    }
    if (Points.Components != 3 || Counts.Components != 1)
    {
        Result.Warnings.Add(TEXT("BasisCurves prim has no usable points/counts."));
        if (PointValue) lightusd_value_destroy(PointValue);
        if (CountValue) lightusd_value_destroy(CountValue);
        if (WidthValue) lightusd_value_destroy(WidthValue);
        return true;
    }
    FArrayView ExactWidths;
    if (bSampled && GetExactTimeSample(Prim, "widths", TimeCode, ExactWidths))
    {
        Widths = ExactWidths;
    }
    else if (bSampled && lightusd_attr_interpolate(Prim, "widths", TimeCode, 1, &WidthValue) == LIGHTUSD_OK)
    {
        lightusd_value_view View = {};
        lightusd_value_get_view(WidthValue, &View);
        Widths = { View.data, View.count, View.components,
            static_cast<uint32>(View.nbytes / (View.count * View.components)), View.storage };
    }
    else
    {
        GetArray(Prim, "widths", Widths);
    }
    GetArray(Prim, "primvars:groom_group_id", Groups);
    GetArray(Prim, "primvars:groom_guide", Guides);
    GetArray(Prim, "primvars:groom_id", IDs);
    GetArray(Prim, "primvars:groom_root_uv", RootUV);
    GetArray(Prim, "primvars:groom_color", Colors);
    GetArray(Prim, "primvars:groom_roughness", Roughness);

    TArray<FVector3f> TessellatedPoints;
    TArray<int32> TessellatedCounts;
    if (bNurbs)
    {
        FArrayView Orders, Knots, Ranges, Weights;
        GetArray(Prim, "order", Orders);
        GetArray(Prim, "knots", Knots);
        GetArray(Prim, "ranges", Ranges);
        GetArray(Prim, "pointWeights", Weights);
        if (TessellateNurbs(Points, Counts, Orders, Knots, Ranges, Weights,
            TessellatedPoints, TessellatedCounts))
        {
            Points.Data = TessellatedPoints.GetData();
            Points.Count = TessellatedPoints.Num();
            Points.Components = 3;
            Points.ComponentSize = sizeof(float);
            Points.Storage = LIGHTUSD_COMP_FLOAT32;
            Counts.Data = TessellatedCounts.GetData();
            Counts.Count = TessellatedCounts.Num();
            Counts.Components = 1;
            Counts.ComponentSize = sizeof(int32);
            Counts.Storage = LIGHTUSD_COMP_INT32;
            Widths = {};
            Result.Warnings.Add(TEXT("NurbsCurves were tessellated to linear HairStrands curves."));
        }
    }

    TArray<FVector3f> StabilizedPoints;
    StabilizeDegenerateBounds(Points, StabilizedPoints, Result);

    int32 StrandCount = static_cast<int32>(Counts.Count);
    int32 VertexCount = 0;
    for (uint64 Index = 0; Index < Counts.Count; ++Index)
        VertexCount += static_cast<int32>(Number(Counts, Index));
    if (StrandCount <= 0 || VertexCount <= 0)
        return true;

    // FHairDescription uses stable IDs into pre-sized attribute arrays.  The
    // UE groom importer follows this pattern; AddVertex/AddStrand after
    // Initialize* would create a second, invalid ID range.
    OutDescription.InitializeVertices(VertexCount);
    OutDescription.InitializeStrands(StrandCount);

    int32 VertexIndex = 0;
    for (int32 StrandIndex = 0; StrandIndex < StrandCount; ++StrandIndex)
    {
        const int32 PointCount = static_cast<int32>(Number(Counts, StrandIndex));
        const FStrandID StrandID(static_cast<int32>(StrandIndex));
        const int32 GroupID = Groups.Count > static_cast<uint64>(StrandIndex)
            ? static_cast<int32>(Number(Groups, StrandIndex)) : 0;
        const int32 Guide = bPromoteGuides ? 0 : Guides.Count > static_cast<uint64>(StrandIndex)
            ? static_cast<int32>(Number(Guides, StrandIndex)) : 0;
        const int32 StrandIDValue = IDs.Count > static_cast<uint64>(StrandIndex)
            ? static_cast<int32>(Number(IDs, StrandIndex)) : StrandIndex;
        SetHairStrandAttribute(OutDescription, StrandID, HairAttribute::Strand::VertexCount, PointCount);
        SetHairStrandAttribute(OutDescription, StrandID, HairAttribute::Strand::GroupID, GroupID);
        SetHairStrandAttribute(OutDescription, StrandID, HairAttribute::Strand::Guide, Guide);
        SetHairStrandAttribute(OutDescription, StrandID, HairAttribute::Strand::ID, StrandIDValue);
        SetHairStrandAttribute(OutDescription, StrandID, HairAttribute::Strand::GroupName,
            FName(*FString::Printf(TEXT("Groom_%d"), GroupID)));
        if (RootUV.Count > static_cast<uint64>(StrandIndex) && RootUV.Components >= 2)
        {
            SetHairStrandAttribute(OutDescription, StrandID, HairAttribute::Strand::RootUV,
                FVector2f(static_cast<float>(Number(RootUV, StrandIndex * RootUV.Components)),
                    static_cast<float>(Number(RootUV, StrandIndex * RootUV.Components + 1))));
        }
        for (int32 PointIndex = 0; PointIndex < PointCount && VertexIndex < VertexCount; ++PointIndex, ++VertexIndex)
        {
            const FVertexID ID(static_cast<int32>(VertexIndex));
            const uint64 Base = static_cast<uint64>(VertexIndex) * Points.Components;
            SetHairVertexAttribute(OutDescription, ID, HairAttribute::Vertex::Position,
                FVector3f(static_cast<float>(Number(Points, Base)), static_cast<float>(Number(Points, Base + 1)), static_cast<float>(Number(Points, Base + 2))));
            const float Width = Widths.Count > static_cast<uint64>(VertexIndex)
                ? static_cast<float>(Number(Widths, VertexIndex)) : 0.01f;
            SetHairVertexAttribute(OutDescription, ID, HairAttribute::Vertex::Width, Width);
            if (Colors.Count > static_cast<uint64>(VertexIndex) && Colors.Components >= 3)
            {
                const uint64 ColorBase = static_cast<uint64>(VertexIndex) * Colors.Components;
                SetHairVertexAttribute(OutDescription, ID, HairAttribute::Vertex::Color,
                    FVector3f(static_cast<float>(Number(Colors, ColorBase)),
                        static_cast<float>(Number(Colors, ColorBase + 1)),
                        static_cast<float>(Number(Colors, ColorBase + 2))));
            }
            if (Roughness.Count > static_cast<uint64>(VertexIndex))
                SetHairVertexAttribute(OutDescription, ID, HairAttribute::Vertex::Roughness,
                    static_cast<float>(Number(Roughness, VertexIndex)));
        }
    }

    if (PointValue) lightusd_value_destroy(PointValue);
    if (CountValue) lightusd_value_destroy(CountValue);
    if (WidthValue) lightusd_value_destroy(WidthValue);
    return true;
}

static bool ImportGroomCache(lightusd_prim Prim, UGroomAsset* Groom,
    const FLightUSDUEOptions& Options, FLightUSDUEResult& Result, bool bNurbs,
    bool bPromoteGuides)
{
    const size_t SampleCount = lightusd_attr_timesample_count(Prim, "points");
    if (SampleCount < 2)
        return true;
    TArray<double> Times;
    Times.SetNum(SampleCount);
    lightusd_attr_timesample_times(Prim, "points", Times.GetData(), SampleCount);

    FGroomCacheProcessor Processor(EGroomCacheType::Strands,
        EGroomCacheAttributes::Position | EGroomCacheAttributes::Width |
        EGroomCacheAttributes::Color);
    TArray<FHairGroupInfoWithVisibility> Visibility = Groom->GetHairGroupsInfo();
    const TArray<FHairGroupsInterpolation>& Interpolation = Groom->GetHairGroupsInterpolation();
    bool bValid = true;
    for (size_t SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
    {
        FHairDescription FrameDescription;
        if (!BuildHairDescription(Prim, Times[SampleIndex], FrameDescription, Result,
            bNurbs, bPromoteGuides))
        {
            bValid = false;
            break;
        }
        TArray<FGroomCacheInputData> InputData;
        if (!UE::Groom::BuildGroupsData(FrameDescription, Groom->GetHairGroupsPlatformData(),
            Visibility, Interpolation, InputData))
        {
            Result.Warnings.Add(TEXT("Animated groom sample topology differs from the static groom."));
            bValid = false;
            break;
        }
        Processor.AddGroomSample(MoveTemp(InputData));
    }
    if (!bValid)
        return false;

    const FString PackagePath = Options.PackagePath.IsEmpty()
        ? TEXT("/Game/LightUSD/Grooms") : Options.PackagePath;
    const FString PrimName = FPaths::GetBaseFilename(SV(lightusd_prim_path(Prim)));
    const FString CacheName = PrimName + TEXT("_StrandsCache");
    UPackage* Package = CreatePackage(*(PackagePath + TEXT("/") + CacheName));
    UGroomCache* Cache = NewObject<UGroomCache>(Package, *CacheName,
        RF_Public | RF_Standalone | RF_Transactional);
    if (!Cache)
        return false;
    FGroomAnimationInfo Animation;
    Animation.NumFrames = static_cast<uint32>(SampleCount);
    Animation.StartFrame = FMath::RoundToInt(Times[0]);
    Animation.EndFrame = FMath::RoundToInt(Times.Last());
    Animation.SecondsPerFrame = 1.0f / 24.0f;
    Animation.Duration = (Animation.EndFrame - Animation.StartFrame) * Animation.SecondsPerFrame;
    Animation.StartTime = Animation.StartFrame * Animation.SecondsPerFrame;
    Animation.EndTime = Animation.EndFrame * Animation.SecondsPerFrame;
    Animation.Attributes = EGroomCacheAttributes::Position | EGroomCacheAttributes::Width |
        EGroomCacheAttributes::Color;
    Cache->Initialize(EGroomCacheType::Strands);
    Processor.TransferChunks(Cache);
    Cache->SetGroomAnimationInfo(Animation);
    Cache->MarkPackageDirty();
    Cache->PostEditChange();
    Result.CreatedAssets.Add(Cache->GetPathName());
    return true;
}

static bool ImportOne(lightusd_prim Prim, const FLightUSDUEOptions& Options,
    FLightUSDUEResult& Result, bool bNurbs)
{
    FArrayView GuideValues;
    bool bGuideOnly = GetArray(Prim, "primvars:groom_guide", GuideValues) &&
        GuideValues.Count > 0;
    for (uint64 Index = 0; bGuideOnly && Index < GuideValues.Count; ++Index)
        bGuideOnly = Number(GuideValues, Index) != 0.0;
    FHairDescription Description;
    double StaticTime = std::numeric_limits<double>::quiet_NaN();
    if (lightusd_attr_timesample_count(Prim, "points") > 0)
        lightusd_attr_timesample_times(Prim, "points", &StaticTime, 1);
    if (!BuildHairDescription(Prim, StaticTime, Description, Result, bNurbs, bGuideOnly))
        return false;
    FHairDescriptionGroups GroupsDescription;
    FGroomBuilder::BuildHairDescriptionGroups(Description, GroupsDescription);
    const int32 GroupCount = GroupsDescription.HairGroups.Num();
    UGroomImportOptions* ImportOptions = NewObject<UGroomImportOptions>(GetTransientPackage());
    ImportOptions->InterpolationSettings.Init(FHairGroupsInterpolation(), GroupCount);
    const FString PackagePath = Options.PackagePath.IsEmpty()
        ? TEXT("/Game/LightUSD/Grooms") : Options.PackagePath;
    const lightusd_sv PrimPath = lightusd_prim_path(Prim);
    const FString AssetName = FPaths::GetBaseFilename(SV(PrimPath));
    UPackage* Package = CreatePackage(*(PackagePath + TEXT("/") + AssetName));
    FHairImportContext Context(ImportOptions, Package, UGroomAsset::StaticClass(),
        FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
    UGroomAsset* Groom = FHairStrandsImporter::ImportHair(Context, Description);
    if (!Groom)
    {
        Result.Warnings.Add(TEXT("HairStrands rejected a BasisCurves prim."));
        return true;
    }
    Groom->MarkPackageDirty();
    if (bGuideOnly)
    {
        Package->GetMetaData().SetValue(Groom, TEXT("LightUSDGuideOnly"), TEXT("1"));
        Result.Warnings.Add(TEXT("Guide-only BasisCurves were promoted to render strands for UE HairStrands and retain their USD guide role on export."));
    }
    Result.CreatedAssets.Add(Groom->GetPathName());
    if (lightusd_attr_timesample_count(Prim, "points") > 1 &&
        !ImportGroomCache(Prim, Groom, Options, Result, bNurbs, bGuideOnly))
        Result.Warnings.Add(TEXT("Animated groom cache import failed; static groom was retained."));

    // Keep imported static grooms renderable even when the USD layer has no
    // UE material binding. The material is deliberately minimal; a later
    // MaterialX/UE graph import can replace this slot.
    const FString MaterialPackagePath = PackagePath + TEXT("/Materials/M_LightUSD_DefaultHair");
    UPackage* MaterialPackage = CreatePackage(*MaterialPackagePath);
    UMaterial* HairMaterial = FindObject<UMaterial>(MaterialPackage, TEXT("M_LightUSD_DefaultHair"));
    if (!HairMaterial)
        HairMaterial = NewObject<UMaterial>(MaterialPackage, TEXT("M_LightUSD_DefaultHair"), RF_Public | RF_Standalone);
    if (HairMaterial)
    {
        HairMaterial->SetShadingModel(EMaterialShadingModel::MSM_Hair);
        for (FHairGroupsMaterial& GroupMaterial : Groom->GetHairGroupsMaterials())
            GroupMaterial.Material = HairMaterial;
        HairMaterial->MarkPackageDirty();
        Result.CreatedAssets.Add(HairMaterial->GetPathName());
    }

    if (!Options.GroomTargetSkeletalMeshPath.IsEmpty())
    {
        USkeletalMesh* Target = LoadObject<USkeletalMesh>(nullptr, *Options.GroomTargetSkeletalMeshPath);
        if (!Target)
        {
            Result.Warnings.Add(FString::Printf(TEXT("Groom target skeletal mesh was not found: %s"), *Options.GroomTargetSkeletalMeshPath));
        }
        else
        {
            const FString BindingName = AssetName + TEXT("_Binding");
            const FString BindingPath = PackagePath + TEXT("/") + BindingName;
            if (UGroomBindingAsset* Binding = UGroomBlueprintLibrary::CreateNewGroomBindingAssetWithPath(
                BindingPath, Groom, Target, 100, nullptr, 0))
            {
                Result.CreatedAssets.Add(Binding->GetPathName());
            }
            else
            {
                Result.Warnings.Add(TEXT("HairStrands could not create a groom binding asset."));
            }
        }
    }
    return true;
}

static bool SetArray(lightusd_stage* Stage, const char* PrimPath, const char* Name,
    lightusd_type Type, const void* Data, size_t Count)
{
    return lightusd_attr_set(Stage, PrimPath, Name, Type, 1, Data, Count, 0) == LIGHTUSD_OK;
}

static bool SetUniform(lightusd_stage* Stage, const char* PrimPath, const char* Name)
{
    const char* Interpolation = "uniform";
    return lightusd_attr_set_metadata(Stage, PrimPath, Name, "interpolation",
        LIGHTUSD_TYPE_TOKEN, Interpolation, 1) == LIGHTUSD_OK;
}

static bool AppendStrand(const FEditableHairStrand& Strand, uint32 GroupID,
    bool bGuide, TArray<FVector3f>& Points, TArray<float>& Widths,
    TArray<int32>& Counts, TArray<int32>& GroupIDs, TArray<int32>& Guides,
    TArray<int32>& StrandIDs, TArray<FVector2f>& RootUVs,
    TArray<FVector3f>& Colors, TArray<float>& Roughness)
{
    if (Strand.ControlPoints.Num() < 2)
        return false;
    Counts.Add(Strand.ControlPoints.Num());
    GroupIDs.Add(static_cast<int32>(GroupID));
    Guides.Add(bGuide ? 1 : 0);
    StrandIDs.Add(static_cast<int32>(Strand.StrandID));
    RootUVs.Add(Strand.RootUV);
    for (const FEditableHairStrandControlPoint& Point : Strand.ControlPoints)
    {
        Points.Add(Point.Position);
        Widths.Add(Point.Radius > 0.0f ? Point.Radius * 2.0f : 0.02f);
        Colors.Add(Point.bHasColor
            ? FVector3f(Point.BaseColor.R, Point.BaseColor.G, Point.BaseColor.B)
            : FVector3f(1.0f, 1.0f, 1.0f));
        Roughness.Add(Point.bHasRoughness ? Point.Roughness : 0.5f);
    }
    return true;
}

static void VisitPrim(lightusd_prim Prim, const FLightUSDUEOptions& Options, FLightUSDUEResult& Result)
{
    if (!lightusd_prim_is_valid(Prim))
        return;
    const FString TypeName = SV(lightusd_prim_type_name(Prim));
    if (TypeName == TEXT("BasisCurves") || TypeName == TEXT("NurbsCurves"))
        ImportOne(Prim, Options, Result, TypeName == TEXT("NurbsCurves"));
    for (size_t Index = 0; Index < lightusd_prim_child_count(Prim); ++Index)
        VisitPrim(lightusd_prim_child(Prim, Index), Options, Result);
}
}

namespace LightUSDUEGroom
{
bool ImportBasisCurves(lightusd_stage* Stage, const FLightUSDUEOptions& Options,
    FLightUSDUEResult& Result)
{
    for (size_t Index = 0; Index < lightusd_stage_root_prim_count(Stage); ++Index)
        VisitPrim(lightusd_stage_root_prim(Stage, Index), Options, Result);
    return true;
}

bool ExportGroom(UGroomAsset* Groom, const FString& Filename, FLightUSDUEResult& Result)
{
    FEditableGroom Editable;
    ConvertFromGroomAsset(Groom, &Editable, false, false, false);
    TArray<FVector3f> Points;
    TArray<float> Widths;
    TArray<int32> Counts;
    TArray<int32> GroupIDs;
    TArray<int32> Guides;
    TArray<int32> StrandIDs;
    TArray<FVector2f> RootUVs;
    TArray<FVector3f> Colors;
    TArray<float> Roughness;
    const bool bGuideOnly = Groom->GetOutermost()->GetMetaData().HasValue(
        Groom, TEXT("LightUSDGuideOnly"));
    for (const FEditableGroomGroup& Group : Editable.Groups)
    {
        for (const FEditableHairStrand& Strand : Group.Strands)
            AppendStrand(Strand, Group.GroupID, bGuideOnly, Points, Widths, Counts,
                GroupIDs, Guides, StrandIDs, RootUVs, Colors, Roughness);
        // HairStrands derives guides from render strands during import. Those
        // derived guides are not additional authored groom curves and would
        // duplicate the geometry on USD export. Preserve guides only for a
        // guide-only group, where they are the actual authored data.
        if (Group.Strands.Num() == 0)
        {
            for (const FEditableHairGuide& Guide : Group.Guides)
            {
                FEditableHairStrand Strand;
                Strand.StrandID = Guide.GuideID;
                Strand.RootUV = Guide.RootUV;
                for (const FEditableHairGuideControlPoint& Point : Guide.ControlPoints)
                {
                    FEditableHairStrandControlPoint& Out = Strand.ControlPoints.AddDefaulted_GetRef();
                    Out.Position = Point.Position;
                    Out.Radius = 0.01f;
                }
                AppendStrand(Strand, Group.GroupID, true, Points, Widths, Counts,
                    GroupIDs, Guides, StrandIDs, RootUVs, Colors, Roughness);
            }
        }
    }
    if (Points.Num() == 0 || Counts.Num() == 0)
    {
        Result.Error = TEXT("UGroomAsset contains no exportable static strands.");
        return false;
    }

    lightusd_stage* Stage = nullptr;
    if (lightusd_stage_create(&Stage) != LIGHTUSD_OK || !Stage)
    {
        Result.Error = UTF8_TO_TCHAR(lightusd_last_error());
        return false;
    }
    const char* PrimPath = "/World/Groom";
    lightusd_prim Prim = {};
    const bool bDefined = lightusd_stage_define_prim(Stage, PrimPath, "BasisCurves", 0, &Prim) == LIGHTUSD_OK;
    bool bOk = bDefined;
    bOk &= lightusd_attr_set(Stage, PrimPath, "type", LIGHTUSD_TYPE_TOKEN, 0,
        "linear", 1, 0) == LIGHTUSD_OK;
    bOk &= lightusd_attr_set(Stage, PrimPath, "wrap", LIGHTUSD_TYPE_TOKEN, 0,
        "nonperiodic", 1, 0) == LIGHTUSD_OK;
    bOk &= SetArray(Stage, PrimPath, "points", LIGHTUSD_TYPE_POINT3F,
        Points.GetData(), Points.Num());
    bOk &= SetArray(Stage, PrimPath, "curveVertexCounts", LIGHTUSD_TYPE_INT,
        Counts.GetData(), Counts.Num());
    bOk &= SetArray(Stage, PrimPath, "widths", LIGHTUSD_TYPE_FLOAT,
        Widths.GetData(), Widths.Num());
    bOk &= SetArray(Stage, PrimPath, "primvars:groom_group_id", LIGHTUSD_TYPE_INT,
        GroupIDs.GetData(), GroupIDs.Num());
    bOk &= SetUniform(Stage, PrimPath, "primvars:groom_group_id");
    bOk &= SetArray(Stage, PrimPath, "primvars:groom_guide", LIGHTUSD_TYPE_INT,
        Guides.GetData(), Guides.Num());
    bOk &= SetUniform(Stage, PrimPath, "primvars:groom_guide");
    bOk &= SetArray(Stage, PrimPath, "primvars:groom_id", LIGHTUSD_TYPE_INT,
        StrandIDs.GetData(), StrandIDs.Num());
    bOk &= SetUniform(Stage, PrimPath, "primvars:groom_id");
    bOk &= SetArray(Stage, PrimPath, "primvars:groom_root_uv", LIGHTUSD_TYPE_FLOAT2,
        RootUVs.GetData(), RootUVs.Num());
    bOk &= SetUniform(Stage, PrimPath, "primvars:groom_root_uv");
    bOk &= SetArray(Stage, PrimPath, "primvars:groom_color", LIGHTUSD_TYPE_COLOR3F,
        Colors.GetData(), Colors.Num());
    bOk &= lightusd_attr_set_metadata(Stage, PrimPath, "primvars:groom_color",
        "interpolation", LIGHTUSD_TYPE_TOKEN, "vertex", 1) == LIGHTUSD_OK;
    bOk &= SetArray(Stage, PrimPath, "primvars:groom_roughness", LIGHTUSD_TYPE_FLOAT,
        Roughness.GetData(), Roughness.Num());
    bOk &= lightusd_attr_set_metadata(Stage, PrimPath, "primvars:groom_roughness",
        "interpolation", LIGHTUSD_TYPE_TOKEN, "vertex", 1) == LIGHTUSD_OK;
    if (!bOk)
    {
        Result.Error = UTF8_TO_TCHAR(lightusd_last_error());
        lightusd_stage_destroy(Stage);
        return false;
    }
    lightusd_save_options SaveOptions;
    lightusd_save_options_init(&SaveOptions);
    const FTCHARToUTF8 OutputPath(*Filename);
    if (lightusd_stage_save(Stage, OutputPath.Get(), &SaveOptions) != LIGHTUSD_OK)
    {
        Result.Error = UTF8_TO_TCHAR(lightusd_last_error());
        lightusd_stage_destroy(Stage);
        return false;
    }
    lightusd_stage_destroy(Stage);
    Result.CreatedAssets.Add(FString::Printf(TEXT("BasisCurves: strands=%d points=%d"),
        Counts.Num(), Points.Num()));
    return true;
}

bool ExportGroomCache(UGroomCache* Cache, UGroomAsset* Groom, const FString& Filename,
    FLightUSDUEResult& Result)
{
    if (!Cache || !Groom || Cache->GetType() != EGroomCacheType::Strands)
    {
        Result.Error = TEXT("ExportGroomCache requires a strands UGroomCache.");
        return false;
    }
    const FGroomAnimationInfo& Animation = Cache->GetGroomAnimationInfo();
    if (Animation.NumFrames < 1)
    {
        Result.Error = TEXT("UGroomCache contains no animation frames.");
        return false;
    }

    lightusd_stage* Stage = nullptr;
    if (lightusd_stage_create(&Stage) != LIGHTUSD_OK || !Stage)
    {
        Result.Error = UTF8_TO_TCHAR(lightusd_last_error());
        return false;
    }
    const char* PrimPath = "/World/GroomCache";
    lightusd_prim Prim = {};
    const bool bDefined = lightusd_stage_define_prim(Stage, PrimPath, "BasisCurves", 0, &Prim) == LIGHTUSD_OK;
    bool bOk = bDefined;
    if (!bDefined)
    {
        Result.Error = FString::Printf(TEXT("Unable to define groom cache prim: %s"),
            UTF8_TO_TCHAR(lightusd_last_error()));
    }
    const bool bType = lightusd_attr_set(Stage, PrimPath, "type", LIGHTUSD_TYPE_TOKEN, 0,
        "linear", 1, 0) == LIGHTUSD_OK;
    const bool bWrap = lightusd_attr_set(Stage, PrimPath, "wrap", LIGHTUSD_TYPE_TOKEN, 0,
        "nonperiodic", 1, 0) == LIGHTUSD_OK;
    bOk &= bType && bWrap;
    if (!bType || !bWrap)
    {
        Result.Error = FString::Printf(TEXT("Unable to author groom cache basis attributes (type=%d wrap=%d): %s"),
            bType, bWrap, UTF8_TO_TCHAR(lightusd_last_error()));
    }

    TArray<int32> Counts;
    TArray<int32> GroupIDs;
    TArray<int32> Guides;
    TArray<int32> StrandIDs;
    TArray<FVector2f> RootUVs;
    const bool bGuideOnly = Groom->GetOutermost()->GetMetaData().HasValue(
        Groom, TEXT("LightUSDGuideOnly"));
    FEditableGroom StaticGroom;
    ConvertFromGroomAsset(Groom, &StaticGroom, false, false, false);
    if (StaticGroom.Groups.Num() == 0)
    {
        Result.Error = TEXT("Source UGroomAsset contains no editable strand groups for cache topology.");
        lightusd_stage_destroy(Stage);
        return false;
    }
    int32 FirstFramePointCount = 0;
    for (uint32 FrameIndex = 0; FrameIndex < Animation.NumFrames; ++FrameIndex)
    {
        FGroomCacheAnimationData Frame;
        if (!Cache->GetGroomDataAtFrameIndex(static_cast<int32>(FrameIndex), Frame) || Frame.GroupsData.Num() == 0)
        {
            bOk = false;
            break;
        }
        TArray<FVector3f> Points;
        TArray<float> Widths;
        if (FrameIndex == 0)
            Counts.Reset();
        if (Frame.GroupsData.Num() != StaticGroom.Groups.Num())
        {
            Result.Error = FString::Printf(TEXT("Groom cache group count (%d) does not match groom asset (%d)."),
                Frame.GroupsData.Num(), StaticGroom.Groups.Num());
            bOk = false;
            break;
        }
        for (int32 GroupIndex = 0; GroupIndex < Frame.GroupsData.Num(); ++GroupIndex)
        {
            const FGroomCacheGroupData& Group = Frame.GroupsData[GroupIndex];
            const FEditableGroomGroup& StaticGroup = StaticGroom.Groups[GroupIndex];
            int32 ExpectedPointCount = 0;
            if (FrameIndex == 0)
            {
                if (StaticGroup.Strands.Num() > 0)
                {
                    for (const FEditableHairStrand& Strand : StaticGroup.Strands)
                    {
                        Counts.Add(Strand.ControlPoints.Num());
                        GroupIDs.Add(StaticGroup.GroupID);
                        Guides.Add(bGuideOnly ? 1 : 0);
                        StrandIDs.Add(Strand.StrandID);
                        RootUVs.Add(Strand.RootUV);
                        ExpectedPointCount += Strand.ControlPoints.Num();
                    }
                }
                else
                {
                    for (const FEditableHairGuide& Guide : StaticGroup.Guides)
                    {
                        Counts.Add(Guide.ControlPoints.Num());
                        GroupIDs.Add(StaticGroup.GroupID);
                        Guides.Add(1);
                        StrandIDs.Add(Guide.GuideID);
                        RootUVs.Add(Guide.RootUV);
                        ExpectedPointCount += Guide.ControlPoints.Num();
                    }
                }
            }
            else if (StaticGroup.Strands.Num() > 0)
            {
                for (const FEditableHairStrand& Strand : StaticGroup.Strands)
                    ExpectedPointCount += Strand.ControlPoints.Num();
            }
            else
            {
                for (const FEditableHairGuide& Guide : StaticGroup.Guides)
                    ExpectedPointCount += Guide.ControlPoints.Num();
            }
            if (Group.VertexData.PointsPosition.Num() != ExpectedPointCount ||
                Group.VertexData.PointsRadius.Num() != ExpectedPointCount)
            {
                Result.Error = FString::Printf(TEXT("Groom cache frame %d group %d point topology does not match the groom asset."),
                    FrameIndex, GroupIndex);
                bOk = false;
                break;
            }
            const float MaxRadius = FMath::Max(Group.StrandData.MaxRadius, KINDA_SMALL_NUMBER);
            Points.Append(Group.VertexData.PointsPosition);
            for (float Radius : Group.VertexData.PointsRadius)
                Widths.Add(Radius * MaxRadius * 2.0f);
        }
        if (!bOk)
            break;
        if (FrameIndex == 0)
            FirstFramePointCount = Points.Num();
        const double Time = Animation.StartFrame + static_cast<double>(FrameIndex);
        const bool bPointsSample = lightusd_attr_set_timesample(Stage, PrimPath, "points", Time,
            LIGHTUSD_TYPE_POINT3F, 1, Points.GetData(), Points.Num()) == LIGHTUSD_OK;
        const bool bWidthsSample = lightusd_attr_set_timesample(Stage, PrimPath, "widths", Time,
            LIGHTUSD_TYPE_FLOAT, 1, Widths.GetData(), Widths.Num()) == LIGHTUSD_OK;
        bOk &= bPointsSample && bWidthsSample;
        if (!bPointsSample || !bWidthsSample)
        {
            Result.Error = FString::Printf(TEXT("Unable to author groom cache time sample at frame %d (points=%d widths=%d): %s"),
                FrameIndex, bPointsSample, bWidthsSample, UTF8_TO_TCHAR(lightusd_last_error()));
        }
    }
    if (Counts.Num() > 0)
    {
        const bool bCounts = SetArray(Stage, PrimPath, "curveVertexCounts", LIGHTUSD_TYPE_INT,
            Counts.GetData(), Counts.Num());
        bOk &= bCounts;
        if (!bCounts)
        {
            Result.Error = FString::Printf(TEXT("Unable to author groom cache curve counts (%d): %s"),
                Counts.Num(), UTF8_TO_TCHAR(lightusd_last_error()));
        }
        const bool bGroups = SetArray(Stage, PrimPath, "primvars:groom_group_id",
            LIGHTUSD_TYPE_INT, GroupIDs.GetData(), GroupIDs.Num()) &&
            SetUniform(Stage, PrimPath, "primvars:groom_group_id");
        const bool bGuides = SetArray(Stage, PrimPath, "primvars:groom_guide",
            LIGHTUSD_TYPE_INT, Guides.GetData(), Guides.Num()) &&
            SetUniform(Stage, PrimPath, "primvars:groom_guide");
        const bool bIDs = SetArray(Stage, PrimPath, "primvars:groom_id",
            LIGHTUSD_TYPE_INT, StrandIDs.GetData(), StrandIDs.Num()) &&
            SetUniform(Stage, PrimPath, "primvars:groom_id");
        const bool bRootUVs = SetArray(Stage, PrimPath, "primvars:groom_root_uv",
            LIGHTUSD_TYPE_FLOAT2, RootUVs.GetData(), RootUVs.Num()) &&
            SetUniform(Stage, PrimPath, "primvars:groom_root_uv");
        bOk &= bGroups && bGuides && bIDs && bRootUVs;
        if (!bGroups || !bGuides || !bIDs || !bRootUVs)
        {
            Result.Error = FString::Printf(TEXT("Unable to author groom cache curve metadata: %s"),
                UTF8_TO_TCHAR(lightusd_last_error()));
        }
    }
    // LightUSD's groom schema requires these per-vertex attributes on a
    // BasisCurves groom. A UE GroomCache does not animate them, so author
    // neutral defaults while points and widths carry the cache animation.
    if (FirstFramePointCount > 0)
    {
        TArray<FVector3f> Colors;
        TArray<float> Roughness;
        Colors.Init(FVector3f(1.0f, 1.0f, 1.0f), FirstFramePointCount);
        Roughness.Init(0.0f, FirstFramePointCount);
        const bool bColor = SetArray(Stage, PrimPath, "primvars:groom_color", LIGHTUSD_TYPE_COLOR3F,
            Colors.GetData(), Colors.Num());
        const bool bColorMeta = lightusd_attr_set_metadata(Stage, PrimPath, "primvars:groom_color",
            "interpolation", LIGHTUSD_TYPE_TOKEN, "vertex", 1) == LIGHTUSD_OK;
        const bool bRoughness = SetArray(Stage, PrimPath, "primvars:groom_roughness", LIGHTUSD_TYPE_FLOAT,
            Roughness.GetData(), Roughness.Num());
        const bool bRoughnessMeta = lightusd_attr_set_metadata(Stage, PrimPath, "primvars:groom_roughness",
            "interpolation", LIGHTUSD_TYPE_TOKEN, "vertex", 1) == LIGHTUSD_OK;
        bOk &= bColor && bColorMeta && bRoughness && bRoughnessMeta;
        if (!bColor || !bColorMeta || !bRoughness || !bRoughnessMeta)
        {
            Result.Error = FString::Printf(TEXT("Unable to author groom cache attributes (points=%d color=%d/%d roughness=%d/%d): %s"),
                FirstFramePointCount, bColor, bColorMeta, bRoughness, bRoughnessMeta,
                UTF8_TO_TCHAR(lightusd_last_error()));
        }
    }
    if (FirstFramePointCount == 0)
    {
        Result.Error = TEXT("UGroomCache first frame contains no point data.");
        bOk = false;
    }
    lightusd_value_view RoughnessView = {};
    if (bOk && lightusd_attr_get(Prim, "primvars:groom_roughness", &RoughnessView) != LIGHTUSD_OK)
    {
        Result.Error = FString::Printf(TEXT("Groom cache roughness was not readable before save: %s"),
            UTF8_TO_TCHAR(lightusd_last_error()));
        bOk = false;
    }
    if (!bOk)
    {
        if (Result.Error.IsEmpty())
        {
            Result.Error = FString::Printf(TEXT("LightUSD groom cache stage authoring failed: %s"),
                UTF8_TO_TCHAR(lightusd_last_error()));
        }
        lightusd_stage_destroy(Stage);
        return false;
    }
    lightusd_save_options SaveOptions;
    lightusd_save_options_init(&SaveOptions);
    const FTCHARToUTF8 OutputPath(*Filename);
    if (lightusd_stage_save(Stage, OutputPath.Get(), &SaveOptions) != LIGHTUSD_OK)
    {
        Result.Error = UTF8_TO_TCHAR(lightusd_last_error());
        lightusd_stage_destroy(Stage);
        return false;
    }
    lightusd_stage_destroy(Stage);
    Result.CreatedAssets.Add(FString::Printf(TEXT("BasisCurvesCache: frames=%d curves=%d"),
        Animation.NumFrames, Counts.Num()));
    return true;
}
}
