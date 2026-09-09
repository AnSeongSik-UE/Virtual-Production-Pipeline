#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstanceProxy.h"
#include "VPAnimInstance.h"
#include "VPRuntimeAvatarAnimInstance.generated.h"

/** Thread-safe proxy that applies the tracking rotations to any compatible VRM skeleton. */
struct FVPRuntimeAvatarAnimInstanceProxy final : public FAnimInstanceProxy
{
	FVPRuntimeAvatarAnimInstanceProxy() = default;
	explicit FVPRuntimeAvatarAnimInstanceProxy(UAnimInstance* Instance)
		: FAnimInstanceProxy(Instance)
	{
	}

	virtual void PreUpdate(UAnimInstance* InAnimInstance, float DeltaSeconds) override;
	virtual bool Evaluate(FPoseContext& Output) override;

private:
	FName HeadBoneName;
	FName NeckBoneName;
	FName LeftUpperArmBoneName;
	FName RightUpperArmBoneName;
	FRotator HeadRotation = FRotator::ZeroRotator;
	FRotator LeftUpperArmRotation = FRotator::ZeroRotator;
	FRotator RightUpperArmRotation = FRotator::ZeroRotator;
	FVector LeftArmLiftAxisParentSpace = FVector::ZeroVector;
	FVector RightArmLiftAxisParentSpace = FVector::ZeroVector;
	FVector LeftArmUpAxisParentSpace = FVector::UpVector;
	FVector RightArmUpAxisParentSpace = FVector::UpVector;
	float LeftArmReferenceElevationDegrees = 0.0f;
	float RightArmReferenceElevationDegrees = 0.0f;
	float LeftUpperArmAppliedAngle = 0.0f;
	float RightUpperArmAppliedAngle = 0.0f;
	float LeftUpperArmAppliedForwardAngle = 0.0f;
	float RightUpperArmAppliedForwardAngle = 0.0f;
	bool bHasLeftArmLiftAxis = false;
	bool bHasRightArmLiftAxis = false;
};

/** Native animation instance used by VRM files loaded while the application is running. */
UCLASS(Transient)
class VPPIPELINE_API UVPRuntimeAvatarAnimInstance : public UVPAnimInstance
{
	GENERATED_BODY()

public:
	/** Optional neck bone receives part of the face rotation for natural motion. */
	FName NeckBoneName;

	/** Humanoid lower-arm names are used only to derive the upper-arm lift plane. */
	FName LeftLowerArmBoneName;
	FName RightLowerArmBoneName;

	/** Cache the per-avatar reference-pose axes after the runtime bone names are assigned. */
	void ConfigureReferencePoseArmLiftAxes();

	/** Pure geometry helper used by runtime setup and automation tests. */
	static bool CalculateArmLiftAxis(
		const FVector& ArmDirectionComponentSpace,
		const FQuat& ParentRotationComponentSpace,
		FVector& OutAxisParentSpace,
		float& OutReferenceElevationDegrees);

	/** Keep tracking/calibration angles unchanged while displaying a relaxed A-pose at rest. */
	static float MapTrackingElevationToVisualElevation(
		float TrackingElevationDegrees,
		float RestElevationDegrees = 35.0f);

	static FRotator ScaleBoneRotation(const FRotator& Rotation, float Weight);

	bool HasLeftArmLiftAxis() const { return bHasLeftArmLiftAxis; }
	bool HasRightArmLiftAxis() const { return bHasRightArmLiftAxis; }
	const FVector& GetLeftArmLiftAxis() const { return LeftArmLiftAxisParentSpace; }
	const FVector& GetRightArmLiftAxis() const { return RightArmLiftAxisParentSpace; }
	const FVector& GetLeftArmUpAxis() const { return LeftArmUpAxisParentSpace; }
	const FVector& GetRightArmUpAxis() const { return RightArmUpAxisParentSpace; }
	float GetLeftArmReferenceElevation() const { return LeftArmReferenceElevationDegrees; }
	float GetRightArmReferenceElevation() const { return RightArmReferenceElevationDegrees; }

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;

private:
	FVector LeftArmLiftAxisParentSpace = FVector::ZeroVector;
	FVector RightArmLiftAxisParentSpace = FVector::ZeroVector;
	FVector LeftArmUpAxisParentSpace = FVector::UpVector;
	FVector RightArmUpAxisParentSpace = FVector::UpVector;
	float LeftArmReferenceElevationDegrees = 0.0f;
	float RightArmReferenceElevationDegrees = 0.0f;
	bool bHasLeftArmLiftAxis = false;
	bool bHasRightArmLiftAxis = false;
};
