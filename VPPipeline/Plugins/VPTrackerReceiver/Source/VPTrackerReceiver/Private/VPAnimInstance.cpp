#include "VPAnimInstance.h"
#include "VPUDPReceiver.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DataTable.h"
#include "Engine/SkeletalMesh.h"
#include "Kismet/GameplayStatics.h"

namespace
{
constexpr TCHAR TrackingProfileSaveSlot[] = TEXT("VPTrackingProfiles");
constexpr int32 TrackingProfileUserIndex = 0;

bool IsManagedAvatarProfileId(const FString& ProfileId)
{
	if (ProfileId.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : ProfileId)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}
	return true;
}
}

void UVPAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();
	CalibrationStatus = TEXT("캘리브레이션 대기");
	ArmValidationStatus = TEXT("팔 검증 대기");

	// Auto-find VPUDPReceiver on the owning actor
	if (bAutoFindReceiver)
	{
		AActor* Owner = GetOwningActor();
		if (Owner)
		{
			CachedReceiver = Owner->FindComponentByClass<UVPUDPReceiver>();
			if (CachedReceiver)
			{
				UE_LOG(LogTemp, Log, TEXT("[VPAnimInstance] Auto-connected to VPUDPReceiver on %s"), *Owner->GetName());
			}
		}
	}

	if (bAutoLoadTrackingProfile && !bProfileInitialized)
	{
		LoadTrackingProfile();
	}
}

void UVPAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);
	CurrentAnimationDeltaSeconds = FMath::Max(0.0f, DeltaSeconds);

	// Pull latest data from auto-found receiver
	if (CachedReceiver)
	{
		TrackingData = CachedReceiver->GetLatestTrackingData();
	}

	// Always apply rest pose + blendshapes (rest pose baseline even without tracking data)
	ApplyBlendshapesToMorphTargets();
	if (TrackingData.bIsValid)
	{
		UpdatePoseBoneTransforms();
		UpdateHeadRotation();
		UpdateUpperArmRotations(DeltaSeconds);
	}
	else
	{
		PoseBoneTransforms.Reset();
		SmoothedHeadRotation = FMath::RInterpTo(
			SmoothedHeadRotation,
			FRotator::ZeroRotator,
			DeltaSeconds,
			TrackingLossReturnSpeed
		);
		HeadRotation = SmoothedHeadRotation;
		UpdateUpperArmRotations(DeltaSeconds);
	}

	TickNeutralCalibration(DeltaSeconds);
	TickArmValidation(DeltaSeconds);
}

void UVPAnimInstance::ApplyTrackingData(const FVPTrackingFrame& Frame)
{
	TrackingData = Frame;
	if (TrackingData.bIsValid)
	{
		UpdatePoseBoneTransforms();
	}
	else
	{
		PoseBoneTransforms.Reset();
	}
}

