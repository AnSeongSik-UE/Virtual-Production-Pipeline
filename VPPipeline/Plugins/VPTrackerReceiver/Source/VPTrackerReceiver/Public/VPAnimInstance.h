#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "VPTrackingData.h"
#include "VPTrackingProfile.h"
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

	/** Time-based smoothing speed for facial morph targets. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Face", meta = (ClampMin = "0.1", ClampMax = "60.0"))
	float FaceBlendshapeSmoothingSpeed = 18.0f;

	/** Brief face detection gaps shorter than this keep the last target. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Face", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FaceTrackingGraceSeconds = 0.15f;

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

	/** Enable head rotation from the FaceLandmarker transformation matrix. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head")
	bool bEnableHeadTracking = true;

	/** Head bone name on the avatar skeleton (VRoid: J_Bip_C_Head) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head")
	FName HeadBoneName = FName("J_Bip_C_Head");

	/** Head rotation sensitivity multiplier */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float HeadRotationScale = 1.0f;

	/** Axis sign multipliers — set to -1 to invert direction (adjustable in editor without rebuild) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float YawSign = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float PitchSign = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "-1.0", ClampMax = "1.0"))
	float RollSign = -1.0f;

	/** Time-based interpolation speed for head rotation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "0.1", ClampMax = "60.0"))
	float HeadRotationSmoothingSpeed = 8.0f;

	/** Maximum head rotation speed, independent of render frame rate. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "30.0", ClampMax = "720.0"))
	float HeadMaxDegreesPerSecond = 180.0f;

	/** Ignore calibrated micro-movements smaller than this angle. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float HeadRotationDeadZoneDegrees = 1.5f;

	/** Current head rotation (readable from Blueprint / AnimGraph) */
	UPROPERTY(BlueprintReadOnly, Category = "VP Head")
	FRotator HeadRotation;

	/** Speed used to return the head to neutral after tracking loss */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Head", meta = (ClampMin = "0.1", ClampMax = "30.0"))
	float TrackingLossReturnSpeed = 8.0f;

	// --- Upper Arm Tracking ---

	/** Enable shoulder-to-elbow upper-arm tracking. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm")
	bool bEnableUpperArmTracking = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm")
	FName LeftUpperArmBoneName = FName("J_Bip_L_UpperArm");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm")
	FName RightUpperArmBoneName = FName("J_Bip_R_UpperArm");

	/** Confidence required to acquire an upper arm that is currently untracked. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float UpperArmConfidenceThreshold = 0.5f;

	/** Lower confidence threshold used after an upper arm has been acquired. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float UpperArmReleaseConfidenceThreshold = 0.3f;

	/** Hold the last stable arm angle across shorter confidence dropouts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float UpperArmTrackingGraceSeconds = 0.25f;

	/** Local rotation axis used by the avatar's upper-arm Transform Modify Bone nodes. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm")
	EVPArmRotationAxis UpperArmRotationAxis = EVPArmRotationAxis::Roll;

	/** Swap the left/right tracking inputs before applying avatar-specific mapping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm")
	bool bSwapUpperArms = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm")
	bool bInvertLeftUpperArm = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm")
	bool bInvertRightUpperArm = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "-2.0", ClampMax = "2.0"))
	float LeftUpperArmSign = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "-2.0", ClampMax = "2.0"))
	float RightUpperArmSign = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "0.25", ClampMax = "2.0"))
	float LeftUpperArmGain = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "0.25", ClampMax = "2.0"))
	float RightUpperArmGain = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "-90.0", ClampMax = "0.0"))
	float UpperArmMinAngle = -30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "0.0", ClampMax = "180.0"))
	float UpperArmMaxAngle = 140.0f;

	/** Time-based interpolation speed for movement and neutral recovery. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "0.1", ClampMax = "30.0"))
	float UpperArmSmoothingSpeed = 10.0f;

	/** Time-based smoothing applied to raw shoulder-to-elbow input angles. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "0.1", ClampMax = "60.0"))
	float UpperArmInputSmoothingSpeed = 12.0f;

	/** Reject the first two input changes larger than this value, then accept a persistent change. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Upper Arm", meta = (ClampMin = "30.0", ClampMax = "180.0"))
	float UpperArmMaxInputJump = 120.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm")
	FRotator LeftUpperArmRotation;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm")
	FRotator RightUpperArmRotation;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm")
	float LeftUpperArmConfidence = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm")
	float RightUpperArmConfidence = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm")
	bool bLeftUpperArmTracked = false;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm")
	bool bRightUpperArmTracked = false;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	float LeftUpperArmInputAngle = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	float RightUpperArmInputAngle = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	float LeftUpperArmOutputAngle = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	float RightUpperArmOutputAngle = 0.0f;

	/** Smoothed angle before avatar-specific bone-axis conversion. */
	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	float LeftUpperArmAppliedAngle = 0.0f;

	/** Smoothed angle before avatar-specific bone-axis conversion. */
	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	float RightUpperArmAppliedAngle = 0.0f;

	/** Smoothed forward swing derived from shoulder-to-elbow depth. */
	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	float LeftUpperArmAppliedForwardAngle = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	float RightUpperArmAppliedForwardAngle = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Upper Arm|Diagnostics")
	int32 RejectedUpperArmInputCount = 0;

	// --- Avatar Profile / Calibration ---

	/** Stable per-avatar key. Empty uses the current skeletal-mesh asset path. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Profile")
	FString AvatarTrackingProfileId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Profile")
	bool bAutoLoadTrackingProfile = true;

	UPROPERTY(BlueprintReadOnly, Category = "VP Calibration")
	bool bHasNeutralCalibration = false;

	UPROPERTY(BlueprintReadOnly, Category = "VP Calibration")
	EVPCalibrationState CalibrationState = EVPCalibrationState::Idle;

	UPROPERTY(BlueprintReadOnly, Category = "VP Calibration")
	float CalibrationRemainingSeconds = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "VP Calibration")
	FString CalibrationStatus;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Calibration", meta = (ClampMin = "1.0", ClampMax = "10.0"))
	float CalibrationCountdownSeconds = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Calibration", meta = (ClampMin = "1.0", ClampMax = "15.0"))
	float CalibrationTrackingRecoverySeconds = 5.0f;

	/** Minimum number of unique stable tracking frames used for neutral calibration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Calibration", meta = (ClampMin = "3", ClampMax = "120"))
	int32 CalibrationMinimumSamples = 10;

	// --- Guided Arm Validation ---

	UPROPERTY(BlueprintReadOnly, Category = "VP Validation")
	EVPArmValidationStage ArmValidationStage = EVPArmValidationStage::Idle;

	UPROPERTY(BlueprintReadOnly, Category = "VP Validation")
	FString ArmValidationStatus;

	UPROPERTY(BlueprintReadOnly, Category = "VP Validation")
	float ArmValidationProgress = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Validation", meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float ValidationPoseHoldSeconds = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Validation", meta = (ClampMin = "2.0", ClampMax = "30.0"))
	float ValidationStageTimeoutSeconds = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Validation", meta = (ClampMin = "1.0", ClampMax = "45.0"))
	float ValidationNeutralTolerance = 15.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Validation", meta = (ClampMin = "10.0", ClampMax = "120.0"))
	float ValidationRaisedThreshold = 35.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Validation", meta = (ClampMin = "1.0", ClampMax = "60.0"))
	float ValidationOtherArmTolerance = 25.0f;

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

	/** Update left/right upper-arm outputs from MediaPipe shoulders and elbows. */
	UFUNCTION(BlueprintCallable, Category = "VP Upper Arm")
	void UpdateUpperArmRotations(float DeltaSeconds);

	UFUNCTION(BlueprintCallable, Category = "VP Calibration")
	bool StartNeutralCalibration();

	UFUNCTION(BlueprintCallable, Category = "VP Calibration")
	void CancelNeutralCalibration();

	UFUNCTION(BlueprintCallable, Category = "VP Profile")
	bool LoadTrackingProfile();

	UFUNCTION(BlueprintCallable, Category = "VP Profile")
	bool SaveTrackingProfile();

	UFUNCTION(BlueprintCallable, Category = "VP Profile")
	void ResetTrackingProfile();

	/** Permanently removes the calibration and mapping profile for one avatar id. */
	static bool DeleteTrackingProfileById(const FString& ProfileId);

	/** Removes content-addressed profiles whose avatar ids no longer exist. */
	static bool PruneManagedTrackingProfiles(
		const TSet<FString>& ValidAvatarIds,
		int32& OutRemovedCount);
	static int32 RemoveManagedTrackingProfilesNotIn(
		TMap<FString, FVPAvatarTrackingProfile>& Profiles,
		const TSet<FString>& ValidAvatarIds);

	/** Update common mapping toggles and persist them. Swapping also swaps saved neutral inputs. */
	UFUNCTION(BlueprintCallable, Category = "VP Profile")
	void SetUpperArmMappingOptions(bool bSwap, bool bInvertLeft, bool bInvertRight);

	UFUNCTION(BlueprintCallable, Category = "VP Validation")
	bool StartArmValidation();

	UFUNCTION(BlueprintCallable, Category = "VP Validation")
	void CancelArmValidation();

	/** Pure shoulder-to-elbow angle calculation used by runtime code and tests. */
	static bool CalculateUpperArmAngle(
		const FVPTrackingFrame& Frame,
		bool bLeft,
		float ConfidenceThreshold,
		float& OutAngleDegrees,
		float& OutConfidence);

	/** 3D shoulder-to-elbow target with an inward torso safety constraint. */
	static bool CalculateUpperArmPose(
		const FVPTrackingFrame& Frame,
		bool bLeft,
		float ConfidenceThreshold,
		float& OutElevationDegrees,
		float& OutForwardDegrees,
		float& OutConfidence,
		bool& bOutInward);

	/** Head roll from facial pose landmarks, independent of shoulder motion. */
	static bool CalculateHeadRollDegrees(
		const FVPTrackingFrame& Frame,
		float ConfidenceThreshold,
		float& OutRollDegrees);

	/** Pure input mapping used by runtime code and automation tests. */
	static float MapUpperArmOutput(
		float InputAngle,
		float NeutralInputAngle,
		float Gain,
		bool bInvert,
		float MinAngle,
		float MaxAngle);

	/** One deterministic filter step. False means a one-frame outlier was held. */
	static bool FilterUpperArmInputStep(
		float RawAngle,
		float DeltaSeconds,
		float SmoothingSpeed,
		float MaxInputJump,
		float& InOutFilteredAngle,
		float& InOutLastRawAngle,
		int32& InOutConsecutiveRejectedFrames,
		bool& bInOutInitialized,
		float& OutFilteredAngle);

	/** Confidence hysteresis and dropout grace step used by runtime and tests. */
	static bool UpdateArmTrackingStateStep(
		bool bWasTracked,
		bool bRawSampleValid,
		float Confidence,
		float DeltaSeconds,
		float AcquireThreshold,
		float ReleaseThreshold,
		float GraceSeconds,
		float& InOutBelowThresholdSeconds,
		bool& bOutFreshSample);

	/** Circular mean for robust calibration of wrapped Euler angles. */
	static float CalculateCircularMeanDegrees(const TArray<float>& Samples);

	/** Deterministic frame-rate-independent head smoothing step. */
	static FRotator SmoothHeadRotationStep(
		const FRotator& Current,
		const FRotator& Target,
		float DeltaSeconds,
		float SmoothingSpeed,
		float MaxDegreesPerSecond);

	/** Removes small calibrated Euler noise without introducing a discontinuity. */
	static FRotator ApplyHeadRotationDeadZone(
		const FRotator& Rotation,
		float DeadZoneDegrees);

	/** Pure validation predicate used by runtime code and automation tests. */
	static bool IsArmValidationPoseSatisfied(
		EVPArmValidationStage Stage,
		float LeftOutputAngle,
		float RightOutputAngle,
		float NeutralTolerance,
		float RaisedThreshold,
		float OtherArmTolerance);

	/** Actionable live correction text used by runtime validation and automation tests. */
	static FString BuildArmValidationFeedback(
		EVPArmValidationStage Stage,
		bool bLeftTracked,
		bool bRightTracked,
		float LeftOutputAngle,
		float RightOutputAngle,
		float LeftConfidence,
		float RightConfidence,
		float AcquireConfidenceThreshold,
		float NeutralTolerance,
		float RaisedThreshold,
		float OtherArmTolerance);

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
	FRotator HeadNeutralRotation = FRotator::ZeroRotator;

	/** Morph targets written by the previous tracking frame. */
	TSet<FName> LastAppliedMorphTargets;
	TMap<FName, float> TargetMorphValues;
	TMap<FName, float> SmoothedMorphValues;
	float FaceTrackingMissingSeconds = 0.0f;
	float CurrentAnimationDeltaSeconds = 1.0f / 60.0f;

	struct FArmInputFilterState
	{
		float FilteredAngle = 0.0f;
		float LastRawAngle = 0.0f;
		int32 ConsecutiveRejectedFrames = 0;
		bool bInitialized = false;
	};

	struct FArmTrackingState
	{
		float BelowThresholdSeconds = 0.0f;
		float LastStableRawAngle = 0.0f;
		float LastStableRawForwardAngle = 0.0f;
		bool bLastRawInward = false;
		bool bTracked = false;
		bool bHasStableAngle = false;
		bool bFreshSample = false;
	};

	FArmInputFilterState LeftArmInputFilter;
	FArmInputFilterState RightArmInputFilter;
	FArmTrackingState LeftArmTrackingState;
	FArmTrackingState RightArmTrackingState;
	float LeftNeutralInputAngle = 0.0f;
	float RightNeutralInputAngle = 0.0f;
	float LeftNeutralForwardAngle = 0.0f;
	float RightNeutralForwardAngle = 0.0f;
	float CalibrationStateElapsedSeconds = 0.0f;
	int64 LastCalibrationFrameId = INDEX_NONE;
	TArray<float> CalibrationHeadPitchSamples;
	TArray<float> CalibrationHeadYawSamples;
	TArray<float> CalibrationHeadRollSamples;
	TArray<float> CalibrationLeftArmSamples;
	TArray<float> CalibrationRightArmSamples;
	TArray<float> CalibrationLeftArmForwardSamples;
	TArray<float> CalibrationRightArmForwardSamples;
	float ValidationStageElapsedSeconds = 0.0f;
	float ValidationPoseHeldSeconds = 0.0f;
	bool bProfileInitialized = false;

	FRotator MakeUpperArmRotation(float AngleDegrees, bool bLeft) const;
	bool CalculateHeadTrackingRotation(FRotator& OutRotation) const;
	bool FilterUpperArmInput(
		float RawAngle,
		float DeltaSeconds,
		FArmInputFilterState& State,
		float& OutFilteredAngle);
	void ResetUpperArmInputFilters();
	void ResetCalibrationSamples();
	void CollectCalibrationSample(const FRotator& RawHeadRotation);
	void TickNeutralCalibration(float DeltaSeconds);
	void CompleteNeutralCalibration();
	void TickArmValidation(float DeltaSeconds);
	void AdvanceArmValidation();
	void FailArmValidation(const FString& Reason);
	FString ResolveTrackingProfileId() const;
	FVPAvatarTrackingProfile BuildTrackingProfile() const;
	void ApplyTrackingProfile(const FVPAvatarTrackingProfile& Profile);

};
