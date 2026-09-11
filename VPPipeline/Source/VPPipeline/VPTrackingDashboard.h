#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VPBroadcastOutput.h"
#include "VPTrackingData.h"
#include "VPTrackingProfile.h"
#include "VPTrackingDashboard.generated.h"

class UBorder;
class UButton;
class UComboBoxString;
class UHorizontalBox;
class UProgressBar;
class USizeBox;
class USlider;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;
class AVPBroadcastOutput;
class AVPAvatarManager;
class UVPAnimInstance;
class UVPUDPReceiver;

/** Code-native UMG dashboard used by the portfolio demo. */
UCLASS()
class VPPIPELINE_API UVPTrackingDashboard : public UUserWidget
{
	GENERATED_BODY()

public:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual int32 NativePaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual void NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent) override;
	virtual void NativeDestruct() override;

	/** Pure instruction mapping used by the dashboard and automation tests. */
	static FText BuildGuidanceText(
		EVPCalibrationState CalibrationState,
		EVPArmValidationStage ValidationStage,
		const FString& CalibrationStatus,
		const FString& ValidationStatus);

	/** Pure helpers shared with automation tests and the pose overlay painter. */
	static FVector2D ProjectPoseLandmark(
		const FVector& NormalizedPosition,
		const FVector2D& PanelOrigin,
		const FVector2D& PanelSize);
	static float GetPoseLandmarkConfidence(const FVPPoseLandmark& Landmark);
	static FText BuildPoseStatusText(
		const FVPTrackingFrame& Frame,
		float& OutWeakestConfidence);
	static FText BuildDeleteAvatarConfirmationText(const FString& DisplayName);
	static FText BuildTrackingStatusText(
		bool bPacketRateReady,
		float PacketsPerSecond,
		float SecondsSinceLastPacket,
		bool bFaceTracked,
		bool bPoseTracked,
		bool bLeftArmTracked,
		bool bRightArmTracked);
	static float CalculatePacketRate(int32 PacketDelta, double ElapsedSeconds);
	static bool ShouldRestartPacketRateSample(double UpdateGapSeconds);
	static bool ShouldShowGuidancePanel(
		EVPCalibrationState CalibrationState,
		EVPArmValidationStage ValidationStage,
		float SuccessDisplayRemainingSeconds,
		float CalibrationFailureDisplayRemainingSeconds);

	/** Shared by the dashboard button and the C keyboard shortcut. */
	void ToggleNeutralCalibration();
	void SetAvatarManager(AVPAvatarManager* InAvatarManager);
	bool IsModalOpen() const;
	bool CanUseCalibrationShortcut() const;

