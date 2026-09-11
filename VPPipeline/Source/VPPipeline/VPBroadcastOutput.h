#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/SaveGame.h"
#include "VPBroadcastOutput.generated.h"

class FSocket;
class UDirectionalLightComponent;
class USceneCaptureComponent2D;
class USceneComponent;
class USpoutSenderComponent;
class UTextureRenderTarget2D;

UENUM(BlueprintType)
enum class EVPBroadcastBackgroundMode : uint8
{
	BackgroundRemoved UMETA(DisplayName = "Background Removed"),
	SolidColor UMETA(DisplayName = "Solid Color")
};

USTRUCT(BlueprintType)
struct FVPAvatarCameraSettings
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	bool bHasSavedView = false;

	UPROPERTY(SaveGame)
	FTransform RelativeTransform = FTransform::Identity;

	UPROPERTY(SaveGame)
	float FieldOfView = 40.0f;

	UPROPERTY(SaveGame)
	bool bHasOrbitPivot = false;

	UPROPERTY(SaveGame)
	FVector RelativeOrbitPivot = FVector::ZeroVector;
};

struct FVPInputCameraDevice
{
	FString Id;
	FString DisplayName;
	bool bIsVirtual = false;

	bool operator==(const FVPInputCameraDevice& Other) const
	{
		return Id == Other.Id && DisplayName == Other.DisplayName && bIsVirtual == Other.bIsVirtual;
	}
};

UCLASS()
class UVPBroadcastSettingsSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(SaveGame)
	EVPBroadcastBackgroundMode BackgroundMode = EVPBroadcastBackgroundMode::BackgroundRemoved;

	UPROPERTY(SaveGame)
	FLinearColor BackgroundColor = FLinearColor(0.0f, 1.0f, 0.0f, 1.0f);

	UPROPERTY(SaveGame)
	float AvatarExposureStops = 0.0f;

	UPROPERTY(SaveGame)
	int32 OutputFramesPerSecond = 60;

	UPROPERTY(SaveGame)
	TMap<FString, FVPAvatarCameraSettings> AvatarCameraSettings;
};

/**
 * Renders only the tracked avatar into one shared texture. The same texture is
 * sampled by the local preview and sent to external receivers through Spout.
 */
UCLASS()
class VPPIPELINE_API AVPBroadcastOutput : public AActor
{
	GENERATED_BODY()

public:
	AVPBroadcastOutput();

	virtual void Tick(float DeltaSeconds) override;

	UTextureRenderTarget2D* GetBroadcastTexture() const { return BroadcastTexture; }
	EVPBroadcastBackgroundMode GetBackgroundMode() const { return BackgroundMode; }
	FLinearColor GetBackgroundColor() const { return BackgroundColor; }
	float GetAvatarExposureStops() const { return AvatarExposureStops; }
	int32 GetOutputFPS() const { return OutputFramesPerSecond; }
	const TArray<FVPInputCameraDevice>& GetInputCameraDevices() const { return InputCameraDevices; }
	const FString& GetActiveInputCameraId() const { return ActiveInputCameraId; }
	const FString& GetInputCameraStatusText() const { return InputCameraStatusText; }
	const FString& GetInputCameraNoticeText() const { return InputCameraNoticeText; }
	int32 GetInputCameraListRevision() const { return InputCameraListRevision; }
	int32 GetInputCameraNoticeRevision() const { return InputCameraNoticeRevision; }
	bool IsInputCameraNoticeError() const { return bInputCameraNoticeError; }
	bool HasActiveInputCamera() const { return !ActiveInputCameraId.IsEmpty(); }
	FString GetBackgroundColorHex() const;
	FString GetBackgroundModeLabel() const;
	FString GetSenderName() const { return SpoutSenderName; }
	bool IsAvatarCaptureReady() const { return CapturedAvatar.IsValid(); }
	bool WasSpoutStartRequested() const { return bSpoutStartRequested; }
	const FString& GetActiveAvatarId() const { return ActiveAvatarId; }

