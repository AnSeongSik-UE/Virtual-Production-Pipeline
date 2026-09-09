#include "VPRuntimeAvatarAnimInstance.h"

#include "Animation/AnimNodeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

DEFINE_LOG_CATEGORY_STATIC(LogVPRuntimeAvatarAnimation, Log, All);

namespace
{
FTransform BuildReferenceComponentTransform(
	const FReferenceSkeleton& ReferenceSkeleton,
	int32 BoneIndex)
{
	FTransform ComponentTransform = FTransform::Identity;
	const TArray<FTransform>& RefPose = ReferenceSkeleton.GetRefBonePose();
	for (int32 CurrentIndex = BoneIndex;
		CurrentIndex != INDEX_NONE;
		CurrentIndex = ReferenceSkeleton.GetParentIndex(CurrentIndex))
	{
		ComponentTransform = ComponentTransform * RefPose[CurrentIndex];
	}
	return ComponentTransform;
}

bool CalculateReferenceArmAxis(
	const FReferenceSkeleton& ReferenceSkeleton,
	const FName UpperArmBoneName,
	const FName LowerArmBoneName,
	FVector& OutAxisParentSpace,
	FVector& OutUpAxisParentSpace,
	float& OutReferenceElevationDegrees)
{
	const int32 UpperArmIndex = ReferenceSkeleton.FindBoneIndex(UpperArmBoneName);
	const int32 LowerArmIndex = ReferenceSkeleton.FindBoneIndex(LowerArmBoneName);
	if (UpperArmIndex == INDEX_NONE || LowerArmIndex == INDEX_NONE)
	{
		return false;
	}

	const FTransform UpperArmComponent = BuildReferenceComponentTransform(
		ReferenceSkeleton, UpperArmIndex);
	const FTransform LowerArmComponent = BuildReferenceComponentTransform(
		ReferenceSkeleton, LowerArmIndex);
	const int32 ParentIndex = ReferenceSkeleton.GetParentIndex(UpperArmIndex);
	const FQuat ParentComponentRotation = ParentIndex == INDEX_NONE
		? FQuat::Identity
		: BuildReferenceComponentTransform(ReferenceSkeleton, ParentIndex).GetRotation();
	OutUpAxisParentSpace = ParentComponentRotation.UnrotateVector(FVector::UpVector).GetSafeNormal();
	return UVPRuntimeAvatarAnimInstance::CalculateArmLiftAxis(
		LowerArmComponent.GetLocation() - UpperArmComponent.GetLocation(),
		ParentComponentRotation,
		OutAxisParentSpace,
		OutReferenceElevationDegrees);
}
}

void FVPRuntimeAvatarAnimInstanceProxy::PreUpdate(
	UAnimInstance* InAnimInstance,
	float DeltaSeconds)
{
	FAnimInstanceProxy::PreUpdate(InAnimInstance, DeltaSeconds);
	if (const UVPRuntimeAvatarAnimInstance* RuntimeInstance =
		Cast<UVPRuntimeAvatarAnimInstance>(InAnimInstance))
	{
		HeadBoneName = RuntimeInstance->HeadBoneName;
		NeckBoneName = RuntimeInstance->NeckBoneName;
		LeftUpperArmBoneName = RuntimeInstance->LeftUpperArmBoneName;
		RightUpperArmBoneName = RuntimeInstance->RightUpperArmBoneName;
		HeadRotation = RuntimeInstance->HeadRotation;
		LeftUpperArmRotation = RuntimeInstance->LeftUpperArmRotation;
		RightUpperArmRotation = RuntimeInstance->RightUpperArmRotation;
		LeftUpperArmAppliedAngle = RuntimeInstance->LeftUpperArmAppliedAngle;
		RightUpperArmAppliedAngle = RuntimeInstance->RightUpperArmAppliedAngle;
		LeftUpperArmAppliedForwardAngle = RuntimeInstance->LeftUpperArmAppliedForwardAngle;
		RightUpperArmAppliedForwardAngle = RuntimeInstance->RightUpperArmAppliedForwardAngle;
		bHasLeftArmLiftAxis = RuntimeInstance->HasLeftArmLiftAxis();
		bHasRightArmLiftAxis = RuntimeInstance->HasRightArmLiftAxis();
		LeftArmLiftAxisParentSpace = RuntimeInstance->GetLeftArmLiftAxis();
		RightArmLiftAxisParentSpace = RuntimeInstance->GetRightArmLiftAxis();
		LeftArmUpAxisParentSpace = RuntimeInstance->GetLeftArmUpAxis();
		RightArmUpAxisParentSpace = RuntimeInstance->GetRightArmUpAxis();
		LeftArmReferenceElevationDegrees = RuntimeInstance->GetLeftArmReferenceElevation();
		RightArmReferenceElevationDegrees = RuntimeInstance->GetRightArmReferenceElevation();
	}
}

bool FVPRuntimeAvatarAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	Output.ResetToRefPose();
	const FBoneContainer& BoneContainer = Output.Pose.GetBoneContainer();

	auto ApplyAdditionalRotation = [&](const FName BoneName, const FRotator& Rotation)
	{
		if (BoneName.IsNone())
		{
			return;
		}
		const int32 MeshBoneIndex = BoneContainer.GetPoseBoneIndexForBoneName(BoneName);
		const FCompactPoseBoneIndex CompactIndex =
			BoneContainer.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBoneIndex));
		if (CompactIndex == INDEX_NONE)
		{
			return;
		}

		FTransform& BoneTransform = Output.Pose[CompactIndex];
		BoneTransform.SetRotation(
			(Rotation.Quaternion() * BoneTransform.GetRotation()).GetNormalized());
	};

	auto ApplyReferencePoseArmLift = [&](const FName BoneName,
		const FVector& AxisParentSpace,
		float AngleDegrees,
		float ReferenceElevationDegrees,
		const FVector& UpAxisParentSpace,
		float ForwardAngleDegrees,
		bool bLeft,
		bool bHasReferenceAxis,
		const FRotator& FallbackRotation)
	{
		if (!bHasReferenceAxis || BoneName.IsNone())
		{
			ApplyAdditionalRotation(BoneName, FallbackRotation);
			return;
		}
		const int32 MeshBoneIndex = BoneContainer.GetPoseBoneIndexForBoneName(BoneName);
		const FCompactPoseBoneIndex CompactIndex =
			BoneContainer.MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshBoneIndex));
		if (CompactIndex == INDEX_NONE)
		{
			return;
		}

		FTransform& BoneTransform = Output.Pose[CompactIndex];
		const float VisualElevationDegrees =
			UVPRuntimeAvatarAnimInstance::MapTrackingElevationToVisualElevation(AngleDegrees);
		const float RotationFromReference =
			VisualElevationDegrees - ReferenceElevationDegrees;
		const FQuat LiftRotation(
			AxisParentSpace,
			FMath::DegreesToRadians(RotationFromReference));
		const FQuat ForwardRotation(
			UpAxisParentSpace.GetSafeNormal(),
			FMath::DegreesToRadians(ForwardAngleDegrees * (bLeft ? 1.0f : -1.0f)));
		BoneTransform.SetRotation(
			(ForwardRotation * LiftRotation * BoneTransform.GetRotation()).GetNormalized());
	};

	if (!NeckBoneName.IsNone())
	{
		ApplyAdditionalRotation(
			NeckBoneName,
			UVPRuntimeAvatarAnimInstance::ScaleBoneRotation(HeadRotation, 0.3f));
		ApplyAdditionalRotation(
			HeadBoneName,
			UVPRuntimeAvatarAnimInstance::ScaleBoneRotation(HeadRotation, 0.7f));
	}
	else
	{
		ApplyAdditionalRotation(HeadBoneName, HeadRotation);
	}
	ApplyReferencePoseArmLift(
		LeftUpperArmBoneName,
		LeftArmLiftAxisParentSpace,
		LeftUpperArmAppliedAngle,
		LeftArmReferenceElevationDegrees,
		LeftArmUpAxisParentSpace,
		LeftUpperArmAppliedForwardAngle,
		true,
		bHasLeftArmLiftAxis,
		LeftUpperArmRotation);
	ApplyReferencePoseArmLift(
		RightUpperArmBoneName,
		RightArmLiftAxisParentSpace,
		RightUpperArmAppliedAngle,
		RightArmReferenceElevationDegrees,
		RightArmUpAxisParentSpace,
		RightUpperArmAppliedForwardAngle,
		false,
		bHasRightArmLiftAxis,
		RightUpperArmRotation);
	return true;
}

FRotator UVPRuntimeAvatarAnimInstance::ScaleBoneRotation(
	const FRotator& Rotation,
	float Weight)
{
	return FQuat::Slerp(
		FQuat::Identity,
		Rotation.Quaternion(),
		FMath::Clamp(Weight, 0.0f, 1.0f)).GetNormalized().Rotator();
}

