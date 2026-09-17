#include "LightUSDUE.h"
#include "LightUSDUEGroom.h"
#include "GroomAsset.h"
#include "GroomCache.h"

#include "Editor.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "USDStageImportContext.h"
#include "USDStageImportOptions.h"
#include "USDStageImporter.h"
#include "USDStageImporterModule.h"
#include "IPythonScriptPlugin.h"
#include "PythonScriptTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Factories/AnimSequenceFactory.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "SkeletalMeshTypes.h"
#include "Subsystems/EditorAssetSubsystem.h"

THIRD_PARTY_INCLUDES_START
extern "C" {
#include "lightusd-c.h"
#include "lightusd-render-c.h"
}
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogLightUSDUE, Log, All);

namespace
{
static FString SV(lightusd_sv Value)
{
    return FString(Value.data ? UTF8_TO_TCHAR(Value.data) : TEXT(""));
}

static void SetLightUSDError(FLightUSDUEResult& Result, const FString& Prefix)
{
    Result.bSucceeded = false;
    Result.Error = Prefix + TEXT(": ") + UTF8_TO_TCHAR(lightusd_last_error());
}

static bool LoadStage(const FString& Filename, const FLightUSDUEOptions& Options,
                      lightusd_stage** OutStage, FLightUSDUEResult& Result)
{
    lightusd_load_options LoadOptions;
    lightusd_load_options_init(&LoadOptions);
    LoadOptions.composed = Options.bCompose ? 1 : 0;
    LoadOptions.load_payloads = Options.bLoadPayloads ? 1 : 0;
    LoadOptions.max_memory = Options.MaxMemoryMB > 0
        ? static_cast<uint64>(Options.MaxMemoryMB) * 1024ull * 1024ull : 0;

    const FTCHARToUTF8 Path(*Filename);
    const lightusd_status Status = lightusd_stage_load(Path.Get(), &LoadOptions, OutStage);
    if (Status != LIGHTUSD_OK)
    {
        SetLightUSDError(Result, TEXT("LightUSD stage load failed"));
        return false;
    }

    lightusd_string* WarningString = nullptr;
    if (lightusd_stage_take_warnings(*OutStage, &WarningString) == LIGHTUSD_OK && WarningString)
    {
        const lightusd_sv Warning = lightusd_string_view(WarningString);
        if (Warning.len > 0)
            Result.Warnings.Add(SV(Warning));
        lightusd_string_destroy(WarningString);
    }
    return true;
}

static void CollectPrim(lightusd_prim Prim, FLightUSDUEResult& Result)
{
    if (!lightusd_prim_is_valid(Prim))
        return;
    Result.PrimPaths.Add(SV(lightusd_prim_path(Prim)));
    for (size_t Index = 0; Index < lightusd_prim_child_count(Prim); ++Index)
        CollectPrim(lightusd_prim_child(Prim, Index), Result);
}

static FLightUSDUEResult ImportWithNativeUE(const FString& Filename,
                                             const FLightUSDUEOptions& Options)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = ELightUSDUEBackend::NativeUE;
    Result.RootLayer = Filename;

    if (!IUsdStageImporterModule::IsAvailable())
    {
        Result.Error = TEXT("The UE USDStageImporter module is unavailable.");
        return Result;
    }

    UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
    if (!World)
    {
        Result.Error = TEXT("Native UE USD import requires an editor world.");
        return Result;
    }

    const FString ObjectName = FPaths::GetBaseFilename(Filename);
    const FString PackagePath = Options.PackagePath.IsEmpty() ? TEXT("/Game/LightUSD") : Options.PackagePath;
    FUsdStageImportContext Context;
    if (!Context.Init(ObjectName, Filename, PackagePath, RF_Public | RF_Standalone, true))
    {
        Result.Error = TEXT("Failed to initialize the UE USD import context.");
        return Result;
    }
    Context.World = World;
    Context.ImportOptions = NewObject<UUsdStageImportOptions>(GetTransientPackage());
    Context.ImportOptions->bImportActors = true;
    Context.ImportOptions->bImportGeometry = Options.bImportGeometry;
    Context.ImportOptions->bImportMaterials = Options.bImportMaterials;
    Context.ImportOptions->bImportSkeletalAnimations = Options.bImportSkeletalAnimation;
    Context.ImportOptions->bImportGroomAssets = Options.bImportGroom;
    Context.ImportOptions->bImportAtSpecificTimeCode = Options.TimeCode != 0.0;
    Context.ImportOptions->ImportTimeCode = static_cast<float>(Options.TimeCode);
    Context.ImportOptions->RenderContextToImport = FName(*Options.RenderContext);
    Context.ImportOptions->MaterialPurpose = FName(*Options.MaterialPurpose);