private:
	UPROPERTY(Transient)
	TObjectPtr<UVPAnimInstance> AnimInstance;

	UPROPERTY(Transient)
	TObjectPtr<UVPUDPReceiver> Receiver;

	UPROPERTY(Transient)
	TObjectPtr<AVPBroadcastOutput> BroadcastOutput;

	UPROPERTY(Transient)
	TObjectPtr<AVPAvatarManager> AvatarManager;

	UPROPERTY(Transient)
	TObjectPtr<UComboBoxString> AvatarSelector;

	UPROPERTY(Transient)
	TObjectPtr<UComboBoxString> InputCameraSelector;

	UPROPERTY(Transient)
	UTextBlock* AvatarStatusText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* GuidanceText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> GuidancePanel;

	UPROPERTY(Transient)
	UTextBlock* TrackingStatusText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* DiagnosticsText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* ProfileStatusText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* CalibrationStatusText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* InputCameraStatusText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* BroadcastStatusText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* BackgroundModeButtonText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* BackgroundColorUsageText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* PoseStatusText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<USlider> BackgroundRedSlider;

	UPROPERTY(Transient)
	TObjectPtr<USlider> BackgroundGreenSlider;

	UPROPERTY(Transient)
	TObjectPtr<USlider> BackgroundBlueSlider;

	UPROPERTY(Transient)
	TObjectPtr<USlider> AvatarExposureSlider;

	UPROPERTY(Transient)
	TObjectPtr<USlider> OutputFPSSlider;

	UPROPERTY(Transient)
	UTextBlock* BackgroundRedValueText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* BackgroundGreenValueText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* BackgroundBlueValueText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* BackgroundColorHexText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* AvatarExposureValueText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* OutputFPSValueText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> BackgroundColorSwatch;

	UPROPERTY(Transient)
	UTextBlock* CalibrationButtonText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* ValidationButtonText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* SwapButtonText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* LeftInvertButtonText = nullptr;

	UPROPERTY(Transient)
	UTextBlock* RightInvertButtonText = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UProgressBar> StageProgressBar;

	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> ControlPanel;

	UPROPERTY(Transient)
	TObjectPtr<UWidgetSwitcher> PaletteSwitcher;

	UPROPERTY(Transient)
	TObjectPtr<UButton> BroadcastPaletteButton;

	UPROPERTY(Transient)
	TObjectPtr<UButton> TrackingSettingsPaletteButton;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> ModalOverlay;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ModalTitleText;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ModalMessageText;

	UPROPERTY(Transient)
	TObjectPtr<UButton> ModalPrimaryButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ModalPrimaryButtonText;

	UPROPERTY(Transient)
	TObjectPtr<UButton> ModalSecondaryButton;

	float TargetSearchElapsedSeconds = 0.0f;
	float SecondsSinceLastPacket = 0.0f;
	float DisplayedPacketRate = 0.0f;
	double PacketRateWindowStartTimeSeconds = 0.0;
	double LastPacketRateUpdateTimeSeconds = 0.0;
	double LastPacketObservedTimeSeconds = 0.0;
	int32 LastObservedPacketCount = INDEX_NONE;
	int32 PacketRateWindowStartCount = 0;
	int32 ObservedAvatarLibraryRevision = INDEX_NONE;
	int32 ObservedAvatarNoticeRevision = 0;
	int32 ObservedInputCameraListRevision = INDEX_NONE;
	int32 ObservedInputCameraNoticeRevision = 0;
	bool bPacketRateReady = false;
	bool bDeleteConfirmationModal = false;
	bool bPanningCamera = false;
	bool bOrbitingCamera = false;
	bool bTrackingSettingsPaletteActive = false;
	bool bUpdatingBackgroundColorControls = false;
	bool bBackgroundColorCommitPending = false;
	bool bUpdatingAvatarExposureControl = false;
	bool bAvatarExposureCommitPending = false;
	bool bUpdatingOutputFPSControl = false;
	bool bOutputFPSCommitPending = false;
	float BackgroundColorCommitRemainingSeconds = 0.0f;
	float AvatarExposureCommitRemainingSeconds = 0.0f;
	float OutputFPSCommitRemainingSeconds = 0.0f;
	float GuidanceSuccessRemainingSeconds = 0.0f;
	float CalibrationFailureDisplayRemainingSeconds = 0.0f;
	EVPCalibrationState PreviousCalibrationState = EVPCalibrationState::Idle;
	EVPArmValidationStage PreviousValidationStage = EVPArmValidationStage::Idle;
	FVector2D LastCameraPointerPosition = FVector2D::ZeroVector;
	TMap<FString, FString> AvatarOptionToId;
	TMap<FString, FString> InputCameraOptionToId;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UWidget>> AvatarRequiredControls;

	void BuildLayout();
	void FindTrackingTargets();
	void UpdatePacketRate();
	void UpdateGuidanceVisibility(float DeltaSeconds);
	void UpdateBackgroundColorCommit(float DeltaSeconds);
	void UpdateAvatarExposureCommit(float DeltaSeconds);
	void UpdateOutputFPSCommit(float DeltaSeconds);
	void RefreshBackgroundColorControls(const FLinearColor& LinearColor);
	void RefreshAvatarExposureControl(float ExposureStops);
	void RefreshOutputFPSControl(int32 FramesPerSecond);
	void RefreshDashboard();
	void RefreshAvatarSelector();
	void RefreshInputCameraSelector();
	void SetDashboardEnabled(bool bEnabled);
	void SetTrackingSettingsPaletteActive(bool bActive);
	void ShowNoticeModal(
		const FString& Message,
		bool bIsError,
		const FString& TitleOverride = FString());
	void ShowDeleteConfirmationModal();
	void HideModal();
	UTextBlock* CreateText(const FString& Text, int32 FontSize, const FLinearColor& Color);
	UButton* CreateButton(
		UVerticalBox* Parent,
		const FString& Label,
		UTextBlock*& OutLabel,
		const FLinearColor& BackgroundColor);
	UButton* CreateInlineButton(
		UHorizontalBox* Parent,
		const FString& Label,
		const FLinearColor& BackgroundColor,
		UTextBlock*& OutLabel);

	UFUNCTION()
	void HandleCalibrationClicked();

	UFUNCTION()
	void HandleBroadcastPaletteClicked();

	UFUNCTION()
	void HandleTrackingSettingsPaletteClicked();

	UFUNCTION()
	void HandleValidationClicked();

	UFUNCTION()
	void HandleSaveProfileClicked();

	UFUNCTION()
	void HandleResetProfileClicked();

	UFUNCTION()
	void HandleSwapClicked();

	UFUNCTION()
	void HandleLeftInvertClicked();

	UFUNCTION()
	void HandleRightInvertClicked();

	UFUNCTION()
	void HandleBackgroundModeClicked();

	UFUNCTION()
	void HandleGreenBackgroundClicked();

	UFUNCTION()
	void HandleBlueBackgroundClicked();

	UFUNCTION()
	void HandleMagentaBackgroundClicked();

	UFUNCTION()
	void HandleBackgroundSliderChanged(float Value);

	UFUNCTION()
	void HandleAvatarExposureChanged(float Value);

	UFUNCTION()
	void HandleOutputFPSChanged(float Value);

	UFUNCTION()
	void HandleInputCameraSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);

	UFUNCTION()
	void HandleRefreshInputCamerasClicked();

	UFUNCTION()
	void HandleAddAvatarClicked();

	UFUNCTION()
	void HandleDeleteAvatarClicked();

	UFUNCTION()
	void HandleModalPrimaryClicked();

	UFUNCTION()
	void HandleModalSecondaryClicked();

	UFUNCTION()
	void HandleResetCameraClicked();

	UFUNCTION()
	void HandleAvatarSelectionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
};