float UVPRuntimeAvatarAnimInstance::MapTrackingElevationToVisualElevation(
	float TrackingElevationDegrees,
	float RestElevationDegrees)
{
	const float RestElevation = FMath::Clamp(RestElevationDegrees, 0.0f, 90.0f);
	if (TrackingElevationDegrees <= 0.0f)
	{
		return RestElevation;
	}
	if (TrackingElevationDegrees < 90.0f)
	{
		return FMath::Lerp(
			RestElevation,
			90.0f,
			TrackingElevationDegrees / 90.0f);
	}
	return TrackingElevationDegrees;
}

bool UVPRuntimeAvatarAnimInstance::CalculateArmLiftAxis(
	const FVector& ArmDirectionComponentSpace,
	const FQuat& ParentRotationComponentSpace,
	FVector& OutAxisParentSpace,
	float& OutReferenceElevationDegrees)
{
	const FVector ArmDirection = ArmDirectionComponentSpace.GetSafeNormal();
	if (ArmDirection.IsNearlyZero())
	{
		OutAxisParentSpace = FVector::ZeroVector;
		OutReferenceElevationDegrees = 0.0f;
		return false;
	}

	// A positive angle must rotate the reference arm direction toward avatar up.
	const FVector AxisComponentSpace = FVector::CrossProduct(
		ArmDirection, FVector::UpVector).GetSafeNormal();
	if (AxisComponentSpace.IsNearlyZero())
	{
		OutAxisParentSpace = FVector::ZeroVector;
		OutReferenceElevationDegrees = 0.0f;
		return false;
	}

	OutAxisParentSpace = ParentRotationComponentSpace.UnrotateVector(
		AxisComponentSpace).GetSafeNormal();
	OutReferenceElevationDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(
		FVector::DotProduct(ArmDirection, FVector::DownVector),
		-1.0f,
		1.0f)));
	return !OutAxisParentSpace.IsNearlyZero();
}

void UVPRuntimeAvatarAnimInstance::ConfigureReferencePoseArmLiftAxes()
{
	bHasLeftArmLiftAxis = false;
	bHasRightArmLiftAxis = false;
	LeftArmLiftAxisParentSpace = FVector::ZeroVector;
	RightArmLiftAxisParentSpace = FVector::ZeroVector;
	LeftArmReferenceElevationDegrees = 0.0f;
	RightArmReferenceElevationDegrees = 0.0f;
	LeftArmUpAxisParentSpace = FVector::UpVector;
	RightArmUpAxisParentSpace = FVector::UpVector;

	const USkeletalMeshComponent* MeshComponent = GetSkelMeshComponent();
	const USkeletalMesh* SkeletalMesh = MeshComponent
		? MeshComponent->GetSkeletalMeshAsset()
		: nullptr;
	if (!SkeletalMesh)
	{
		return;
	}

	const FReferenceSkeleton& ReferenceSkeleton = SkeletalMesh->GetRefSkeleton();
	bHasLeftArmLiftAxis = CalculateReferenceArmAxis(
		ReferenceSkeleton,
		LeftUpperArmBoneName,
		LeftLowerArmBoneName,
		LeftArmLiftAxisParentSpace,
		LeftArmUpAxisParentSpace,
		LeftArmReferenceElevationDegrees);
	bHasRightArmLiftAxis = CalculateReferenceArmAxis(
		ReferenceSkeleton,
		RightUpperArmBoneName,
		RightLowerArmBoneName,
		RightArmLiftAxisParentSpace,
		RightArmUpAxisParentSpace,
		RightArmReferenceElevationDegrees);
	UE_LOG(
		LogVPRuntimeAvatarAnimation,
		Display,
		TEXT("Reference-pose arm lift: left=%s %.1fdeg right=%s %.1fdeg"),
		bHasLeftArmLiftAxis ? TEXT("ready") : TEXT("Euler fallback"),
		LeftArmReferenceElevationDegrees,
		bHasRightArmLiftAxis ? TEXT("ready") : TEXT("Euler fallback"),
		RightArmReferenceElevationDegrees);
}

FAnimInstanceProxy* UVPRuntimeAvatarAnimInstance::CreateAnimInstanceProxy()
{
	return new FVPRuntimeAvatarAnimInstanceProxy(this);
}
