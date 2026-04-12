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