void UVPAnimInstance::CacheMappingTable()
{
	CachedMapping.Reset();
	bMappingCached = true;

	if (!BlendshapeMappingTable)
	{
		return;
	}

	const FString ContextStr(TEXT("VPAnimInstance::CacheMappingTable"));
	TArray<FVPBlendshapeMapping*> Rows;
	BlendshapeMappingTable->GetAllRows<FVPBlendshapeMapping>(ContextStr, Rows);

	for (const FVPBlendshapeMapping* Row : Rows)
	{
		if (Row && !Row->ARKitName.IsNone() && !Row->MorphTargetName.IsNone())
		{
			CachedMapping.Add(Row->ARKitName, TPair<FName, float>(Row->MorphTargetName, Row->Scale));
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[VPAnimInstance] Cached %d blendshape mappings from DataTable"), CachedMapping.Num());
}

void UVPAnimInstance::ApplyBlendshapesToMorphTargets()
{
	USkeletalMeshComponent* MeshComp = GetSkelMeshComponent();
	if (!MeshComp)
	{
		return;
	}

	// Cache mapping table on first use
	if (!bMappingCached)
	{
		CacheMappingTable();
	}

	const bool bHasMapping = CachedMapping.Num() > 0;
	const bool bFaceTracked = TrackingData.bIsValid && TrackingData.bFaceTracked;
	if (bFaceTracked)
	{
		FaceTrackingMissingSeconds = 0.0f;
		TargetMorphValues.Reset();
		for (const auto& Pair : TrackingData.FaceData.Blendshapes)
		{
			if (Pair.Key == FName("_neutral"))
			{
				continue;
			}

			FName TargetName = Pair.Key;
			float Scale = 1.0f;
			if (bHasMapping)
			{
				const TPair<FName, float>* Mapping = CachedMapping.Find(Pair.Key);
				if (!Mapping)
				{
					continue;
				}
				TargetName = Mapping->Key;
				Scale = Mapping->Value;
			}

			const float RawValue = Pair.Value < BlendshapeDeadZone ? 0.0f : Pair.Value;
			TargetMorphValues.Add(
				TargetName,
				FMath::Clamp(RawValue * FaceBlendWeight * Scale, 0.0f, 1.0f));
		}
	}
	else
	{
		FaceTrackingMissingSeconds += CurrentAnimationDeltaSeconds;
		if (FaceTrackingMissingSeconds > FaceTrackingGraceSeconds)
		{
			TargetMorphValues.Reset();
		}
	}

	TSet<FName> MorphTargets = LastAppliedMorphTargets;
	for (const auto& Pair : RestPoseMorphTargets)
	{
		MorphTargets.Add(Pair.Key);
	}
	for (const auto& Pair : TargetMorphValues)
	{
		MorphTargets.Add(Pair.Key);
	}

	const float Alpha = 1.0f - FMath::Exp(
		-FMath::Max(0.1f, FaceBlendshapeSmoothingSpeed) * CurrentAnimationDeltaSeconds);
	FaceBlendshapes.Reset();
	LastAppliedMorphTargets.Reset();
	for (const FName& MorphTarget : MorphTargets)
	{
		const float RestValue = FMath::Clamp(RestPoseMorphTargets.FindRef(MorphTarget), 0.0f, 1.0f);
		const float TargetValue = TargetMorphValues.Contains(MorphTarget)
			? TargetMorphValues.FindRef(MorphTarget)
			: RestValue;
		float& SmoothedValue = SmoothedMorphValues.FindOrAdd(MorphTarget, RestValue);
		SmoothedValue = FMath::Lerp(SmoothedValue, TargetValue, Alpha);
		if (FMath::IsNearlyEqual(SmoothedValue, TargetValue, 0.0001f))
		{
			SmoothedValue = TargetValue;
		}
		MeshComp->SetMorphTarget(MorphTarget, SmoothedValue);
		FaceBlendshapes.Add(MorphTarget, SmoothedValue);
		LastAppliedMorphTargets.Add(MorphTarget);
	}
}

void UVPAnimInstance::UpdateHeadRotation()
{
	if (!bEnableHeadTracking)
	{
		return;
	}

	FRotator RawRotation;
	if (!CalculateHeadTrackingRotation(RawRotation))
	{
		SmoothedHeadRotation = SmoothHeadRotationStep(
			SmoothedHeadRotation,
			FRotator::ZeroRotator,
			CurrentAnimationDeltaSeconds,
			TrackingLossReturnSpeed,
			HeadMaxDegreesPerSecond);
		HeadRotation = SmoothedHeadRotation;
		return;
	}

	const FRotator TargetRotation = ApplyHeadRotationDeadZone(
		(RawRotation - HeadNeutralRotation).GetNormalized(),
		HeadRotationDeadZoneDegrees);

	SmoothedHeadRotation = SmoothHeadRotationStep(
		SmoothedHeadRotation,
		TargetRotation,
		CurrentAnimationDeltaSeconds,
		HeadRotationSmoothingSpeed,
		HeadMaxDegreesPerSecond);
	HeadRotation = SmoothedHeadRotation;

	// HeadRotation is stored as a UPROPERTY for use in AnimBP (Transform Modify Bone node)
	// or can be read from Blueprint to drive bone rotation.
}

FRotator UVPAnimInstance::ApplyHeadRotationDeadZone(
	const FRotator& Rotation,
	float DeadZoneDegrees)
{
	const float SafeDeadZone = FMath::Max(0.0f, DeadZoneDegrees);
	auto FilterAxis = [SafeDeadZone](float Value)
	{
		const float Normalized = FRotator::NormalizeAxis(Value);
		const float Magnitude = FMath::Abs(Normalized);
		return Magnitude <= SafeDeadZone
			? 0.0f
			: FMath::Sign(Normalized) * (Magnitude - SafeDeadZone);
	};
	return FRotator(
		FilterAxis(Rotation.Pitch),
		FilterAxis(Rotation.Yaw),
		FilterAxis(Rotation.Roll));
}

FRotator UVPAnimInstance::SmoothHeadRotationStep(
	const FRotator& Current,
	const FRotator& Target,
	float DeltaSeconds,
	float SmoothingSpeed,
	float MaxDegreesPerSecond)
{
	const float SafeDeltaSeconds = FMath::Max(0.0f, DeltaSeconds);
	const float Alpha = 1.0f - FMath::Exp(-FMath::Max(0.1f, SmoothingSpeed) * SafeDeltaSeconds);
	const FRotator Desired = FMath::Lerp(Current, Target, Alpha).GetNormalized();
	FRotator Delta = (Desired - Current).GetNormalized();
	const float MaxStep = FMath::Max(0.0f, MaxDegreesPerSecond) * SafeDeltaSeconds;
	Delta.Pitch = FMath::Clamp(Delta.Pitch, -MaxStep, MaxStep);
	Delta.Yaw = FMath::Clamp(Delta.Yaw, -MaxStep, MaxStep);
	Delta.Roll = FMath::Clamp(Delta.Roll, -MaxStep, MaxStep);
	return (Current + Delta).GetNormalized();
}

bool UVPAnimInstance::CalculateHeadTrackingRotation(FRotator& OutRotation) const
{
	OutRotation = FRotator::ZeroRotator;
	if (!TrackingData.bIsValid || !TrackingData.bFaceTracked)
	{
		return false;
	}

	const float FacePitch = FMath::Clamp(
		TrackingData.FaceRotation.Pitch * HeadRotationScale, -30.0f, 30.0f);
	const float FaceYaw = FMath::Clamp(
		TrackingData.FaceRotation.Yaw * HeadRotationScale, -45.0f, 45.0f);
	const float FaceRoll = FMath::Clamp(
		TrackingData.FaceRotation.Roll * HeadRotationScale, -25.0f, 25.0f);

	// Runtime VRM local axes retain the established mapping: facial roll drives
	// bone Pitch, facial yaw drives bone Yaw, and facial pitch drives bone Roll.
	OutRotation = FRotator(
		FaceRoll * RollSign,
		FaceYaw * YawSign,
		FacePitch * PitchSign);
	return true;
}

bool UVPAnimInstance::CalculateHeadRollDegrees(
	const FVPTrackingFrame& Frame,
	float ConfidenceThreshold,
	float& OutRollDegrees)
{
	OutRollDegrees = 0.0f;
	if (!Frame.bIsValid || !Frame.bPoseTracked || Frame.PoseLandmarks.Num() < 9)
	{
		return false;
	}

	auto TryFacePair = [&](int32 LeftIndex, int32 RightIndex)
	{
		const FVPPoseLandmark& Left = Frame.PoseLandmarks[LeftIndex];
		const FVPPoseLandmark& Right = Frame.PoseLandmarks[RightIndex];
		const float Confidence = FMath::Min(
			FMath::Min(Left.Visibility, Left.Presence),
			FMath::Min(Right.Visibility, Right.Presence));
		const FVector2D Difference(
			Right.Position.X - Left.Position.X,
			Right.Position.Y - Left.Position.Y);
		if (Confidence < ConfidenceThreshold || FMath::Abs(Difference.X) < 0.005f)
		{
			return false;
		}

		OutRollDegrees = FMath::RadiansToDegrees(FMath::Atan2(
			Difference.Y,
			FMath::Abs(Difference.X)));
		return FMath::IsFinite(OutRollDegrees);
	};

	// MediaPipe Pose: eye centers 2/5, ears 7/8. Eyes are steadier at close range;
	// ears provide a fallback when glasses, hair, or a partial turn hides an eye.
	return TryFacePair(2, 5) || TryFacePair(7, 8);
}

void UVPAnimInstance::UpdatePoseBoneTransforms()
{
	if (!TrackingData.bPoseTracked)
	{
		PoseBoneTransforms.Reset();
		return;
	}

	const int32 NumLandmarks = TrackingData.PoseLandmarks.Num();
	PoseBoneTransforms.SetNum(NumLandmarks);

	for (int32 i = 0; i < NumLandmarks; ++i)
	{
		const FVector& NormPos = TrackingData.PoseLandmarks[i].Position;

		// Convert normalized coords to UE space:
		// MediaPipe: X right, Y down, Z toward camera
		// UE:        X forward, Y right, Z up
		const FVector UEPos(
			-NormPos.Z * PosePositionScale,  // MediaPipe Z (depth) -> UE X (forward, inverted)
			NormPos.X * PosePositionScale,   // MediaPipe X (right) -> UE Y (right)
			-NormPos.Y * PosePositionScale   // MediaPipe Y (down)  -> UE Z (up, inverted)
		);

		PoseBoneTransforms[i] = FTransform(FQuat::Identity, UEPos, FVector::OneVector);
	}
}

bool UVPAnimInstance::CalculateUpperArmAngle(
	const FVPTrackingFrame& Frame,
	bool bLeft,
	float ConfidenceThreshold,
	float& OutAngleDegrees,
	float& OutConfidence)
{
	float ForwardDegrees = 0.0f;
	bool bInward = false;
	return CalculateUpperArmPose(
		Frame,
		bLeft,
		ConfidenceThreshold,
		OutAngleDegrees,
		ForwardDegrees,
		OutConfidence,
		bInward);
}

bool UVPAnimInstance::CalculateUpperArmPose(
	const FVPTrackingFrame& Frame,
	bool bLeft,
	float ConfidenceThreshold,
	float& OutElevationDegrees,
	float& OutForwardDegrees,
	float& OutConfidence,
	bool& bOutInward)
{
	OutElevationDegrees = 0.0f;
	OutForwardDegrees = 0.0f;
	OutConfidence = 0.0f;
	bOutInward = false;
	if (!Frame.bIsValid || !Frame.bPoseTracked || Frame.PoseLandmarks.Num() < 15)
	{
		return false;
	}

	const int32 ShoulderIndex = bLeft ? 11 : 12;
	const int32 ElbowIndex = bLeft ? 13 : 14;
	const FVPPoseLandmark& Shoulder = Frame.PoseLandmarks[ShoulderIndex];
	const FVPPoseLandmark& Elbow = Frame.PoseLandmarks[ElbowIndex];
	OutConfidence = FMath::Min(
		FMath::Min(Shoulder.Visibility, Shoulder.Presence),
		FMath::Min(Elbow.Visibility, Elbow.Presence));
	if (OutConfidence < ConfidenceThreshold)
	{
		return false;
	}

	const FVector2D ShoulderPosition(Shoulder.Position.X, Shoulder.Position.Y);
	const FVector2D ElbowPosition(Elbow.Position.X, Elbow.Position.Y);
	const FVector2D ShoulderToElbow = ElbowPosition - ShoulderPosition;
	if (ShoulderToElbow.SizeSquared() < FMath::Square(0.02f))
	{
		return false;
	}

	// The tracker sends the unmirrored camera image. A person's left arm moves
	// toward increasing image X; the right arm moves toward decreasing image X.
	const float Outward = bLeft ? ShoulderToElbow.X : -ShoulderToElbow.X;
	const float Down = ShoulderToElbow.Y;
	const float PlanarLength = ShoulderToElbow.Size();
	bOutInward = Outward < -PlanarLength * 0.05f;

	// Do not allow a tracked elbow to steer the upper arm through the torso.
	// When the elbow crosses inward, retain its height but place the arm in front.
	const float SafeOutward = bOutInward
		? FMath::Max(0.15f * PlanarLength, 0.005f)
		: Outward;
	OutElevationDegrees = FMath::RadiansToDegrees(FMath::Atan2(SafeOutward, Down));

	// MediaPipe Z becomes more negative toward the camera. Normalize depth by
	// visible upper-arm length so distance from the webcam does not change gain.
	const float Forward = Shoulder.Position.Z - Elbow.Position.Z;
	OutForwardDegrees = FMath::Clamp(
		FMath::RadiansToDegrees(FMath::Atan2(Forward, FMath::Max(PlanarLength, 0.02f))),
		-20.0f,
		60.0f);
	if (bOutInward)
	{
		OutForwardDegrees = FMath::Max(OutForwardDegrees, 30.0f);
	}
	return FMath::IsFinite(OutElevationDegrees) && FMath::IsFinite(OutForwardDegrees);
}

FRotator UVPAnimInstance::MakeUpperArmRotation(float AngleDegrees, bool bLeft) const
{
	const float SignedAngle = AngleDegrees * (bLeft ? LeftUpperArmSign : RightUpperArmSign);
	switch (UpperArmRotationAxis)
	{
	case EVPArmRotationAxis::Pitch:
		return FRotator(SignedAngle, 0.0f, 0.0f);
	case EVPArmRotationAxis::Yaw:
		return FRotator(0.0f, SignedAngle, 0.0f);
	case EVPArmRotationAxis::Roll:
	default:
		return FRotator(0.0f, 0.0f, SignedAngle);
	}
}

float UVPAnimInstance::MapUpperArmOutput(
	float InputAngle,
	float NeutralInputAngle,
	float Gain,
	bool bInvert,
	float MinAngle,
	float MaxAngle)
{
	const float Direction = bInvert ? -1.0f : 1.0f;
	const float Delta = FMath::FindDeltaAngleDegrees(NeutralInputAngle, InputAngle);
	return FMath::Clamp(Delta * FMath::Max(0.0f, Gain) * Direction, MinAngle, MaxAngle);
}

bool UVPAnimInstance::FilterUpperArmInput(
	float RawAngle,
	float DeltaSeconds,
	FArmInputFilterState& State,
	float& OutFilteredAngle)
{
	const bool bAccepted = FilterUpperArmInputStep(
		RawAngle,
		DeltaSeconds,
		UpperArmInputSmoothingSpeed,
		UpperArmMaxInputJump,
		State.FilteredAngle,
		State.LastRawAngle,
		State.ConsecutiveRejectedFrames,
		State.bInitialized,
		OutFilteredAngle);
	if (!bAccepted)
	{
		++RejectedUpperArmInputCount;
	}
	return bAccepted;
}

bool UVPAnimInstance::FilterUpperArmInputStep(
	float RawAngle,
	float DeltaSeconds,
	float SmoothingSpeed,
	float MaxInputJump,
	float& InOutFilteredAngle,
	float& InOutLastRawAngle,
	int32& InOutConsecutiveRejectedFrames,
	bool& bInOutInitialized,
	float& OutFilteredAngle)
{
	if (!bInOutInitialized)
	{
		InOutFilteredAngle = RawAngle;
		InOutLastRawAngle = RawAngle;
		InOutConsecutiveRejectedFrames = 0;
		bInOutInitialized = true;
		OutFilteredAngle = RawAngle;
		return true;
	}

	const float InputJump = FMath::Abs(FMath::FindDeltaAngleDegrees(InOutLastRawAngle, RawAngle));
	if (InputJump > MaxInputJump && InOutConsecutiveRejectedFrames < 2)
	{
		++InOutConsecutiveRejectedFrames;
		OutFilteredAngle = InOutFilteredAngle;
		return false;
	}

	if (InputJump > MaxInputJump)
	{
		InOutFilteredAngle = RawAngle;
	}
	else
	{
		const float Alpha = 1.0f - FMath::Exp(-FMath::Max(0.1f, SmoothingSpeed) * DeltaSeconds);
		const float Delta = FMath::FindDeltaAngleDegrees(InOutFilteredAngle, RawAngle);
		InOutFilteredAngle = FRotator::NormalizeAxis(InOutFilteredAngle + Delta * Alpha);
	}

	InOutLastRawAngle = RawAngle;
	InOutConsecutiveRejectedFrames = 0;
	OutFilteredAngle = InOutFilteredAngle;
	return true;
}

bool UVPAnimInstance::UpdateArmTrackingStateStep(
	bool bWasTracked,
	bool bRawSampleValid,
	float Confidence,
	float DeltaSeconds,
	float AcquireThreshold,
	float ReleaseThreshold,
	float GraceSeconds,
	float& InOutBelowThresholdSeconds,
	bool& bOutFreshSample)
{
	bOutFreshSample = false;
	const float SafeAcquireThreshold = FMath::Clamp(AcquireThreshold, 0.0f, 1.0f);
	const float SafeReleaseThreshold = FMath::Clamp(
		ReleaseThreshold, 0.0f, SafeAcquireThreshold);
	const float RequiredConfidence = bWasTracked
		? SafeReleaseThreshold
		: SafeAcquireThreshold;
	if (bRawSampleValid && Confidence >= RequiredConfidence)
	{
		InOutBelowThresholdSeconds = 0.0f;
		bOutFreshSample = true;
		return true;
	}

	if (bWasTracked)
	{
		InOutBelowThresholdSeconds += FMath::Max(0.0f, DeltaSeconds);
		return InOutBelowThresholdSeconds <= FMath::Max(0.0f, GraceSeconds);
	}

	InOutBelowThresholdSeconds = 0.0f;
	return false;
}

void UVPAnimInstance::ResetUpperArmInputFilters()
{
	LeftArmInputFilter = FArmInputFilterState();
	RightArmInputFilter = FArmInputFilterState();
}

void UVPAnimInstance::UpdateUpperArmRotations(float DeltaSeconds)
{
	float RawLeftAngle = 0.0f;
	float RawRightAngle = 0.0f;
	float RawLeftForwardAngle = 0.0f;
	float RawRightForwardAngle = 0.0f;
	float RawLeftConfidence = 0.0f;
	float RawRightConfidence = 0.0f;
	bool bRawLeftInward = false;
	bool bRawRightInward = false;
	const bool bRawLeftValid = bEnableUpperArmTracking && CalculateUpperArmPose(
		TrackingData, true, 0.0f, RawLeftAngle, RawLeftForwardAngle,
		RawLeftConfidence, bRawLeftInward);
	const bool bRawRightValid = bEnableUpperArmTracking && CalculateUpperArmPose(
		TrackingData, false, 0.0f, RawRightAngle, RawRightForwardAngle,
		RawRightConfidence, bRawRightInward);
	LeftArmTrackingState.bLastRawInward = bRawLeftValid && bRawLeftInward;
	RightArmTrackingState.bLastRawInward = bRawRightValid && bRawRightInward;
	LeftArmTrackingState.bTracked = UpdateArmTrackingStateStep(
		LeftArmTrackingState.bTracked,
		bRawLeftValid,
		RawLeftConfidence,
		DeltaSeconds,
		UpperArmConfidenceThreshold,
		UpperArmReleaseConfidenceThreshold,
		UpperArmTrackingGraceSeconds,
		LeftArmTrackingState.BelowThresholdSeconds,
		LeftArmTrackingState.bFreshSample);
	RightArmTrackingState.bTracked = UpdateArmTrackingStateStep(
		RightArmTrackingState.bTracked,
		bRawRightValid,
		RawRightConfidence,
		DeltaSeconds,
		UpperArmConfidenceThreshold,
		UpperArmReleaseConfidenceThreshold,
		UpperArmTrackingGraceSeconds,
		RightArmTrackingState.BelowThresholdSeconds,
		RightArmTrackingState.bFreshSample);
	if (LeftArmTrackingState.bFreshSample)
	{
		LeftArmTrackingState.LastStableRawAngle = RawLeftAngle;
		LeftArmTrackingState.LastStableRawForwardAngle = RawLeftForwardAngle;
		LeftArmTrackingState.bHasStableAngle = true;
	}
	if (RightArmTrackingState.bFreshSample)
	{
		RightArmTrackingState.LastStableRawAngle = RawRightAngle;
		RightArmTrackingState.LastStableRawForwardAngle = RawRightForwardAngle;
		RightArmTrackingState.bHasStableAngle = true;
	}

	bLeftUpperArmTracked = bSwapUpperArms
		? RightArmTrackingState.bTracked && RightArmTrackingState.bHasStableAngle
		: LeftArmTrackingState.bTracked && LeftArmTrackingState.bHasStableAngle;
	bRightUpperArmTracked = bSwapUpperArms
		? LeftArmTrackingState.bTracked && LeftArmTrackingState.bHasStableAngle
		: RightArmTrackingState.bTracked && RightArmTrackingState.bHasStableAngle;
	LeftUpperArmConfidence = bSwapUpperArms ? RawRightConfidence : RawLeftConfidence;
	RightUpperArmConfidence = bSwapUpperArms ? RawLeftConfidence : RawRightConfidence;
	const float MappedLeftRawAngle = bSwapUpperArms
		? RightArmTrackingState.LastStableRawAngle
		: LeftArmTrackingState.LastStableRawAngle;
	const float MappedRightRawAngle = bSwapUpperArms
		? LeftArmTrackingState.LastStableRawAngle
		: RightArmTrackingState.LastStableRawAngle;
	const float MappedLeftRawForwardAngle = bSwapUpperArms
		? RightArmTrackingState.LastStableRawForwardAngle
		: LeftArmTrackingState.LastStableRawForwardAngle;
	const float MappedRightRawForwardAngle = bSwapUpperArms
		? LeftArmTrackingState.LastStableRawForwardAngle
		: RightArmTrackingState.LastStableRawForwardAngle;
	const bool bMappedLeftInward = bSwapUpperArms
		? RightArmTrackingState.bLastRawInward
		: LeftArmTrackingState.bLastRawInward;
	const bool bMappedRightInward = bSwapUpperArms
		? LeftArmTrackingState.bLastRawInward
		: RightArmTrackingState.bLastRawInward;
	float LeftForwardTarget = 0.0f;
	float RightForwardTarget = 0.0f;

	if (bLeftUpperArmTracked)
	{
		LeftUpperArmInputAngle = MappedLeftRawAngle;
		float FilteredAngle = MappedLeftRawAngle;
		FilterUpperArmInput(MappedLeftRawAngle, DeltaSeconds, LeftArmInputFilter, FilteredAngle);
		LeftUpperArmOutputAngle = MapUpperArmOutput(
			FilteredAngle,
			LeftNeutralInputAngle,
			LeftUpperArmGain,
			bInvertLeftUpperArm,
			UpperArmMinAngle,
			UpperArmMaxAngle);
		LeftForwardTarget = FMath::Clamp(
			MappedLeftRawForwardAngle - LeftNeutralForwardAngle, -20.0f, 60.0f);
	}
	else
	{
		LeftUpperArmInputAngle = 0.0f;
		LeftUpperArmOutputAngle = 0.0f;
		LeftArmInputFilter = FArmInputFilterState();
	}

	if (bRightUpperArmTracked)
	{
		RightUpperArmInputAngle = MappedRightRawAngle;
		float FilteredAngle = MappedRightRawAngle;
		FilterUpperArmInput(MappedRightRawAngle, DeltaSeconds, RightArmInputFilter, FilteredAngle);
		RightUpperArmOutputAngle = MapUpperArmOutput(
			FilteredAngle,
			RightNeutralInputAngle,
			RightUpperArmGain,
			bInvertRightUpperArm,
			UpperArmMinAngle,
			UpperArmMaxAngle);
		RightForwardTarget = FMath::Clamp(
			MappedRightRawForwardAngle - RightNeutralForwardAngle, -20.0f, 60.0f);
	}
	else
	{
		RightUpperArmInputAngle = 0.0f;
		RightUpperArmOutputAngle = 0.0f;
		RightArmInputFilter = FArmInputFilterState();
	}

	// A self-occluded inward elbow is not allowed to collapse through the torso.
	// After the short confidence grace period it blends to a conservative pose in front.
	const bool bLeftSafeOcclusion = !bLeftUpperArmTracked && bMappedLeftInward;
	const bool bRightSafeOcclusion = !bRightUpperArmTracked && bMappedRightInward;
	const float LeftTargetAngle = bLeftUpperArmTracked
		? LeftUpperArmOutputAngle
		: (bLeftSafeOcclusion ? FMath::Clamp(LeftUpperArmAppliedAngle, 20.0f, 90.0f) : 0.0f);
	const float RightTargetAngle = bRightUpperArmTracked
		? RightUpperArmOutputAngle
		: (bRightSafeOcclusion ? FMath::Clamp(RightUpperArmAppliedAngle, 20.0f, 90.0f) : 0.0f);
	if (bLeftSafeOcclusion)
	{
		LeftForwardTarget = 30.0f;
	}
	if (bRightSafeOcclusion)
	{
		RightForwardTarget = 30.0f;
	}
	LeftUpperArmAppliedAngle = FMath::FInterpTo(
		LeftUpperArmAppliedAngle, LeftTargetAngle, DeltaSeconds, UpperArmSmoothingSpeed);
	RightUpperArmAppliedAngle = FMath::FInterpTo(
		RightUpperArmAppliedAngle, RightTargetAngle, DeltaSeconds, UpperArmSmoothingSpeed);
	LeftUpperArmAppliedForwardAngle = FMath::FInterpTo(
		LeftUpperArmAppliedForwardAngle, LeftForwardTarget, DeltaSeconds, UpperArmSmoothingSpeed);
	RightUpperArmAppliedForwardAngle = FMath::FInterpTo(
		RightUpperArmAppliedForwardAngle, RightForwardTarget, DeltaSeconds, UpperArmSmoothingSpeed);

	// Blueprint avatars retain their configurable Euler axis. Runtime VRM avatars
	// consume the same smoothed scalar angles and map them to their reference pose.
	LeftUpperArmRotation = MakeUpperArmRotation(LeftUpperArmAppliedAngle, true);
	RightUpperArmRotation = MakeUpperArmRotation(RightUpperArmAppliedAngle, false);

}

bool UVPAnimInstance::StartNeutralCalibration()
{
	CancelArmValidation();
	ResetCalibrationSamples();
	CalibrationStateElapsedSeconds = 0.0f;
	CalibrationRemainingSeconds = CalibrationCountdownSeconds;

	FRotator RawHeadRotation;
	const bool bTrackingReady = bLeftUpperArmTracked && bRightUpperArmTracked &&
		CalculateHeadTrackingRotation(RawHeadRotation);
	CalibrationState = bTrackingReady
		? EVPCalibrationState::CountingDown
		: EVPCalibrationState::WaitingForTracking;
	CalibrationStatus = bTrackingReady
		? TEXT("정면을 보고 어깨를 수평으로 유지하세요. 양팔을 몸 옆에 내리고 팔꿈치까지 보여주세요.")
		: TEXT("정면을 보고 얼굴·양쪽 어깨·팔꿈치가 모두 보이도록 위치를 조정하세요.");
	return true;
}

void UVPAnimInstance::CancelNeutralCalibration()
{
	CalibrationState = EVPCalibrationState::Idle;
	CalibrationStateElapsedSeconds = 0.0f;
	CalibrationRemainingSeconds = 0.0f;
	CalibrationStatus = TEXT("캘리브레이션 대기");
	ResetCalibrationSamples();
}

void UVPAnimInstance::TickNeutralCalibration(float DeltaSeconds)
{
	if (CalibrationState != EVPCalibrationState::CountingDown &&
		CalibrationState != EVPCalibrationState::WaitingForTracking)
	{
		return;
	}

	FRotator RawHeadRotation;
	const bool bTrackingReady = bLeftUpperArmTracked && bRightUpperArmTracked &&
		CalculateHeadTrackingRotation(RawHeadRotation);

	if (CalibrationState == EVPCalibrationState::CountingDown)
	{
		if (!bTrackingReady)
		{
			CalibrationState = EVPCalibrationState::WaitingForTracking;
			CalibrationStateElapsedSeconds = 0.0f;
			CalibrationRemainingSeconds = CalibrationTrackingRecoverySeconds;
			CalibrationStatus = TEXT("추적이 끊겼습니다. 5초 안에 얼굴과 양팔을 다시 보여주세요.");
			return;
		}

		CollectCalibrationSample(RawHeadRotation);
		CalibrationStateElapsedSeconds += DeltaSeconds;
		CalibrationRemainingSeconds = FMath::Max(
			0.0f, CalibrationCountdownSeconds - CalibrationStateElapsedSeconds);
		CalibrationStatus = FString::Printf(
			TEXT("정면·어깨 수평·양팔 내림 유지: %.1f초 · 정상 샘플 %d/%d"),
			CalibrationRemainingSeconds,
			CalibrationLeftArmSamples.Num(),
			CalibrationMinimumSamples);
		if (CalibrationStateElapsedSeconds >= CalibrationCountdownSeconds &&
			CalibrationLeftArmSamples.Num() >= CalibrationMinimumSamples)
		{
			CompleteNeutralCalibration();
		}
		return;
	}

	if (bTrackingReady)
	{
		CalibrationState = EVPCalibrationState::CountingDown;
		CalibrationStateElapsedSeconds = 0.0f;
		CalibrationRemainingSeconds = CalibrationCountdownSeconds;
		ResetCalibrationSamples();
		CalibrationStatus = TEXT("추적이 복구되었습니다. 3초 캘리브레이션을 다시 시작합니다.");
		return;
	}

	CalibrationStateElapsedSeconds += DeltaSeconds;
	CalibrationRemainingSeconds = FMath::Max(
		0.0f, CalibrationTrackingRecoverySeconds - CalibrationStateElapsedSeconds);
	CalibrationStatus = FString::Printf(
		TEXT("추적 복구 대기: %.1f초"), CalibrationRemainingSeconds);
	if (CalibrationStateElapsedSeconds >= CalibrationTrackingRecoverySeconds)
	{
		CalibrationState = EVPCalibrationState::Failed;
		CalibrationStatus = TEXT("캘리브레이션 실패: 추적을 복구하지 못했습니다.");
	}
}

void UVPAnimInstance::CompleteNeutralCalibration()
{
	if (CalibrationLeftArmSamples.Num() < CalibrationMinimumSamples ||
		CalibrationRightArmSamples.Num() != CalibrationLeftArmSamples.Num() ||
		CalibrationHeadPitchSamples.Num() != CalibrationLeftArmSamples.Num())
	{
		CalibrationState = EVPCalibrationState::WaitingForTracking;
		CalibrationStateElapsedSeconds = 0.0f;
		CalibrationRemainingSeconds = CalibrationTrackingRecoverySeconds;
		CalibrationStatus = TEXT("정상 추적 샘플이 부족합니다. 얼굴과 양팔 추적을 복구해주세요.");
		return;
	}

	HeadNeutralRotation = FRotator(
		CalculateCircularMeanDegrees(CalibrationHeadPitchSamples),
		CalculateCircularMeanDegrees(CalibrationHeadYawSamples),
		CalculateCircularMeanDegrees(CalibrationHeadRollSamples));
	LeftNeutralInputAngle = CalculateCircularMeanDegrees(CalibrationLeftArmSamples);
	RightNeutralInputAngle = CalculateCircularMeanDegrees(CalibrationRightArmSamples);
	LeftNeutralForwardAngle = CalculateCircularMeanDegrees(CalibrationLeftArmForwardSamples);
	RightNeutralForwardAngle = CalculateCircularMeanDegrees(CalibrationRightArmForwardSamples);
	bHasNeutralCalibration = true;
	CalibrationState = EVPCalibrationState::Succeeded;
	CalibrationRemainingSeconds = 0.0f;
	CalibrationStatus = SaveTrackingProfile()
		? TEXT("캘리브레이션 완료: 아바타 프로필에 저장했습니다.")
		: TEXT("캘리브레이션 완료: 프로필 키가 없어 현재 세션에만 적용합니다.");
	ResetUpperArmInputFilters();
}

void UVPAnimInstance::ResetCalibrationSamples()
{
	LastCalibrationFrameId = INDEX_NONE;
	CalibrationHeadPitchSamples.Reset();
	CalibrationHeadYawSamples.Reset();
	CalibrationHeadRollSamples.Reset();
	CalibrationLeftArmSamples.Reset();
	CalibrationRightArmSamples.Reset();
	CalibrationLeftArmForwardSamples.Reset();
	CalibrationRightArmForwardSamples.Reset();
}

void UVPAnimInstance::CollectCalibrationSample(const FRotator& RawHeadRotation)
{
	const bool bLeftFresh = bSwapUpperArms
		? RightArmTrackingState.bFreshSample
		: LeftArmTrackingState.bFreshSample;
	const bool bRightFresh = bSwapUpperArms
		? LeftArmTrackingState.bFreshSample
		: RightArmTrackingState.bFreshSample;
	if (!bLeftFresh || !bRightFresh || TrackingData.FrameId == LastCalibrationFrameId)
	{
		return;
	}

	LastCalibrationFrameId = TrackingData.FrameId;
	CalibrationHeadPitchSamples.Add(RawHeadRotation.Pitch);
	CalibrationHeadYawSamples.Add(RawHeadRotation.Yaw);
	CalibrationHeadRollSamples.Add(RawHeadRotation.Roll);
	CalibrationLeftArmSamples.Add(bSwapUpperArms
		? RightArmTrackingState.LastStableRawAngle
		: LeftArmTrackingState.LastStableRawAngle);
	CalibrationRightArmSamples.Add(bSwapUpperArms
		? LeftArmTrackingState.LastStableRawAngle
		: RightArmTrackingState.LastStableRawAngle);
	CalibrationLeftArmForwardSamples.Add(bSwapUpperArms
		? RightArmTrackingState.LastStableRawForwardAngle
		: LeftArmTrackingState.LastStableRawForwardAngle);
	CalibrationRightArmForwardSamples.Add(bSwapUpperArms
		? LeftArmTrackingState.LastStableRawForwardAngle
		: RightArmTrackingState.LastStableRawForwardAngle);
}

float UVPAnimInstance::CalculateCircularMeanDegrees(const TArray<float>& Samples)
{
	if (Samples.IsEmpty())
	{
		return 0.0f;
	}

	double SumSin = 0.0;
	double SumCos = 0.0;
	for (const float Sample : Samples)
	{
		const double Radians = FMath::DegreesToRadians(
			static_cast<double>(FRotator::NormalizeAxis(Sample)));
		SumSin += FMath::Sin(Radians);
		SumCos += FMath::Cos(Radians);
	}
	return FRotator::NormalizeAxis(static_cast<float>(
		FMath::RadiansToDegrees(FMath::Atan2(SumSin, SumCos))));
}

FString UVPAnimInstance::ResolveTrackingProfileId() const
{
	FString ExplicitId = AvatarTrackingProfileId;
	ExplicitId.TrimStartAndEndInline();
	if (!ExplicitId.IsEmpty())
	{
		return ExplicitId;
	}

	const USkeletalMeshComponent* MeshComponent = GetSkelMeshComponent();
	const USkeletalMesh* SkeletalMesh = MeshComponent ? MeshComponent->GetSkeletalMeshAsset() : nullptr;
	return SkeletalMesh ? SkeletalMesh->GetPathName() : FString();
}

FVPAvatarTrackingProfile UVPAnimInstance::BuildTrackingProfile() const
{
	FVPAvatarTrackingProfile Profile;
	Profile.bSwapUpperArms = bSwapUpperArms;
	Profile.bInvertLeftUpperArm = bInvertLeftUpperArm;
	Profile.bInvertRightUpperArm = bInvertRightUpperArm;
	Profile.RotationAxis = UpperArmRotationAxis;
	Profile.LeftGain = LeftUpperArmGain;
	Profile.RightGain = RightUpperArmGain;
	Profile.MinAngle = UpperArmMinAngle;
	Profile.MaxAngle = UpperArmMaxAngle;
	Profile.InputSmoothingSpeed = UpperArmInputSmoothingSpeed;
	Profile.MaxInputJump = UpperArmMaxInputJump;
	Profile.bHasNeutralCalibration = bHasNeutralCalibration;
	Profile.NeutralCalibrationVersion = 2;
	Profile.HeadNeutralRotation = HeadNeutralRotation;
	Profile.LeftNeutralInputAngle = LeftNeutralInputAngle;
	Profile.RightNeutralInputAngle = RightNeutralInputAngle;
	Profile.LeftNeutralForwardAngle = LeftNeutralForwardAngle;
	Profile.RightNeutralForwardAngle = RightNeutralForwardAngle;
	return Profile;
}

void UVPAnimInstance::ApplyTrackingProfile(const FVPAvatarTrackingProfile& Profile)
{
	bSwapUpperArms = Profile.bSwapUpperArms;
	bInvertLeftUpperArm = Profile.bInvertLeftUpperArm;
	bInvertRightUpperArm = Profile.bInvertRightUpperArm;
	UpperArmRotationAxis = Profile.RotationAxis;
	LeftUpperArmGain = FMath::Clamp(Profile.LeftGain, 0.25f, 2.0f);
	RightUpperArmGain = FMath::Clamp(Profile.RightGain, 0.25f, 2.0f);
	UpperArmMinAngle = FMath::Clamp(Profile.MinAngle, -90.0f, 0.0f);
	UpperArmMaxAngle = FMath::Clamp(Profile.MaxAngle, 0.0f, 180.0f);
	UpperArmInputSmoothingSpeed = FMath::Clamp(Profile.InputSmoothingSpeed, 0.1f, 60.0f);
	UpperArmMaxInputJump = FMath::Clamp(Profile.MaxInputJump, 30.0f, 180.0f);
	const bool bCalibrationCompatible = Profile.NeutralCalibrationVersion == 2;
	bHasNeutralCalibration = Profile.bHasNeutralCalibration && bCalibrationCompatible;
	HeadNeutralRotation = bCalibrationCompatible
		? Profile.HeadNeutralRotation
		: FRotator::ZeroRotator;
	LeftNeutralInputAngle = bCalibrationCompatible
		? FRotator::NormalizeAxis(Profile.LeftNeutralInputAngle)
		: 0.0f;
	RightNeutralInputAngle = bCalibrationCompatible
		? FRotator::NormalizeAxis(Profile.RightNeutralInputAngle)
		: 0.0f;
	LeftNeutralForwardAngle = bCalibrationCompatible
		? FRotator::NormalizeAxis(Profile.LeftNeutralForwardAngle)
		: 0.0f;
	RightNeutralForwardAngle = bCalibrationCompatible
		? FRotator::NormalizeAxis(Profile.RightNeutralForwardAngle)
		: 0.0f;
	ResetUpperArmInputFilters();
}

bool UVPAnimInstance::LoadTrackingProfile()
{
	bProfileInitialized = true;
	const FString ProfileId = ResolveTrackingProfileId();
	if (ProfileId.IsEmpty() ||
		!UGameplayStatics::DoesSaveGameExist(TrackingProfileSaveSlot, TrackingProfileUserIndex))
	{
		return false;
	}

	const UVPTrackingProfileSaveGame* SaveGame = Cast<UVPTrackingProfileSaveGame>(
		UGameplayStatics::LoadGameFromSlot(TrackingProfileSaveSlot, TrackingProfileUserIndex));
	if (!SaveGame || SaveGame->SchemaVersion != 1)
	{
		return false;
	}

	const FVPAvatarTrackingProfile* Profile = SaveGame->Profiles.Find(ProfileId);
	if (!Profile)
	{
		return false;
	}

	ApplyTrackingProfile(*Profile);
	return true;
}

bool UVPAnimInstance::SaveTrackingProfile()
{
	const FString ProfileId = ResolveTrackingProfileId();
	if (ProfileId.IsEmpty())
	{
		return false;
	}

	UVPTrackingProfileSaveGame* SaveGame = nullptr;
	if (UGameplayStatics::DoesSaveGameExist(TrackingProfileSaveSlot, TrackingProfileUserIndex))
	{
		SaveGame = Cast<UVPTrackingProfileSaveGame>(
			UGameplayStatics::LoadGameFromSlot(TrackingProfileSaveSlot, TrackingProfileUserIndex));
	}
	if (!SaveGame || SaveGame->SchemaVersion != 1)
	{
		SaveGame = Cast<UVPTrackingProfileSaveGame>(
			UGameplayStatics::CreateSaveGameObject(UVPTrackingProfileSaveGame::StaticClass()));
	}
	if (!SaveGame)
	{
		return false;
	}

	SaveGame->SchemaVersion = 1;
	SaveGame->Profiles.Add(ProfileId, BuildTrackingProfile());
	return UGameplayStatics::SaveGameToSlot(
		SaveGame, TrackingProfileSaveSlot, TrackingProfileUserIndex);
}

void UVPAnimInstance::ResetTrackingProfile()
{
	ApplyTrackingProfile(FVPAvatarTrackingProfile());
	CalibrationState = EVPCalibrationState::Idle;
	CalibrationStatus = TEXT("프로필을 기본값으로 초기화했습니다.");
	RejectedUpperArmInputCount = 0;
	SaveTrackingProfile();
}

bool UVPAnimInstance::DeleteTrackingProfileById(const FString& ProfileId)
{
	if (ProfileId.IsEmpty() ||
		!UGameplayStatics::DoesSaveGameExist(TrackingProfileSaveSlot, TrackingProfileUserIndex))
	{
		return true;
	}

	UVPTrackingProfileSaveGame* SaveGame = Cast<UVPTrackingProfileSaveGame>(
		UGameplayStatics::LoadGameFromSlot(TrackingProfileSaveSlot, TrackingProfileUserIndex));
	if (!SaveGame || SaveGame->SchemaVersion != 1)
	{
		return false;
	}
	SaveGame->Profiles.Remove(ProfileId);
	return UGameplayStatics::SaveGameToSlot(
		SaveGame, TrackingProfileSaveSlot, TrackingProfileUserIndex);
}

bool UVPAnimInstance::PruneManagedTrackingProfiles(
	const TSet<FString>& ValidAvatarIds,
	int32& OutRemovedCount)
{
	OutRemovedCount = 0;
	if (!UGameplayStatics::DoesSaveGameExist(
		TrackingProfileSaveSlot, TrackingProfileUserIndex))
	{
		return true;
	}

	UVPTrackingProfileSaveGame* SaveGame = Cast<UVPTrackingProfileSaveGame>(
		UGameplayStatics::LoadGameFromSlot(
			TrackingProfileSaveSlot, TrackingProfileUserIndex));
	if (!SaveGame || SaveGame->SchemaVersion != 1)
	{
		return false;
	}

	OutRemovedCount = RemoveManagedTrackingProfilesNotIn(
		SaveGame->Profiles, ValidAvatarIds);
	return OutRemovedCount == 0 || UGameplayStatics::SaveGameToSlot(
		SaveGame, TrackingProfileSaveSlot, TrackingProfileUserIndex);
}

int32 UVPAnimInstance::RemoveManagedTrackingProfilesNotIn(
	TMap<FString, FVPAvatarTrackingProfile>& Profiles,
	const TSet<FString>& ValidAvatarIds)
{
	int32 RemovedCount = 0;
	for (auto It = Profiles.CreateIterator(); It; ++It)
	{
		if (IsManagedAvatarProfileId(It.Key()) &&
			!ValidAvatarIds.Contains(It.Key()))
		{
			It.RemoveCurrent();
			++RemovedCount;
		}
	}
	return RemovedCount;
}

void UVPAnimInstance::SetUpperArmMappingOptions(
	bool bSwap,
	bool bInvertLeft,
	bool bInvertRight)
{
	if (bSwapUpperArms != bSwap)
	{
		Swap(LeftNeutralInputAngle, RightNeutralInputAngle);
		Swap(LeftNeutralForwardAngle, RightNeutralForwardAngle);
	}
	bSwapUpperArms = bSwap;
	bInvertLeftUpperArm = bInvertLeft;
	bInvertRightUpperArm = bInvertRight;
	ResetUpperArmInputFilters();
	SaveTrackingProfile();
}

bool UVPAnimInstance::StartArmValidation()
{
	if (!bHasNeutralCalibration)
	{
		ArmValidationStage = EVPArmValidationStage::Failed;
		ArmValidationStatus = TEXT("팔 검증 전에 중립 자세 캘리브레이션이 필요합니다.");
		ArmValidationProgress = 0.0f;
		return false;
	}

	ArmValidationStage = EVPArmValidationStage::Neutral;
	ArmValidationStatus = TEXT("1/4: 정면을 보고 양팔을 몸 옆에 내린 채 1초 유지하세요.");
	ArmValidationProgress = 0.0f;
	ValidationStageElapsedSeconds = 0.0f;
	ValidationPoseHeldSeconds = 0.0f;
	RejectedUpperArmInputCount = 0;
	return true;
}

void UVPAnimInstance::CancelArmValidation()
{
	ArmValidationStage = EVPArmValidationStage::Idle;
	ArmValidationStatus = TEXT("팔 검증 대기");
	ArmValidationProgress = 0.0f;
	ValidationStageElapsedSeconds = 0.0f;
	ValidationPoseHeldSeconds = 0.0f;
}

bool UVPAnimInstance::IsArmValidationPoseSatisfied(
	EVPArmValidationStage Stage,
	float LeftOutputAngle,
	float RightOutputAngle,
	float NeutralTolerance,
	float RaisedThreshold,
	float OtherArmTolerance)
{
	const float LeftMagnitude = FMath::Abs(LeftOutputAngle);
	const float RightMagnitude = FMath::Abs(RightOutputAngle);
	switch (Stage)
	{
	case EVPArmValidationStage::Neutral:
	case EVPArmValidationStage::NeutralAfterLeft:
		return LeftMagnitude <= NeutralTolerance && RightMagnitude <= NeutralTolerance;
	case EVPArmValidationStage::LeftArmRaised:
		return LeftMagnitude >= RaisedThreshold && RightMagnitude <= OtherArmTolerance;
	case EVPArmValidationStage::RightArmRaised:
		return RightMagnitude >= RaisedThreshold && LeftMagnitude <= OtherArmTolerance;
	default:
		return false;
	}
}

FString UVPAnimInstance::BuildArmValidationFeedback(
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
	float OtherArmTolerance)
{
	if (!bLeftTracked && !bRightTracked)
	{
		return FString::Printf(
			TEXT("양쪽 어깨와 팔꿈치를 화면 안쪽에 보이게 조정하세요.\n왼팔 %.2f · 오른팔 %.2f · 필요 %.2f"),
			LeftConfidence,
			RightConfidence,
			AcquireConfidenceThreshold);
	}
	if (!bLeftTracked)
	{
		return FString::Printf(
			TEXT("본인 왼쪽 어깨와 팔꿈치를 화면 안쪽에 보이게 조정하세요.\n신뢰도 %.2f · 필요 %.2f"),
			LeftConfidence,
			AcquireConfidenceThreshold);
	}
	if (!bRightTracked)
	{
		return FString::Printf(
			TEXT("본인 오른쪽 어깨와 팔꿈치를 화면 안쪽에 보이게 조정하세요.\n신뢰도 %.2f · 필요 %.2f"),
			RightConfidence,
			AcquireConfidenceThreshold);
	}

	const float LeftMagnitude = FMath::Abs(LeftOutputAngle);
	const float RightMagnitude = FMath::Abs(RightOutputAngle);
	switch (Stage)
	{
	case EVPArmValidationStage::Neutral:
	case EVPArmValidationStage::NeutralAfterLeft:
		if (LeftMagnitude > NeutralTolerance && RightMagnitude > NeutralTolerance)
		{
			return FString::Printf(
				TEXT("양팔을 몸 옆으로 완전히 내리세요.\n왼팔 %.0f° · 오른팔 %.0f° · 허용 %.0f° 이하"),
				LeftMagnitude,
				RightMagnitude,
				NeutralTolerance);
		}
		if (LeftMagnitude > NeutralTolerance)
		{
			return FString::Printf(
				TEXT("본인 왼팔을 몸 옆으로 완전히 내리세요.\n현재 %.0f° · 허용 %.0f° 이하"),
				LeftMagnitude,
				NeutralTolerance);
		}
		if (RightMagnitude > NeutralTolerance)
		{
			return FString::Printf(
				TEXT("본인 오른팔을 몸 옆으로 완전히 내리세요.\n현재 %.0f° · 허용 %.0f° 이하"),
				RightMagnitude,
				NeutralTolerance);
		}
		return TEXT("좋습니다. 정면을 보고 양팔을 내린 채 그대로 유지하세요.");

	case EVPArmValidationStage::LeftArmRaised:
		if (RightMagnitude > OtherArmTolerance)
		{
			return FString::Printf(
				TEXT("본인 오른팔을 몸 옆으로 완전히 내리세요.\n현재 %.0f° · 허용 %.0f° 이하"),
				RightMagnitude,
				OtherArmTolerance);
		}
		if (LeftMagnitude < RaisedThreshold)
		{
			return FString::Printf(
				TEXT("본인 왼팔을 옆으로 어깨 높이까지 더 드세요.\n현재 %.0f° · 필요 %.0f° 이상"),
				LeftMagnitude,
				RaisedThreshold);
		}
		return TEXT("좋습니다. 오른팔은 내리고 왼팔을 든 채 그대로 유지하세요.");

	case EVPArmValidationStage::RightArmRaised:
		if (LeftMagnitude > OtherArmTolerance)
		{
			return FString::Printf(
				TEXT("본인 왼팔을 몸 옆으로 완전히 내리세요.\n현재 %.0f° · 허용 %.0f° 이하"),
				LeftMagnitude,
				OtherArmTolerance);
		}
		if (RightMagnitude < RaisedThreshold)
		{
			return FString::Printf(
				TEXT("본인 오른팔을 옆으로 어깨 높이까지 더 드세요.\n현재 %.0f° · 필요 %.0f° 이상"),
				RightMagnitude,
				RaisedThreshold);
		}
		return TEXT("좋습니다. 왼팔은 내리고 오른팔을 든 채 그대로 유지하세요.");

	default:
		return TEXT("팔 검증 대기");
	}
}

void UVPAnimInstance::TickArmValidation(float DeltaSeconds)
{
	if (ArmValidationStage == EVPArmValidationStage::Idle ||
		ArmValidationStage == EVPArmValidationStage::Passed ||
		ArmValidationStage == EVPArmValidationStage::Failed)
	{
		return;
	}

	ArmValidationStatus = BuildArmValidationFeedback(
		ArmValidationStage,
		bLeftUpperArmTracked,
		bRightUpperArmTracked,
		LeftUpperArmOutputAngle,
		RightUpperArmOutputAngle,
		LeftUpperArmConfidence,
		RightUpperArmConfidence,
		UpperArmConfidenceThreshold,
		ValidationNeutralTolerance,
		ValidationRaisedThreshold,
		ValidationOtherArmTolerance);

	ValidationStageElapsedSeconds += DeltaSeconds;
	if (ValidationStageElapsedSeconds >= ValidationStageTimeoutSeconds)
	{
		FailArmValidation(FString::Printf(
			TEXT("제한 시간 초과 · %s"),
			*ArmValidationStatus));
		return;
	}

	const bool bPoseSatisfied = bLeftUpperArmTracked && bRightUpperArmTracked &&
		IsArmValidationPoseSatisfied(
		ArmValidationStage,
		LeftUpperArmOutputAngle,
		RightUpperArmOutputAngle,
		ValidationNeutralTolerance,
		ValidationRaisedThreshold,
		ValidationOtherArmTolerance);
	ValidationPoseHeldSeconds = bPoseSatisfied
		? ValidationPoseHeldSeconds + DeltaSeconds
		: 0.0f;

	float StageBase = 0.0f;
	switch (ArmValidationStage)
	{
	case EVPArmValidationStage::LeftArmRaised: StageBase = 0.25f; break;
	case EVPArmValidationStage::NeutralAfterLeft: StageBase = 0.5f; break;
	case EVPArmValidationStage::RightArmRaised: StageBase = 0.75f; break;
	default: break;
	}
	ArmValidationProgress = FMath::Clamp(
		StageBase + 0.25f * ValidationPoseHeldSeconds / FMath::Max(0.1f, ValidationPoseHoldSeconds),
		0.0f,
		1.0f);

	if (ValidationPoseHeldSeconds >= ValidationPoseHoldSeconds)
	{
		AdvanceArmValidation();
	}
}

void UVPAnimInstance::AdvanceArmValidation()
{
	ValidationStageElapsedSeconds = 0.0f;
	ValidationPoseHeldSeconds = 0.0f;
	switch (ArmValidationStage)
	{
	case EVPArmValidationStage::Neutral:
		ArmValidationStage = EVPArmValidationStage::LeftArmRaised;
		ArmValidationStatus = TEXT("2/4: 본인 오른팔은 내리고 왼팔을 옆으로 어깨 높이(약 90도)까지 들어 1초 유지하세요.");
		break;
	case EVPArmValidationStage::LeftArmRaised:
		ArmValidationStage = EVPArmValidationStage::NeutralAfterLeft;
		ArmValidationStatus = TEXT("3/4: 왼팔을 몸 옆으로 다시 내리고 양팔 중립을 1초 유지하세요.");
		break;
	case EVPArmValidationStage::NeutralAfterLeft:
		ArmValidationStage = EVPArmValidationStage::RightArmRaised;
		ArmValidationStatus = TEXT("4/4: 본인 왼팔은 내리고 오른팔을 옆으로 어깨 높이(약 90도)까지 들어 1초 유지하세요.");
		break;
	case EVPArmValidationStage::RightArmRaised:
		ArmValidationStage = EVPArmValidationStage::Passed;
		ArmValidationProgress = 1.0f;
		ArmValidationStatus = FString::Printf(
			TEXT("팔 검증 통과. 거부된 이상치 입력: %d회"), RejectedUpperArmInputCount);
		break;
	default:
		FailArmValidation(TEXT("알 수 없는 팔 검증 단계입니다."));
		break;
	}
}

void UVPAnimInstance::FailArmValidation(const FString& Reason)
{
	ArmValidationStage = EVPArmValidationStage::Failed;
	ArmValidationStatus = FString::Printf(TEXT("팔 검증 실패: %s"), *Reason);
}
