#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "LightUSDUE.generated.h"

class USkeleton;
class USkeletalMesh;
class UAnimSequence;

UENUM(BlueprintType)
enum class ELightUSDUEBackend : uint8
{
    Auto UMETA(DisplayName="Auto"),
    NativeUE UMETA(DisplayName="Unreal USD"),
    LightUSD UMETA(DisplayName="LightUSD")
};

USTRUCT(BlueprintType)
struct LIGHTUSDUE_API FLightUSDUEOptions
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") ELightUSDUEBackend Backend = ELightUSDUEBackend::Auto;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bImportGeometry = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bImportMaterials = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bImportPhysics = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bImportSkeletalAnimation = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bImportGroom = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bCompose = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bLoadPayloads = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bPreserveCustomData = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") bool bRelinkMetaHuman = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Material") bool bExportMaterialGraph = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Material") bool bPreferMaterialX = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Material") bool bPreserveUEConfig = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") int32 MaxMemoryMB = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") double TimeCode = 0.0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") FString PackagePath;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") FString MetaHumanAssetPath;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Groom") FString GroomTargetSkeletalMeshPath;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") FString RenderContext = TEXT("universal");
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|USD") FString MaterialPurpose = TEXT("render");
};

USTRUCT(BlueprintType)
struct LIGHTUSDUE_API FLightUSDUEResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category="LightUSD|USD") bool bSucceeded = false;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|USD") ELightUSDUEBackend BackendUsed = ELightUSDUEBackend::Auto;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|USD") FString Error;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|USD") TArray<FString> Warnings;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|USD") TArray<FString> CreatedAssets;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|USD") TArray<FString> PrimPaths;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|USD") FString RootLayer;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|Material") int32 MaterialGraphNodes = 0;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|Material") int32 MaterialXNodes = 0;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|Material") TArray<FString> UnsupportedMaterialNodes;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|Groom") int32 GroomAssets = 0;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|Groom") int32 GroomBindings = 0;
    UPROPERTY(BlueprintReadOnly, Category="LightUSD|Groom") int32 GroomCaches = 0;
};

USTRUCT(BlueprintType)
struct LIGHTUSDUE_API FLightUSDUEBoneTrack
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Animation") FName BoneName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Animation") TArray<FVector> Translations;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Animation") TArray<FQuat> Rotations;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Animation") TArray<FVector> Scales;
};

USTRUCT(BlueprintType)
struct LIGHTUSDUE_API FLightUSDUEFloatCurve
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Animation") FName CurveName;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Animation") TArray<float> Times;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="LightUSD|Animation") TArray<float> Values;
};

UCLASS()
class LIGHTUSDUE_API ULightUSDUEBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()

public:
    UFUNCTION(BlueprintCallable, Category="LightUSD|USD")
    static FLightUSDUEResult ImportUSD(const FString& Filename, const FLightUSDUEOptions& Options);

    UFUNCTION(BlueprintCallable, Category="LightUSD|USD")
    static FLightUSDUEResult ExportUSD(const FString& Filename, const FLightUSDUEOptions& Options);

    UFUNCTION(BlueprintCallable, Category="LightUSD|Groom")
    static FLightUSDUEResult ExportGroom(UObject* GroomAsset, const FString& Filename,
                                         const FLightUSDUEOptions& Options);

    UFUNCTION(BlueprintCallable, Category="LightUSD|Groom")
    static FLightUSDUEResult ExportGroomCache(UObject* GroomCache, UObject* GroomAsset,
                                              const FString& Filename,
                                              const FLightUSDUEOptions& Options);

    /** Export a UMaterial, UMaterialInstance, or UMaterialFunction graph using
     * the LightUSD MaterialX-first bridge. */
    UFUNCTION(BlueprintCallable, Category="LightUSD|Material")
    static FLightUSDUEResult ExportMaterial(UObject* Material, const FString& Filename,
                                             const FLightUSDUEOptions& Options);

    /** Import a LightUSD UE/MaterialX material graph into a UE Material asset. */
    UFUNCTION(BlueprintCallable, Category="LightUSD|Material")
    static FLightUSDUEResult ImportMaterial(const FString& Filename,
                                             const FLightUSDUEOptions& Options);

    UFUNCTION(BlueprintCallable, Category="LightUSD|USD")
    static FLightUSDUEResult ValidateUSD(const FString& Filename, const FLightUSDUEOptions& Options);

    /** Create and save a UE AnimSequence using the native animation data controller. */
    UFUNCTION(BlueprintCallable, Category="LightUSD|Animation")
    static FLightUSDUEResult CreateSkeletalAnimation(
        const FString& Name, USkeleton* Skeleton, const TArray<FLightUSDUEBoneTrack>& Tracks,
        const FString& PackagePath, int32 FrameRate = 30, int32 NumFrames = 1,
        USkeletalMesh* PreviewMesh = nullptr);

    /** Read a USD SkelAnimation with LightUSD and save it as a UE AnimSequence. */
    UFUNCTION(BlueprintCallable, Category="LightUSD|Animation")
    static FLightUSDUEResult ImportSkeletalAnimation(
        const FString& Filename, const FString& Name, USkeleton* Skeleton,
        const FString& PackagePath, USkeletalMesh* PreviewMesh = nullptr);

    /** Add facial/morph animation curves to an existing UE AnimSequence. */
    UFUNCTION(BlueprintCallable, Category="LightUSD|Animation")
    static FLightUSDUEResult AddSkeletalAnimationCurves(
        UAnimSequence* Sequence, const TArray<FLightUSDUEFloatCurve>& Curves);

    /** Return float curves authored on an AnimSequence, including morph metadata. */
    UFUNCTION(BlueprintCallable, Category="LightUSD|Animation")
    static void GetSkeletalAnimationCurves(
        UAnimSequence* Sequence, TArray<FLightUSDUEFloatCurve>& Curves);

    UFUNCTION(BlueprintPure, Category="LightUSD|USD")
    static bool IsNativeBackendAvailable();
};
