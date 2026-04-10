#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "VPTrackingData.generated.h"

/**
 * DataTable row: ARKit blendshape name -> avatar Morph Target name
 * Use this to remap ARKit names to your avatar's morph target naming convention.
 */
USTRUCT(BlueprintType)
struct VPTRACKERRECEIVER_API FVPBlendshapeMapping : public FTableRowBase
{
	GENERATED_BODY()

	/** ARKit blendshape name (e.g. "eyeBlinkLeft") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Mapping")
	FName ARKitName;

	/** Target morph target name on the avatar (e.g. "EyeBlink_L") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Mapping")
	FName MorphTargetName;

	/** Weight scale factor (default 1.0) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Mapping", meta = (ClampMin = "0.0", ClampMax = "3.0"))
	float Scale = 1.0f;
};

/**
 * ARKit 52 blendshape weights from MediaPipe FaceLandmarker
 */
USTRUCT(BlueprintType)
struct VPTRACKERRECEIVER_API FVPBlendshapeData
{
	GENERATED_BODY()

	/** ARKit blendshape name -> weight (0.0 ~ 1.0) */
	UPROPERTY(BlueprintReadOnly, Category = "VP Tracking")
	TMap<FName, float> Blendshapes;

	/** Number of valid blendshapes */
	int32 Num() const { return Blendshapes.Num(); }
};

/**
 * Single pose landmark from MediaPipe PoseLandmarker
 */
USTRUCT(BlueprintType)
struct VPTRACKERRECEIVER_API FVPPoseLandmark
{
	GENERATED_BODY()

	/** Normalized position (0~1) */
	UPROPERTY(BlueprintReadOnly, Category = "VP Tracking")
	FVector Position = FVector::ZeroVector;
};

/**
 * Complete tracking frame containing face + pose data
 * Matches the binary packet format from Python sender.py
 */
USTRUCT(BlueprintType)
struct VPTRACKERRECEIVER_API FVPTrackingFrame
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VP Tracking")
	double Timestamp = 0.0;

	UPROPERTY(BlueprintReadOnly, Category = "VP Tracking")
	FVPBlendshapeData FaceData;

	/** 33 MediaPipe pose landmarks */
	UPROPERTY(BlueprintReadOnly, Category = "VP Tracking")
	TArray<FVPPoseLandmark> PoseLandmarks;

	UPROPERTY(BlueprintReadOnly, Category = "VP Tracking")
	bool bIsValid = false;
};