	void SetBackgroundMode(EVPBroadcastBackgroundMode NewMode);
	void SetBackgroundColor(const FLinearColor& NewColor);
	void PreviewBackgroundColor(const FLinearColor& NewColor);
	void CommitBackgroundColor();
	void PreviewAvatarExposure(float NewExposureStops);
	void CommitAvatarExposure();
	void PreviewOutputFPS(int32 NewFPS);
	void CommitOutputFPS();
	void RequestInputCameraList();
	void SelectInputCamera(const FString& DeviceId);
	bool SetBackgroundColorHex(const FString& HexColor);
	void SetCapturedAvatar(AActor* AvatarActor, const FString& AvatarId);
	void ClearCapturedAvatar();
	void PanCamera(const FVector2D& ScreenDelta);
	void OrbitCamera(const FVector2D& ScreenDelta);
	void AdjustCameraZoom(float WheelDelta);
	void ResetCameraToFullBody();
	bool DeleteCameraProfile(const FString& AvatarId);
	bool PruneCameraProfiles(const TSet<FString>& ValidAvatarIds, int32& OutRemovedCount);

	static bool ParseHexColor(const FString& HexColor, FLinearColor& OutColor);
	static FLinearColor FromSrgb8(uint8 Red, uint8 Green, uint8 Blue);
	static float SanitizeAvatarExposure(float ExposureStops);
	static int32 SanitizeOutputFPS(int32 FramesPerSecond);
	static int32 RemoveCameraProfilesNotIn(
		TMap<FString, FVPAvatarCameraSettings>& CameraProfiles,
		const TSet<FString>& ValidAvatarIds);
	static FTransform CalculateInitialCameraTransform(
		const FVector& BoundsOrigin,
		float CameraDistance,
		const FRotator& ReferenceCameraRotation);
	static FTransform CalculateOrbitCameraTransform(
		const FTransform& CameraTransform,
		const FVector& OrbitPivot,
		const FVector2D& ScreenDelta);
	static FRotator GetAvatarFrontCameraRotation();
	static FVector CalculateBodyCenteredFramingOrigin(
		const FVector& BoundsOrigin,
		const FVector& BodyAnchor,
		const FRotator& ReferenceCameraRotation);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneCaptureComponent2D> CaptureComponent;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UDirectionalLightComponent> AvatarKeyLight;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USpoutSenderComponent> SpoutSender;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> BroadcastTexture;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> AvatarCaptureTexture;

	TWeakObjectPtr<AActor> CapturedAvatar;
	FSocket* ControlSocket = nullptr;
	EVPBroadcastBackgroundMode BackgroundMode = EVPBroadcastBackgroundMode::BackgroundRemoved;
	FLinearColor BackgroundColor = FLinearColor(0.0f, 1.0f, 0.0f, 1.0f);
	float AvatarExposureStops = 0.0f;
	int32 OutputFramesPerSecond = 60;
	TArray<FVPInputCameraDevice> InputCameraDevices;
	FString ActiveInputCameraId;
	FString InputCameraStatusText = TEXT("입력 카메라 확인 중");
	FString InputCameraNoticeText;
	int32 InputCameraListRevision = 0;
	int32 InputCameraNoticeRevision = 0;
	bool bInputCameraNoticeError = false;
	bool bInputCameraListReceived = false;
	double LastInputCameraListRequestSeconds = -1.0;
	FString SpoutSenderName = TEXT("Virtual Production Pipeline");
	FString LifecycleSessionToken;
	FString ActiveAvatarId;
	TMap<FString, FVPAvatarCameraSettings> AvatarCameraSettings;
	FVector CameraOrbitPivot = FVector::ZeroVector;
	bool bCameraOrbitPivotValid = false;
	bool bSpoutStartRequested = false;
	bool bInitialCameraFramed = false;
	double LastLifecycleHeartbeatSeconds = 0.0;

	static constexpr int32 OutputWidth = 1280;
	static constexpr int32 OutputHeight = 720;
	static constexpr int32 ControlPort = 7001;
	static constexpr double LifecycleHeartbeatIntervalSeconds = 1.0;

	void CreateBroadcastTexture();
	void RenderBroadcastFrame();
	void FrameAvatarForInitialView();
	bool TryGetBodyFramingAnchor(FVector& OutAnchor) const;
	bool RestoreOrDeriveCameraOrbitPivot(const FVPAvatarCameraSettings& Settings);
	bool DeriveCameraOrbitPivot(FVector& OutPivot) const;
	bool RestoreCameraForActiveAvatar();
	void SaveCameraForActiveAvatar();
	void ApplyBackgroundColor();
	void ApplyOutputFPS();
	FLinearColor GetOutputClearColor() const;
	void LoadSettings();
	bool SaveSettings() const;
	void SendInputCameraCommand(const TCHAR* Action, const FString& DeviceId = FString());
	void PollControlResponses();
	void SendLifecycleCommand(const TCHAR* Event);
	void SendControlPayload(const FString& Payload);
};
