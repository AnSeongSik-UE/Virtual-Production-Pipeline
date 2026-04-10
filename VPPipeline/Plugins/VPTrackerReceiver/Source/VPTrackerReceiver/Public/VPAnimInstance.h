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
 * - Pose landmarks -> Bone Transform array
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

	/** Auto-find UVPUDPReceiver on the owning actor and feed data each frame */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Tracking")
	bool bAutoFindReceiver = true;

	/** Apply a complete tracking frame (blendshapes + pose) */
	UFUNCTION(BlueprintCallable, Category = "VP Pipeline")
	void ApplyTrackingData(const FVPTrackingFrame& Frame);

	/** Apply blendshapes as morph targets on the owning skeletal mesh */
	UFUNCTION(BlueprintCallable, Category = "VP Tracking")
	void ApplyBlendshapesToMorphTargets();

	/** Convert pose landmarks to bone transforms array */
	UFUNCTION(BlueprintCallable, Category = "VP Tracking")
	void UpdatePoseBoneTransforms();

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
};