    IUsdStageImporterModule::Get().GetImporter()->ImportFromFile(Context);
    if (Context.SceneActor)
        Result.CreatedAssets.Add(Context.SceneActor->GetPathName());
    for (UObject* Asset : Context.ImportedAssets)
    {
        if (Asset)
            Result.CreatedAssets.Add(Asset->GetPathName());
    }
    Result.bSucceeded = Context.SceneActor != nullptr || Context.ImportedAssets.Num() > 0;
    if (!Result.bSucceeded)
        Result.Error = TEXT("UE USD importer produced no actors or assets.");
    return Result;
}

static FString PythonQuote(const FString& Value)
{
    FString Escaped = Value.ReplaceCharWithEscapedChar();
    Escaped.ReplaceInline(TEXT("\\'"), TEXT("\\\\'"));
    return FString::Printf(TEXT("'%s'"), *Escaped);
}

static FLightUSDUEResult RunMaterialPython(const FString& Code,
                                            const FString& Filename)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = ELightUSDUEBackend::LightUSD;
    Result.RootLayer = Filename;
    IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
    if (!Python) {
        Result.Error = TEXT("PythonScriptPlugin is unavailable.");
        return Result;
    }
    if (!Python->IsPythonInitialized() && !Python->ForceEnablePythonAtRuntime()) {
        Result.Error = TEXT("Python could not be initialized.");
        return Result;
    }
    FPythonCommandEx Command;
    const FString Escaped = Code.ReplaceCharWithEscapedChar();
    Command.Command = FString::Printf(
        TEXT("(exec(\"%s\") or str(globals().get('_lightusd_ue_result', '')))"),
        *Escaped);
    Command.ExecutionMode = EPythonCommandExecutionMode::EvaluateStatement;
    if (!Python->ExecPythonCommandEx(Command)) {
        Result.Error = TEXT("Unreal Python material bridge failed; see the UE log.");
        return Result;
    }
    FString Json = Command.CommandResult;
    bool bRemoved = false;
    Json.TrimCharInline(FString::ElementType('\''), &bRemoved);
    TSharedPtr<FJsonObject> Object;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
    if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid()) {
        Result.Error = TEXT("Material bridge returned invalid result JSON.");
        return Result;
    }
    Result.bSucceeded = Object->GetBoolField(TEXT("succeeded"));
    Result.Error = Object->GetStringField(TEXT("error"));
    Result.CreatedAssets = [&Object]() {
        TArray<FString> Values;
        const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
        if (Object->TryGetArrayField(TEXT("created_assets"), Array))
            for (const TSharedPtr<FJsonValue>& Value : *Array) Values.Add(Value->AsString());
        return Values;
    }();
    return Result;
}
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::ImportUSD(const FString& Filename,
                                                          const FLightUSDUEOptions& Options)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = Options.Backend == ELightUSDUEBackend::NativeUE
        ? ELightUSDUEBackend::NativeUE : ELightUSDUEBackend::LightUSD;
    Result.RootLayer = Filename;

    FString MaterialLayer;
    const bool bMaterialGraphLayer =
        FFileHelper::LoadFileToString(MaterialLayer, *Filename) &&
        MaterialLayer.Contains(TEXT("MaterialUEConfigAPI")) &&
        MaterialLayer.Contains(TEXT("def Material")) &&
        !MaterialLayer.Contains(TEXT("def Mesh"));
    if (bMaterialGraphLayer &&
        (Options.Backend == ELightUSDUEBackend::NativeUE ||
         Options.Backend == ELightUSDUEBackend::Auto)) {
        return ImportMaterial(Filename, Options);
    }

    if (Options.Backend == ELightUSDUEBackend::NativeUE)
        return ImportWithNativeUE(Filename, Options);
    if (Options.Backend == ELightUSDUEBackend::Auto && IsNativeBackendAvailable())
        return ImportWithNativeUE(Filename, Options);

    lightusd_stage* Stage = nullptr;
    if (!LoadStage(Filename, Options, &Stage, Result))
        return Result;
    for (size_t Index = 0; Index < lightusd_stage_root_prim_count(Stage); ++Index)
        CollectPrim(lightusd_stage_root_prim(Stage, Index), Result);

    lightusd_render_config Config;
    lightusd_render_config_init(&Config);
    Config.triangulate = 1;
    Config.compute_normals = 1;
    Config.compute_tangents = 1;
    Config.load_textures = Options.bImportMaterials ? 1 : 0;
    Config.time_code = Options.TimeCode;
    lightusd_render_scene* RenderScene = nullptr;
    if (lightusd_render_convert(Stage, &Config, &RenderScene) != LIGHTUSD_OK)
    {
        SetLightUSDError(Result, TEXT("LightUSD render conversion failed"));
        lightusd_stage_destroy(Stage);
        return Result;
    }

    Result.CreatedAssets.Add(FString::Printf(
        TEXT("LightUSDRenderScene: meshes=%d materials=%d skeletons=%d animations=%d"),
        static_cast<int32>(lightusd_render_count(RenderScene, LIGHTUSD_RENDER_MESH)),
        static_cast<int32>(lightusd_render_count(RenderScene, LIGHTUSD_RENDER_MATERIAL)),
        static_cast<int32>(lightusd_render_count(RenderScene, LIGHTUSD_RENDER_SKELETON)),
        static_cast<int32>(lightusd_render_count(RenderScene, LIGHTUSD_RENDER_ANIMATION))));
    if (Options.bImportGroom)
    {
        const int32 Before = Result.CreatedAssets.Num();
        if (!LightUSDUEGroom::ImportBasisCurves(Stage, Options, Result))
            Result.Warnings.Add(TEXT("BasisCurves adapter reported an import failure."));
        for (int32 Index = Before; Index < Result.CreatedAssets.Num(); ++Index)
        {
            if (Result.CreatedAssets[Index].Contains(TEXT("GroomBindingAsset")))
                ++Result.GroomBindings;
            else if (Result.CreatedAssets[Index].Contains(TEXT("GroomAsset")))
                ++Result.GroomAssets;
            else if (Result.CreatedAssets[Index].Contains(TEXT("GroomCache")))
                ++Result.GroomCaches;
        }
    }
    if (Options.bImportPhysics)
        Result.Warnings.Add(TEXT("Physics API schemas are preserved for the UE physics adapter."));

    lightusd_render_scene_destroy(RenderScene);
    lightusd_stage_destroy(Stage);
    Result.bSucceeded = true;
    return Result;
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::ExportUSD(const FString& Filename,
                                                          const FLightUSDUEOptions& Options)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = Options.Backend == ELightUSDUEBackend::NativeUE
        ? ELightUSDUEBackend::NativeUE : ELightUSDUEBackend::LightUSD;
    Result.Error = TEXT("Export requires a UE world/asset selection; use the Python facade.");
    return Result;
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::ExportGroom(
    UObject* GroomObject, const FString& Filename, const FLightUSDUEOptions&)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = ELightUSDUEBackend::LightUSD;
    Result.RootLayer = Filename;
    UGroomAsset* Groom = Cast<UGroomAsset>(GroomObject);
    if (!Groom)
    {
        Result.Error = TEXT("ExportGroom requires a UGroomAsset.");
        return Result;
    }
    Result.bSucceeded = LightUSDUEGroom::ExportGroom(Groom, Filename, Result);
    return Result;
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::ExportGroomCache(
    UObject* GroomCacheObject, UObject* GroomAssetObject, const FString& Filename,
    const FLightUSDUEOptions&)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = ELightUSDUEBackend::LightUSD;
    Result.RootLayer = Filename;
    UGroomCache* Cache = Cast<UGroomCache>(GroomCacheObject);
    if (!Cache)
    {
        Result.Error = TEXT("ExportGroomCache requires a UGroomCache.");
        return Result;
    }
    UGroomAsset* Groom = Cast<UGroomAsset>(GroomAssetObject);
    if (!Groom)
    {
        Result.Error = TEXT("ExportGroomCache requires the source UGroomAsset for curve topology.");
        return Result;
    }
    Result.bSucceeded = LightUSDUEGroom::ExportGroomCache(Cache, Groom, Filename, Result);
    return Result;
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::ExportMaterial(
    UObject* Material, const FString& Filename, const FLightUSDUEOptions& Options)
{
    if (!Material) {
        FLightUSDUEResult Result;
        Result.Error = TEXT("Material object is null.");
        return Result;
    }
    const FString Code = FString::Printf(
        TEXT("import json, lightusd_ue.api as api; "
             "_lightusd_ue_result=json.dumps(api.export_material("
             "unreal.load_object(None,%s), %s, preserve_ue_config=%s, prefer_materialx=%s).__dict__)"),
        *PythonQuote(Material->GetPathName()), *PythonQuote(Filename),
        Options.bPreserveUEConfig ? TEXT("True") : TEXT("False"),
        Options.bPreferMaterialX ? TEXT("True") : TEXT("False"));
    return RunMaterialPython(Code, Filename);
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::ImportMaterial(
    const FString& Filename, const FLightUSDUEOptions& Options)
{
    const FString PackagePath = Options.PackagePath.IsEmpty()
        ? TEXT("/Game/LightUSD/Materials") : Options.PackagePath;
    const FString Code = FString::Printf(
        TEXT("import json, lightusd_ue.api as api; "
             "_lightusd_ue_result=json.dumps(api.import_material(%s, %s).__dict__)"),
        *PythonQuote(Filename), *PythonQuote(PackagePath));
    return RunMaterialPython(Code, Filename);
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::ValidateUSD(const FString& Filename,
                                                            const FLightUSDUEOptions& Options)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = ELightUSDUEBackend::LightUSD;
    Result.RootLayer = Filename;
    lightusd_stage* Stage = nullptr;
    if (!LoadStage(Filename, Options, &Stage, Result))
        return Result;
    for (size_t Index = 0; Index < lightusd_stage_root_prim_count(Stage); ++Index)
        CollectPrim(lightusd_stage_root_prim(Stage, Index), Result);
    Result.bSucceeded = true;
    lightusd_stage_destroy(Stage);
    return Result;
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::CreateSkeletalAnimation(
    const FString& Name, USkeleton* Skeleton, const TArray<FLightUSDUEBoneTrack>& Tracks,
    const FString& PackagePath, int32 FrameRate, int32 NumFrames,
    USkeletalMesh* PreviewMesh)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = ELightUSDUEBackend::NativeUE;
    if (!Skeleton)
    {
        Result.Error = TEXT("CreateSkeletalAnimation: target skeleton is null");
        return Result;
    }
    if (PackagePath.IsEmpty() || Name.IsEmpty() || FrameRate <= 0 || NumFrames <= 0)
    {
        Result.Error = TEXT("CreateSkeletalAnimation: invalid name, package path, frame rate, or frame count");
        return Result;
    }

    FString UniquePackageName;
    FString UniqueAssetName;
    FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
    AssetToolsModule.Get().CreateUniqueAssetName(
        PackagePath / Name, TEXT(""), UniquePackageName, UniqueAssetName);

    UAnimSequenceFactory* Factory = NewObject<UAnimSequenceFactory>();
    Factory->TargetSkeleton = Skeleton;
    Factory->PreviewSkeletalMesh = PreviewMesh;
    UAnimSequence* Sequence = Cast<UAnimSequence>(AssetToolsModule.Get().CreateAsset(
        UniqueAssetName, FPackageName::GetLongPackagePath(UniquePackageName),
        UAnimSequence::StaticClass(), Factory));
    if (!Sequence)
    {
        Result.Error = TEXT("CreateSkeletalAnimation: AnimSequenceFactory failed");
        return Result;
    }

    IAnimationDataController& Controller = Sequence->GetController();
    Controller.SetFrameRate(FFrameRate(FrameRate, 1));
    Controller.SetNumberOfFrames(FFrameNumber(NumFrames));
    for (const FLightUSDUEBoneTrack& Track : Tracks)
    {
        if (Track.BoneName.IsNone())
            continue;
        FName CanonicalBoneName = Track.BoneName;
        const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
        int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(CanonicalBoneName);
        if (BoneIndex == INDEX_NONE)
        {
            for (int32 Candidate = 0; Candidate < ReferenceSkeleton.GetNum(); ++Candidate)
            {
                if (ReferenceSkeleton.GetBoneName(Candidate).ToString().Equals(
                        CanonicalBoneName.ToString(), ESearchCase::IgnoreCase))
                {
                    BoneIndex = Candidate;
                    CanonicalBoneName = ReferenceSkeleton.GetBoneName(Candidate);
                    break;
                }
            }
        }
        if (BoneIndex == INDEX_NONE)
        {
            Result.Warnings.Add(FString::Printf(TEXT("Skipped unknown skeleton bone '%s'"),
                                                *CanonicalBoneName.ToString()));
            continue;
        }
        Controller.AddBoneCurve(CanonicalBoneName);
        Controller.SetBoneTrackKeys(CanonicalBoneName, Track.Translations,
                                    Track.Rotations, Track.Scales);
    }
    Controller.NotifyPopulated();
    Sequence->MarkPackageDirty();
    if (GEditor)
    {
        if (UEditorAssetSubsystem* AssetSubsystem =
                GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
        {
            AssetSubsystem->SaveAsset(Sequence->GetPathName(), false);
        }
    }
    Result.CreatedAssets.Add(Sequence->GetPathName());
    Result.bSucceeded = true;
    return Result;
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::AddSkeletalAnimationCurves(
    UAnimSequence* Sequence, const TArray<FLightUSDUEFloatCurve>& Curves)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = ELightUSDUEBackend::NativeUE;
    if (!Sequence)
    {
        Result.Error = TEXT("AddSkeletalAnimationCurves: sequence is null");
        return Result;
    }
    IAnimationDataController& Controller = Sequence->GetController();
    for (const FLightUSDUEFloatCurve& Curve : Curves)
    {
        if (Curve.CurveName.IsNone() || Curve.Times.Num() != Curve.Values.Num())
            continue;
        const FAnimationCurveIdentifier Identifier(Curve.CurveName, ERawCurveTrackTypes::RCT_Float);
        Controller.AddCurve(Identifier);
        if (USkeleton* Skeleton = Sequence->GetSkeleton())
        {
            Skeleton->AddCurveMetaData(Curve.CurveName, false);
            Skeleton->SetCurveMetaDataMorphTarget(Curve.CurveName, true);
            Skeleton->MarkPackageDirty();
        }
        TArray<FRichCurveKey> Keys;
        Keys.Reserve(Curve.Times.Num());
        for (int32 Index = 0; Index < Curve.Times.Num(); ++Index)
            Keys.Emplace(Curve.Times[Index], Curve.Values[Index]);
        Controller.SetCurveKeys(Identifier, Keys);
    }
    Controller.NotifyPopulated();
    Sequence->MarkPackageDirty();
    if (GEditor)
    {
        if (UEditorAssetSubsystem* AssetSubsystem =
                GEditor->GetEditorSubsystem<UEditorAssetSubsystem>())
            AssetSubsystem->SaveAsset(Sequence->GetPathName(), false);
    }
    Result.CreatedAssets.Add(Sequence->GetPathName());
    Result.bSucceeded = true;
    return Result;
}

void ULightUSDUEBlueprintLibrary::GetSkeletalAnimationCurves(
    UAnimSequence* Sequence, TArray<FLightUSDUEFloatCurve>& Curves)
{
    Curves.Reset();
    if (!Sequence || !Sequence->GetDataModel())
        return;
    USkeleton* Skeleton = Sequence->GetSkeleton();
    for (const FFloatCurve& SourceCurve : Sequence->GetDataModel()->GetFloatCurves())
    {
        if (Skeleton && !Skeleton->GetCurveMetaDataMorphTarget(SourceCurve.GetName()))
            continue;
        FLightUSDUEFloatCurve Curve;
        Curve.CurveName = SourceCurve.GetName();
        SourceCurve.GetKeys(Curve.Times, Curve.Values);
        if (Curve.Times.Num() == Curve.Values.Num() && Curve.Times.Num() > 0)
            Curves.Add(MoveTemp(Curve));
    }
}

namespace
{
static bool AnimationViewAt(lightusd_prim Prim, const char* Name, size_t Index,
                            double Time, lightusd_value_view* OutView,
                            lightusd_value** OwnedValue)
{
    *OwnedValue = nullptr;
    if (lightusd_attr_timesample_count(Prim, Name) > Index)
    {
        double SampleTime = 0.0;
        if (lightusd_attr_timesample_at(Prim, Name, Index, &SampleTime, OutView) == LIGHTUSD_OK &&
            FMath::IsNearlyEqual(SampleTime, Time))
            return true;
    }
    if (lightusd_attr_interpolate(Prim, Name, Time, 0, OwnedValue) == LIGHTUSD_OK)
        return lightusd_value_get_view(*OwnedValue, OutView) == LIGHTUSD_OK;
    return lightusd_attr_get(Prim, Name, OutView) == LIGHTUSD_OK;
}

static bool ReadAnimationUSDView(lightusd_prim Prim, const char* Name, size_t Index,
                                 double Time, int32 ExpectedComponents,
                                 TArray<FVector>& OutVectors, TArray<FQuat>& OutQuats)
{
    lightusd_value_view View{};
    lightusd_value* Owned = nullptr;
    if (!AnimationViewAt(Prim, Name, Index, Time, &View, &Owned))
        return false;
    const bool IsQuat = ExpectedComponents == 4;
    if (!View.data || View.components != ExpectedComponents ||
        View.storage != LIGHTUSD_COMP_FLOAT32 || View.count == 0)
    {
        if (Owned) lightusd_value_destroy(Owned);
        return false;
    }
    const float* Data = static_cast<const float*>(View.data);
    if (IsQuat)
    {
        OutQuats.SetNum(static_cast<int32>(View.count));
        for (size_t I = 0; I < View.count; ++I)
            OutQuats[static_cast<int32>(I)] = FQuat(Data[I * 4 + 1], Data[I * 4 + 2],
                                                   Data[I * 4 + 3], Data[I * 4 + 0]);
    }
    else
    {
        OutVectors.SetNum(static_cast<int32>(View.count));
        for (size_t I = 0; I < View.count; ++I)
            OutVectors[static_cast<int32>(I)] = FVector(Data[I * 3 + 0], Data[I * 3 + 1],
                                                        Data[I * 3 + 2]);
    }
    if (Owned) lightusd_value_destroy(Owned);
    return true;
}
}

FLightUSDUEResult ULightUSDUEBlueprintLibrary::ImportSkeletalAnimation(
    const FString& Filename, const FString& Name, USkeleton* Skeleton,
    const FString& PackagePath, USkeletalMesh* PreviewMesh)
{
    FLightUSDUEResult Result;
    Result.BackendUsed = ELightUSDUEBackend::LightUSD;
    if (!Skeleton)
    {
        Result.Error = TEXT("ImportSkeletalAnimation: target skeleton is null");
        return Result;
    }
    lightusd_load_options Options;
    lightusd_load_options_init(&Options);
    Options.composed = 1;
    lightusd_stage* Stage = nullptr;
    const FTCHARToUTF8 Path(*Filename);
    if (lightusd_stage_load(Path.Get(), &Options, &Stage) != LIGHTUSD_OK)
    {
        SetLightUSDError(Result, TEXT("ImportSkeletalAnimation: USD load failed"));
        return Result;
    }

    lightusd_prim AnimationPrim{};
    TFunction<void(lightusd_prim)> FindAnimation = [&](lightusd_prim Prim)
    {
        if (!lightusd_prim_is_valid(Prim) || lightusd_prim_is_active(Prim) == 0)
            return;
        if (SV(lightusd_prim_type_name(Prim)) == TEXT("SkelAnimation"))
        {
            AnimationPrim = Prim;
            return;
        }
        for (size_t I = 0; I < lightusd_prim_child_count(Prim) &&
             !lightusd_prim_is_valid(AnimationPrim); ++I)
            FindAnimation(lightusd_prim_child(Prim, I));
    };
    for (size_t I = 0; I < lightusd_stage_root_prim_count(Stage) &&
         !lightusd_prim_is_valid(AnimationPrim); ++I)
        FindAnimation(lightusd_stage_root_prim(Stage, I));
    if (!lightusd_prim_is_valid(AnimationPrim))
    {
        lightusd_stage_destroy(Stage);
        Result.Error = TEXT("ImportSkeletalAnimation: no SkelAnimation prim found");
        return Result;
    }

    lightusd_strlist* JointList = nullptr;
    if (lightusd_attr_get_token_array(AnimationPrim, "joints", &JointList) != LIGHTUSD_OK ||
        !JointList || lightusd_strlist_size(JointList) == 0)
    {
        if (JointList) lightusd_strlist_destroy(JointList);
        lightusd_stage_destroy(Stage);
        Result.Error = TEXT("ImportSkeletalAnimation: SkelAnimation has no joints");
        return Result;
    }

    const char* SampleAttribute = nullptr;
    for (const char* Candidate : {"rotations", "translations", "scales"})
    {
        if (lightusd_attr_timesample_count(AnimationPrim, Candidate) > 0)
        {
            SampleAttribute = Candidate;
            break;
        }
    }
    const size_t SampleCount = SampleAttribute
        ? lightusd_attr_timesample_count(AnimationPrim, SampleAttribute) : 0;
    if (SampleCount == 0)
    {
        lightusd_strlist_destroy(JointList);
        lightusd_stage_destroy(Stage);
        Result.Error = TEXT("ImportSkeletalAnimation: animation has no time samples");
        return Result;
    }
    TArray<double> SampleTimes;
    SampleTimes.SetNum(static_cast<int32>(SampleCount));
    lightusd_attr_timesample_times(AnimationPrim, SampleAttribute, SampleTimes.GetData(), SampleCount);
    TArray<FLightUSDUEBoneTrack> Tracks;
    Tracks.Reserve(static_cast<int32>(lightusd_strlist_size(JointList)));
    for (size_t JointIndex = 0; JointIndex < lightusd_strlist_size(JointList); ++JointIndex)
    {
        const FString JointPath = SV(lightusd_strlist_get(JointList, JointIndex));
        const int32 Slash = JointPath.Find(TEXT("/"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
        FLightUSDUEBoneTrack Track;
        Track.BoneName = FName(Slash == INDEX_NONE ? JointPath : JointPath.Mid(Slash + 1));
        for (size_t SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
        {
            TArray<FVector> Vectors;
            TArray<FQuat> Quats;
            const bool HaveTranslation = ReadAnimationUSDView(
                AnimationPrim, "translations", SampleIndex, SampleTimes[static_cast<int32>(SampleIndex)],
                3, Vectors, Quats);
            Track.Translations.Add(HaveTranslation && Vectors.IsValidIndex(static_cast<int32>(JointIndex))
                ? Vectors[static_cast<int32>(JointIndex)] : FVector::ZeroVector);
            Vectors.Reset(); Quats.Reset();
            const bool HaveRotation = ReadAnimationUSDView(
                AnimationPrim, "rotations", SampleIndex, SampleTimes[static_cast<int32>(SampleIndex)],
                4, Vectors, Quats);
            Track.Rotations.Add(HaveRotation && Quats.IsValidIndex(static_cast<int32>(JointIndex))
                ? Quats[static_cast<int32>(JointIndex)] : FQuat::Identity);
            Vectors.Reset(); Quats.Reset();
            const bool HaveScale = ReadAnimationUSDView(
                AnimationPrim, "scales", SampleIndex, SampleTimes[static_cast<int32>(SampleIndex)],
                3, Vectors, Quats);
            Track.Scales.Add(HaveScale && Vectors.IsValidIndex(static_cast<int32>(JointIndex))
                ? Vectors[static_cast<int32>(JointIndex)] : FVector::OneVector);
        }
        Tracks.Add(MoveTemp(Track));
    }
    TArray<FLightUSDUEFloatCurve> Curves;
    lightusd_strlist* BlendShapeList = nullptr;
    if (lightusd_attr_get_token_array(AnimationPrim, "blendShapes", &BlendShapeList) == LIGHTUSD_OK &&
        BlendShapeList && lightusd_strlist_size(BlendShapeList) > 0 &&
        lightusd_attr_timesample_count(AnimationPrim, "blendShapeWeights") > 0)
    {
        const size_t WeightSampleCount = lightusd_attr_timesample_count(AnimationPrim, "blendShapeWeights");
        TArray<double> WeightTimes;
        WeightTimes.SetNum(static_cast<int32>(WeightSampleCount));
        lightusd_attr_timesample_times(AnimationPrim, "blendShapeWeights",
                                       WeightTimes.GetData(), WeightSampleCount);
        for (size_t BlendIndex = 0; BlendIndex < lightusd_strlist_size(BlendShapeList); ++BlendIndex)
        {
            FLightUSDUEFloatCurve Curve;
            Curve.CurveName = FName(SV(lightusd_strlist_get(BlendShapeList, BlendIndex)));
            for (size_t SampleIndex = 0; SampleIndex < WeightSampleCount; ++SampleIndex)
            {
                lightusd_value_view View{};
                lightusd_value* Owned = nullptr;
                if (!AnimationViewAt(AnimationPrim, "blendShapeWeights", SampleIndex,
                                     WeightTimes[static_cast<int32>(SampleIndex)], &View, &Owned) ||
                    !View.data || View.components != 1 || View.storage != LIGHTUSD_COMP_FLOAT32 ||
                    !View.count || BlendIndex >= View.count)
                {
                    if (Owned) lightusd_value_destroy(Owned);
                    continue;
                }
                Curve.Times.Add(static_cast<float>(WeightTimes[static_cast<int32>(SampleIndex)]));
                Curve.Values.Add(static_cast<const float*>(View.data)[BlendIndex]);
                if (Owned) lightusd_value_destroy(Owned);
            }
            if (Curve.Times.Num() > 0)
                Curves.Add(MoveTemp(Curve));
        }
    }
    const int32 FrameRate = 30;
    const int32 NumFrames = static_cast<int32>(FMath::RoundToInt(SampleTimes.Last())) + 1;
    const FLightUSDUEResult Created = CreateSkeletalAnimation(
        Name, Skeleton, Tracks, PackagePath, FrameRate, FMath::Max(NumFrames, 1), PreviewMesh);
    FLightUSDUEResult Final = Created;
    if (Created.bSucceeded && Curves.Num() > 0)
    {
        UAnimSequence* Sequence = LoadObject<UAnimSequence>(nullptr, *Created.CreatedAssets[0]);
        Final = AddSkeletalAnimationCurves(Sequence, Curves);
    }
    Final.BackendUsed = ELightUSDUEBackend::LightUSD;
    if (BlendShapeList) lightusd_strlist_destroy(BlendShapeList);
    lightusd_strlist_destroy(JointList);
    lightusd_stage_destroy(Stage);
    return Final;
}

bool ULightUSDUEBlueprintLibrary::IsNativeBackendAvailable()
{
    return FModuleManager::Get().IsModuleLoaded(TEXT("USDStageImporter")) ||
           FModuleManager::Get().ModuleExists(TEXT("USDStageImporter"));
}

class FLightUSDUE final : public IModuleInterface
{
public:
    virtual void StartupModule() override {}
    virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FLightUSDUE, LightUSDUE)
