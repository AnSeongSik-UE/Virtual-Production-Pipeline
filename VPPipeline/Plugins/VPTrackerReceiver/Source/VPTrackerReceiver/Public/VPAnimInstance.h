#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "VPTrackingData.h"
#include "VPAnimInstance.generated.h"

class UVPUDPReceiver;
class UDataTable;

/**
 * Custom AnimInstance that applies tracking data to skeletal mesh.
 * - Blendshape -> Morph Target (with optional DataTable remapping)
 * - Pose landmarks -> Head bone rotation
 * Assign this as the Anim Class on your avatar's Skeletal Mesh Component.
 */
UCLASS()
class VPTRACKERRECEIVER_API UVPAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	/** Current tracking data (updated every frame) */
	UPROPERTY(BlueprintReadWrite, Category = "VP Tracking")
	FVPTrackingFrame TrackingData;

	/** Blend weight for facial morph targets (0.0 = off, 1.0 = full, up to 2.0 for exaggeration) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Tracking", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float FaceBlendWeight = 1.0f;

	/** Dead zone: blendshape values below this threshold are treated as 0 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Tracking", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float BlendshapeDeadZone = 0.15f;

	/**
	 * Optional DataTable (row type: FVPBlendshapeMapping) for ARKit -> Morph Target name remapping.
	 * If null, ARKit names are used directly as morph target names.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Tracking")
	TObjectPtr<UDataTable> BlendshapeMappingTable;

	/** Blendshapes after remapping (readable from Blueprint / AnimGraph) */
	UPROPERTY(BlueprintReadOnly, Category = "VP Face")
	TMap<FName, float> FaceBlendshapes;

	/** Pose landmark transforms (33 entries, position from MediaPipe, identity rotation) */
	UPROPERTY(BlueprintReadOnly, Category = "VP Pose")
	TArray<FTransform> PoseBoneTransforms;

	/** Position scale: converts normalized (0~1) MediaPipe coords to UE units */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Pose", meta = (ClampMin = "1.0", ClampMax = "1000.0"))
	float PosePositionScale = 100.0f;

	/**
	 * Rest pose morph targets applied as baseline every frame.
	 * Example: set "Fcl_MTH_Close" = 1.0 to keep mouth closed by default.
	 * Tracking values are added on top of these.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Tracking")
	TMap<FName, float> RestPoseMorphTargets;

	/** Auto-find UVPUDPReceiver on the owning actor and feed data each frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Tracking")
	bool bAutoFindReceiver = true;

	// --- Head Rotation Tracking ---

	/** Enable head rotation from pose landmarks */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head")
	bool bEnableHeadTracking = true;

	/** Head bone name on the avatar skeleton (VRoid: J_Bip_C_Head) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head")
	FName HeadBoneName = FName("J_Bip_C_Head");

	/** Head rotation sensitivity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float HeadRotationScale = 1.5f;

	/** Axis sign multipliers — set to -1 to invert direction (adjustable in editor without rebuild) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float YawSign = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float PitchSign = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float RollSign = -1.0f;

	/** Smoothing factor for head rotation (0 = no smoothing, 1 = max smoothing) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "0.0", ClampMax = "0.99"))
	float HeadRotationSmoothing = 0.5f;

	/** Max rotation change per frame (degrees) - prevents wild jumps from noisy landmarks */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "0.5", ClampMax = "20.0"))
	float HeadMaxDeltaPerFrame = 5.0f;

	/** Current head rotation (readable from Blueprint / AnimGraph) */
	UPROPERTY(BlueprintReadOnly, Category = "VP Head")
	FRotator HeadRotation;

	/** Apply a complete tracking frame (blendshapes + pose) */
	UFUNCTION(BlueprintCallable, Category = "VP Pipeline")
	void ApplyTrackingData(const FVPTrackingFrame& Frame);

	/** Apply blendshapes as morph targets on the owning skeletal mesh */
	UFUNCTION(BlueprintCallable, Category = "VP Tracking")
	void ApplyBlendshapesToMorphTargets();

	/** Convert pose landmarks to bone transforms array */
	UFUNCTION(BlueprintCallable, Category = "VP Tracking")
	void UpdatePoseBoneTransforms();

	/** Calculate and apply head rotation from pose landmarks */
	UFUNCTION(BlueprintCallable, Category = "VP Head")
	void UpdateHeadRotation();

	virtual void NativeInitializeAnimation() override;
	virtual void NativeUpdateAnimation(float DeltaSeconds) override;

private:
	/** Cached remapping: ARKit name -> (MorphTarget name, scale) */
	TMap<FName, TPair<FName, float>> CachedMapping;
	bool bMappingCached = false;
	void CacheMappingTable();

	/** Cached receiver reference */
	UPROPERTY()
	TObjectPtr<UVPUDPReceiver> CachedReceiver;

	/** Previous head rotation for smoothing */
	FRotator SmoothedHeadRotation = FRotator::ZeroRotator;

};
