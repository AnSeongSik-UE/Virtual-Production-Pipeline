#include "VPAnimInstance.h"
#include "VPUDPReceiver.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/DataTable.h"

void UVPAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

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
}

void UVPAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
	Super::NativeUpdateAnimation(DeltaSeconds);

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
	}
}

void UVPAnimInstance::ApplyTrackingData(const FVPTrackingFrame& Frame)
{
	TrackingData = Frame;
	if (TrackingData.bIsValid)
	{
		ApplyBlendshapesToMorphTargets();
		UpdatePoseBoneTransforms();
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

	// Apply rest pose baseline first
	for (const auto& RestPair : RestPoseMorphTargets)
	{
		MeshComp->SetMorphTarget(RestPair.Key, FMath::Clamp(RestPair.Value, 0.0f, 1.0f));
	}

	FaceBlendshapes.Reset();

	for (const auto& Pair : TrackingData.FaceData.Blendshapes)
	{
		if (Pair.Key == FName("_neutral"))
		{
			continue;
		}

		FName TargetName = Pair.Key;
		float Scale = 1.0f;

		// Apply DataTable remapping if available
		if (bHasMapping)
		{
			const TPair<FName, float>* Mapping = CachedMapping.Find(Pair.Key);
			if (Mapping)
			{
				TargetName = Mapping->Key;
				Scale = Mapping->Value;
			}
			else
			{
				// ARKit name not in mapping table -> skip (only mapped targets applied)
				continue;
			}
		}

		const float RawValue = Pair.Value < BlendshapeDeadZone ? 0.0f : Pair.Value;
		const float FinalValue = FMath::Clamp(RawValue * FaceBlendWeight * Scale, 0.0f, 1.0f);
		MeshComp->SetMorphTarget(TargetName, FinalValue);
		FaceBlendshapes.Add(TargetName, FinalValue);
	}
}

void UVPAnimInstance::UpdateHeadRotation()
{
	if (!bEnableHeadTracking)
	{
		return;
	}

	// Need at least landmarks: 0(nose), 11(L shoulder), 12(R shoulder)
	const int32 NumLandmarks = TrackingData.PoseLandmarks.Num();
	if (NumLandmarks < 13)
	{
		return;
	}

	// MediaPipe landmark positions (normalized 0~1, X right, Y down)
	const FVector2D Nose(TrackingData.PoseLandmarks[0].Position.X, TrackingData.PoseLandmarks[0].Position.Y);
	const FVector2D LShoulder(TrackingData.PoseLandmarks[11].Position.X, TrackingData.PoseLandmarks[11].Position.Y);
	const FVector2D RShoulder(TrackingData.PoseLandmarks[12].Position.X, TrackingData.PoseLandmarks[12].Position.Y);

	// Also use depth (Z) for pitch
	const float NoseZ = TrackingData.PoseLandmarks[0].Position.Z;
	const float ShoulderZ = (TrackingData.PoseLandmarks[11].Position.Z + TrackingData.PoseLandmarks[12].Position.Z) * 0.5f;

	// Shoulder midpoint as reference
	const FVector2D ShoulderMid = (LShoulder + RShoulder) * 0.5f;
	const float ShoulderWidth = FVector2D::Distance(LShoulder, RShoulder);

	if (ShoulderWidth < 0.01f)
	{
		return;
	}

	// Roll: shoulder tilt angle (calculated first for compensation)
	const float ShoulderDeltaY = RShoulder.Y - LShoulder.Y;
	const float RollRad = FMath::Atan2(ShoulderDeltaY, ShoulderWidth);
	const float Roll = FMath::Clamp(FMath::RadiansToDegrees(RollRad) * -1.0f * HeadRotationScale, -25.0f, 25.0f);

	// Nose offset from shoulder center (normalized by shoulder width)
	const float RawOffsetX = (Nose.X - ShoulderMid.X) / ShoulderWidth;
	const float RawOffsetY = (Nose.Y - ShoulderMid.Y) / ShoulderWidth;

	// Roll compensation: rotate nose offset back by -Roll to remove tilt contamination
	const float CosR = FMath::Cos(-RollRad);
	const float SinR = FMath::Sin(-RollRad);
	const float NoseOffsetX = CosR * RawOffsetX - SinR * RawOffsetY;
	const float NoseOffsetY = SinR * RawOffsetX + CosR * RawOffsetY;

	// Yaw: compensated horizontal offset
	const float Yaw = FMath::Clamp(NoseOffsetX * 60.0f * HeadRotationScale, -45.0f, 45.0f);

	// Pitch: compensated vertical offset
	const float NeutralOffset = -0.8f; // neutral head position is above shoulders
	const float Pitch = FMath::Clamp((NoseOffsetY - NeutralOffset) * 60.0f * HeadRotationScale, -30.0f, 30.0f);

	// VRoid bone axis: Roll->Pitch, Yaw, Pitch->Roll with sign multipliers
	const FRotator TargetRotation(Roll * RollSign, Yaw * YawSign, Pitch * PitchSign);

	// Smooth
	FRotator NewSmoothed = FMath::Lerp(TargetRotation, SmoothedHeadRotation, HeadRotationSmoothing);

	// Rate limit: clamp max change per frame to prevent wild jumps
	FRotator Delta = (NewSmoothed - SmoothedHeadRotation).GetNormalized();
	Delta.Pitch = FMath::Clamp(Delta.Pitch, -HeadMaxDeltaPerFrame, HeadMaxDeltaPerFrame);
	Delta.Yaw = FMath::Clamp(Delta.Yaw, -HeadMaxDeltaPerFrame, HeadMaxDeltaPerFrame);
	Delta.Roll = FMath::Clamp(Delta.Roll, -HeadMaxDeltaPerFrame, HeadMaxDeltaPerFrame);

	SmoothedHeadRotation = (SmoothedHeadRotation + Delta).GetNormalized();
	HeadRotation = SmoothedHeadRotation;

	// HeadRotation is stored as a UPROPERTY for use in AnimBP (Transform Modify Bone node)
	// or can be read from Blueprint to drive bone rotation.
}

void UVPAnimInstance::UpdatePoseBoneTransforms()
{
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
