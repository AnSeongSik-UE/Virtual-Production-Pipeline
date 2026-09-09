#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "VPTrackingProfile.generated.h"

UENUM(BlueprintType)
enum class EVPArmRotationAxis : uint8
{
	Pitch,
	Yaw,
	Roll
};

UENUM(BlueprintType)
enum class EVPCalibrationState : uint8
{
	Idle,
	CountingDown,
	WaitingForTracking,
	Succeeded,
	Failed
};

UENUM(BlueprintType)
enum class EVPArmValidationStage : uint8
{
	Idle,
	Neutral,
	LeftArmRaised,
	NeutralAfterLeft,
	RightArmRaised,
	Passed,
	Failed
};

/** Avatar-specific settings and neutral calibration values. */
USTRUCT(BlueprintType)
struct VPTRACKERRECEIVER_API FVPAvatarTrackingProfile
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	bool bSwapUpperArms = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	bool bInvertLeftUpperArm = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	bool bInvertRightUpperArm = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	EVPArmRotationAxis RotationAxis = EVPArmRotationAxis::Roll;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float LeftGain = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float RightGain = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float MinAngle = -30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float MaxAngle = 140.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float InputSmoothingSpeed = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float MaxInputJump = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	bool bHasNeutralCalibration = false;

	/** Calibration payload revision; older pose-based head calibration is not reused. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	int32 NeutralCalibrationVersion = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	FRotator HeadNeutralRotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float LeftNeutralInputAngle = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float RightNeutralInputAngle = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float LeftNeutralForwardAngle = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "VP Profile")
	float RightNeutralForwardAngle = 0.0f;
};

/** Local SaveGame container. Profiles are keyed by an explicit id or skeletal-mesh asset path. */
UCLASS()
class VPTRACKERRECEIVER_API UVPTrackingProfileSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(SaveGame)
	int32 SchemaVersion = 1;

	UPROPERTY(SaveGame)
	TMap<FString, FVPAvatarTrackingProfile> Profiles;
};
